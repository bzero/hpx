//  Copyright (c) 2007-2010 Hartmut Kaiser
// 
//  Part of this code has been adopted from code published under the BSL by:
//
//  (C) Copyright 2005-7 Anthony Williams 
//  (C) Copyright 2007 David Deakins 
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying 
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#if !defined(HPX_LCOS_MUTEX_JUN_23_2008_0530PM)
#define HPX_LCOS_MUTEX_JUN_23_2008_0530PM

#include <hpx/hpx_fwd.hpp>
#include <hpx/util/spinlock_pool.hpp>
#include <hpx/util/unlock_lock.hpp>

#include <boost/atomic.hpp>
#include <boost/noncopyable.hpp>
#include <boost/intrusive/list.hpp>
#include <boost/thread/locks.hpp>

// Description of the mutex algorithm is explained here:
// http://lists.boost.org/Archives/boost/2006/09/110367.php
//
// The algorithm is: 
// 
// init(): 
//    active_count=0; 
//    no semaphore 
// 
// lock(): 
//    atomic increment active_count 
//    if new active_count ==1, that's us, so we've got the lock 
//    else 
//         get semaphore, and wait 
//         now we've got the lock 
// 
// unlock(): 
//    atomic decrement active_count 
//    if new active_count >0, then other threads are waiting, 
//        so release semaphore. 
// 
// locked(): 
//    return active_count>0 
// 
// get_semaphore(): 
//    if there's already a semaphore associated with this mutex, return that 
//    else 
//        create new semaphore. 
//        use atomic compare-and-swap to make this the associated semaphore if 
//            none 
//        if another thread beat us to it, and already set a semaphore, destroy 
//            new one, and return already-set one 
//        else return the new semaphore 

///////////////////////////////////////////////////////////////////////////////
namespace hpx { namespace lcos { namespace detail
{
    template <typename T>
    inline bool interlocked_bit_test_and_set(boost::atomic<T>& x, long bit)
    {
        T const value = 1u << bit;
        boost::uint32_t old = x.load(boost::memory_order_acquire);
        do {
            boost::uint32_t tmp = old;
            if (x.compare_exchange_strong(tmp, T(old | value)))
                break;
            old = tmp;
        } while(true);
        return (old & value) != 0;
    }

    // A mutex can be used to synchronize the access to an arbitrary resource
    class mutex 
    {
    private:
        BOOST_STATIC_CONSTANT(boost::uint32_t, lock_flag_bit = 31);
        BOOST_STATIC_CONSTANT(boost::uint32_t, lock_flag_value = 1u << lock_flag_bit);

    private:
        struct tag {};
        typedef hpx::util::spinlock_pool<tag> mutex_type;

        // define data structures needed for intrusive slist container used for
        // the queues
        struct queue_entry
        {
            typedef boost::intrusive::list_member_hook<
                boost::intrusive::link_mode<boost::intrusive::auto_unlink>
            > hook_type;

            queue_entry(threads::thread_id_type id)
              : id_(id)
            {}

            threads::thread_id_type id_;
            hook_type list_hook_;
        };

        typedef boost::intrusive::member_hook<
            queue_entry, queue_entry::hook_type,
            &queue_entry::list_hook_
        > list_option_type;

        typedef boost::intrusive::list<
            queue_entry, list_option_type, 
            boost::intrusive::constant_time_size<false>
        > queue_type;

    public:
        mutex(char const* const description)
          : active_count_(0), description_(description)
        {}

        ~mutex()
        {
            if (!queue_.empty() && LHPX_ENABLED(fatal)) {
                LERR_(fatal) << "~mutex: " << description_ 
                             << ": queue is not empty";

                mutex_type::scoped_lock l(this);
                while (!queue_.empty()) {
                    threads::thread_id_type id = queue_.front().id_;
                    queue_.pop_front();

                    // we know that the id is actually the pointer to the thread
                    threads::thread* thrd = reinterpret_cast<threads::thread*>(id);
                    LERR_(fatal) << "~mutex: " << description_
                            << ": pending thread: " 
                            << get_thread_state_name(thrd->get_state()) 
                            << "(" << id << "): " << thrd->get_description();
                }
            }
            BOOST_ASSERT(queue_.empty());
        }

        bool try_lock()
        {
            return !interlocked_bit_test_and_set(active_count_, lock_flag_bit);
        }

        void mark_waiting_and_try_lock(boost::uint32_t& old_count)
        {
            for(;;) 
            {
                boost::uint32_t new_count = (old_count & lock_flag_value) ? 
                    (old_count + 1) : (old_count | lock_flag_value);

                boost::uint32_t tmp = old_count;
                if (active_count_.compare_exchange_strong(tmp, new_count))
                    break;
                old_count = tmp;
            }
        }

