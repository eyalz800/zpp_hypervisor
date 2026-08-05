#pragma once
#include "zpp/diag/sink.h"
#include "zpp/diag/sinks/counter.h"

namespace zpp::diag
{
/**
 * The channels this loader can drive.
 *
 * The loader's list and the hypervisor's are different files with the same
 * include spelling, because a sink type can only exist where its channel
 * does. Everything here needs boot services or the firmware's own console,
 * and both are gone by the time the hypervisor is resident - so the split
 * is not a preference, it is what the platform allows. The alternative,
 * one list with `#if` in it, would put conditionals back into the one part
 * of this design that is supposed to be a readable table.
 *
 * What goes here as each lands:
 *
 *     using sinks = sink_list<counter_sink,
 *                             firmware_console_sink,
 *                             serial_sink,
 *                             esp_file_sink>;
 *
 * Note what this loader already has that is worth folding in rather than
 * keeping beside: zpp/trace.h's own 0x8000 byte accumulator, its firmware
 * console mirror and its UART writer are three sinks and a ring, written
 * before there was anything to share them with. Once they read out of the
 * shared ring instead, the loader's trace and the hypervisor's records are
 * one stream with one numbering, and the file the ESP sink writes contains
 * both sides of the hand-over.
 */
using sinks = sink_list<counter_sink>;

} // namespace zpp::diag
