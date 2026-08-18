#include "zpp/hypervisor/nested_vmx.h"

namespace zpp::hypervisor
{
namespace
{
constexpr char digit(bool value)
{
    return value ? '1' : '0';
}

constexpr char digit(unsigned value)
{
    return static_cast<char>('0' + (value % 10));
}
} // namespace

/**
 * What the *compiler* saw, spelled into `.rodata` so that `strings` on the
 * built hypervisor - or on the loader that embeds it - answers "is this
 * switch actually in the binary" without a boot and without a debugger.
 *
 * **This exists because a CMake cache reading `ON` is not evidence.**
 * `ZPP_PUBLISH_REFERENCE_TSC:BOOL=ON` sat in both caches and in
 * `compile_commands.json` while the object file that consumed it was
 * stale, so `publish_reference_tsc_page` was linked in as a bare `ret`
 * and every measurement taken against that binary - two sessions of them -
 * was of a configuration nobody had built. The switch controlled the
 * single largest exit reason on the machine at the time.
 *
 * Built from the `constexpr bool`s the code branches on rather than from
 * the macros behind them, deliberately: a manifest assembled from `-D`
 * flags would have agreed with the cache and been just as wrong. These are
 * the same constants `if constexpr` is evaluated against, in a translation
 * unit that includes the same header, so a value here that disagrees with
 * behaviour means the *link* is inconsistent - which is the only failure
 * left once the compile is proven.
 *
 * `gnu::used` keeps the compiler from dropping it and `gnu::retain` marks
 * the section `SHF_GNU_RETAIN`, which is what survives `--gc-sections`;
 * `check-invariants.sh` builds with both.
 *
 * The one-second check, which is the whole point:
 *
 * ```sh
 * strings out/debug/x86_64/zpp_hypervisor | grep 'zpp switches'
 * ```
 */
extern "C" [[gnu::used, gnu::retain]] constinit const char
    zpp_build_switches[] = {
        'z', 'p', 'p', ' ', 's', 'w', 'i', 't', 'c', 'h', 'e', 's', ':',
        ' ', 'n', 'e', 's', 't', 'e', 'd', '=',
        digit(nested_vmx::enabled),
        ' ', 'e', 'v', 'm', 'c', 's', '=',
        digit(nested_vmx::evmcs_offered),
        ' ', 's', 'h', 'a', 'd', 'o', 'w', 'v', 'm', 'c', 's', '=',
        digit(nested_vmx::shadow_vmcs_enabled),
        ' ', 't', 'p', 'r', '=',
        digit(nested_vmx::tpr_shadow_offered),
        ' ', 'r', 'e', 'f', 't', 's', 'c', '=',
        digit(nested_vmx::publish_reference_tsc),
        ' ', 's', 'e', 'l', 'f', 'i', 'p', 'i', '=',
        digit(nested_vmx::deliver_self_ipi),
        ' ', 'd', 'e', 'f', 'e', 'r', '=',
        digit(nested_vmx::defer_guest_state),
        ' ', 's', 'h', 'a', 'd', 'o', 'w', 'g', 's', '=',
        digit(nested_vmx::shadow_guest_state),
        ' ', 's', 't', 'e', 'p', 'v', 't', 'l', '=',
        digit(nested_vmx::step_vtl),
        ' ', 'p', 'r', 'o', 'f', 'i', 'l', 'e', '=',
        digit(nested_vmx::profile_l2),
        ' ', 's', 't', 'r', 'e', 't', 'c', 'h', '=',
        digit(static_cast<unsigned>(ZPP_STRETCH_GUEST_TIMER)),
        // Two digits, where every switch above needs one: the values
        // worth running are 8 and 16, and one digit would print 16 as
        // `6` - a manifest that disagrees with the build is worse than
        // no manifest, which is the whole argument for this array.
        ' ', 'v', 't', 'l', 'c', 'a', 'p', '=',
        digit(nested_vmx::capture_vtl_deeply),
        ' ', 'd', 'i', 'l', 'a', 't', 'e', '=',
        digit(static_cast<unsigned>(nested_vmx::time_dilation / 10)),
        digit(static_cast<unsigned>(nested_vmx::time_dilation)),
        '\0'};

} // namespace zpp::hypervisor