        void clear_waiting_and_try_lock(boost::uint32_t& old_count)
        {
            old_count &= ~lock_flag_value;
            for(;;) 
            {
                boost::uint32_t new_count = (old_count & lock_flag_value) ? 
                    old_count : ((old_count-1) | lock_flag_value); 

                boost::uint32_t tmp = old_count;
                if (active_count_.compare_exchange_strong(tmp, new_count))
                    break;
                old_count = tmp;
            }
        }

        bool wait_for_single_object(
            ::boost::system_time const& wait_until = ::boost::system_time())
        {
            threads::thread_self& self = threads::get_self();
            threads::thread_id_type id = self.get_thread_id();

            // enqueue this thread
            mutex_type::scoped_lock l(this);

            queue_entry e(id);
            queue_.push_back(e);

            util::unlock_the_lock<mutex_type::scoped_lock> ul(l);

            // timeout at the given time, if appropriate
            if (!wait_until.is_not_a_date_time()) 
                threads::set_thread_state(id, wait_until);

            // if this timed out, return true
            return threads::wait_timeout == self.yield(threads::suspended);
            // queue_entry goes out of scope (removes itself 
            // from the list, re-acquiring the unlocked mutex before 
            // doing so)
        }

        void set_event()
        {
            mutex_type::scoped_lock l(this);
            if (!queue_.empty()) {
                threads::thread_id_type id = queue_.front().id_;
                queue_.pop_front();

                l.unlock();
                set_thread_state(id, threads::pending);
            }
        }

        void lock()
        {
            using namespace boost::lockfree;
            if (try_lock())
                return;

            boost::uint32_t old_count = 
                active_count_.load(boost::memory_order_acquire);
            mark_waiting_and_try_lock(old_count);

            if (old_count & lock_flag_value)
            {
                // wait for lock to get available
                bool lock_acquired = false;
                do {
                    BOOST_VERIFY(!wait_for_single_object());
                    clear_waiting_and_try_lock(old_count);
                    lock_acquired = !(old_count & lock_flag_value);
                } while (!lock_acquired);
            }
        }

        bool timed_lock(::boost::system_time const& wait_until)
        {
            using namespace boost::lockfree;
            if (try_lock())
                return true;

            boost::uint32_t old_count = 
                active_count_.load(boost::memory_order_acquire);
            mark_waiting_and_try_lock(old_count);

            if (old_count & lock_flag_value)
            {
                // wait for lock to get available
                bool lock_acquired = false;
                do {
                    if (wait_for_single_object()) 
                    {
                        // if this timed out, just return false
                        --active_count_;
                        return false;
                    }
                    clear_waiting_and_try_lock(old_count);
                    lock_acquired = !(old_count & lock_flag_value);
                } while (!lock_acquired);
            }
            return true;
        }

        template<typename Duration>
        bool timed_lock(Duration const& timeout)
        {
            return timed_lock(boost::get_system_time()+timeout);
        }

        bool timed_lock(boost::xtime const& timeout)
        {
            return timed_lock(boost::posix_time::ptime(timeout));
        }

        void unlock()
        {
            BOOST_ASSERT(active_count_ & lock_flag_value);
            active_count_ += lock_flag_value;
            set_event();
        }

        typedef boost::unique_lock<mutex> scoped_lock;
        typedef boost::detail::try_lock_wrapper<mutex> scoped_try_lock;

    private:
        boost::atomic_uint32_t active_count_;
        queue_type queue_;
        char const* const description_;
    };

}}}

///////////////////////////////////////////////////////////////////////////////
namespace hpx { namespace lcos 
{
    ///////////////////////////////////////////////////////////////////////////
    class mutex : boost::noncopyable, public detail::mutex
    {
    public:
        mutex(char const* const description = "")
          : detail::mutex(description)
        {}
        ~mutex()
        {}

        typedef boost::unique_lock<mutex> scoped_lock;
        typedef boost::detail::try_lock_wrapper<mutex> scoped_try_lock;
    };

    typedef mutex try_mutex;

    ///////////////////////////////////////////////////////////////////////////
    class timed_mutex : boost::noncopyable, public detail::mutex
    {
    public:
        timed_mutex(char const* const description = "")
          : detail::mutex(description)
        {}
        ~timed_mutex()
        {}

        typedef boost::unique_lock<timed_mutex> scoped_timed_lock;
        typedef boost::detail::try_lock_wrapper<timed_mutex> scoped_try_lock;
        typedef scoped_timed_lock scoped_lock;
    };

}}

#endif

