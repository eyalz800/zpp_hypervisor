#pragma once
#include "zpp/hypervisor.h"
#include <atomic>
#include <new>

namespace zpp::hypervisor
{
/**
 * The global state object that holds the hypervisor.
 */
extern struct state
{
    /**
     * Create the global hypervisor state object object.
     */
    static void create_once();

    /**
     * The hypervisor object.
     */
    zpp::hypervisor::hypervisor hypervisor;
} & g_state;

} // namespace zpp::hypervisor