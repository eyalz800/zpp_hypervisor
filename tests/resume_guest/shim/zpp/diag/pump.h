#pragma once
// The trickle out of the ring, counted so a test can say the resume path
// reached the end rather than only that it wrote the right field. The
// harness defines `run`.
#include "zpp/diag/config.h"

namespace zpp::diag
{
struct pump
{
    static void run();
};

} // namespace zpp::diag
