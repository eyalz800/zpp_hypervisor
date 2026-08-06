#pragma once
#include "zpp/diag/sink.h"
#include "zpp/diag/sinks/counter.h"
#include "zpp/diag/sinks/esp_blocks.h"

namespace zpp::diag
{
/**
 * The channels the resident hypervisor can drive.
 *
 * One of these files per program, found by the same include spelling from
 * pump.h, because a sink type can only exist where its channel does: the
 * EFI system partition as a file and the firmware console need boot
 * services, which are gone by the time this code is resident. The
 * alternative - one list with platform conditionals in it - would put
 * `#if` back into the design at the one place the configuration is
 * supposed to be readable.
 *
 * Order is delivery order within one pump pass, so the cheapest and most
 * reliable channel goes first. It matters only when a pass is cut short.
 *
 * What goes here as each lands:
 *
 *     using sinks = sink_list<counter_sink,
 *                             framebuffer_sink,   // screen.h, halt paths
 *                             usb_debug_sink,
 *                             esp_block_sink,     // raw blocks, resident
 *                             serial_sink>;
 *
 * Enabling one in zpp/diag/config.h without adding it here is a build
 * failure - see the static_assert at the bottom of pump.h.
 */
using sinks = sink_list<counter_sink, esp_block_sink>;

} // namespace zpp::diag
