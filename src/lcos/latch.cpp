//  Copyright (c) 2007-2015 Hartmut Kaiser
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/hpx_fwd.hpp>
#include <hpx/runtime/components/derived_component_factory.hpp>
#include <hpx/runtime/actions/continuation.hpp>
#include <hpx/runtime/components/runtime_support.hpp>
#include <hpx/lcos/server/latch.hpp>
#include <hpx/util/serialize_exception.hpp>

///////////////////////////////////////////////////////////////////////////////
// latch
typedef hpx::components::managed_component<hpx::lcos::server::latch> latch_type;

HPX_DEFINE_GET_COMPONENT_TYPE_STATIC(
    hpx::lcos::server::latch, hpx::components::component_latch)
HPX_REGISTER_DERIVED_COMPONENT_FACTORY(latch_type, hpx_lcos_server_latch,
    "hpx::lcos::base_lco_with_value", hpx::components::factory_enabled)

HPX_REGISTER_ACTION(
    hpx::lcos::server::latch::create_component_action,
    hpx_lcos_server_latch_create_component_action)
HPX_REGISTER_ACTION(
    hpx::lcos::server::latch::wait_action,
    hpx_lcos_server_latch_wait_action)

