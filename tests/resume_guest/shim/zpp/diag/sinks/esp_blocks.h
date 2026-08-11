#pragma once
// Whether the block sink wants the controller polled again. The harness
// defines `ready`, and the resume path's answer to it is one of the two
// counters this harness watches.
#include "zpp/diag/config.h"

namespace zpp::diag
{
struct esp_block_sink
{
    static bool ready();
};

} // namespace zpp::diag
