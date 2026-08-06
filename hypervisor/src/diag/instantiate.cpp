#include "zpp/diag/pump.h"

/**
 * Makes the compiler check the resident side's sinks.
 *
 * Nothing in the hypervisor includes the diagnostic facility yet - the
 * pump is not called from the exit handler, and the disk sink is not
 * wired to a loader hand-over. Without this file the sinks would
 * therefore never be compiled by the freestanding toolchain at all, only
 * by whatever a developer happened to point at them, and that is exactly
 * the rot config.h warns about: "code behind #ifdef rots silently and
 * fails the day someone flips the switch".
 *
 * An explicit instantiation rather than a call, because a call would mean
 * wiring the pump into a path that runs, and the sinks are not ready to
 * run. Instantiating the template is what forces every member to be
 * compiled, which is the whole point - a header that is merely included
 * has its templates parsed but not checked.
 *
 * `#if` rather than `if constexpr`, which is against this tree's usual
 * rule and is unavoidable here: an explicit instantiation is a
 * declaration and cannot appear inside a discarded statement. The guard
 * is what keeps the storage - eight kilobytes of queues and two four
 * kilobyte buffers - out of a release build, and therefore what keeps
 * `scripts/ci/check-diag-absent.sh` passing and the two release binaries
 * byte for byte identical.
 */
#if ZPP_DIAG
template struct zpp::diag::esp_blocks_for<zpp::diag::sink::esp_blocks>;
template struct zpp::diag::counter_for<zpp::diag::sink::counter>;
#endif
