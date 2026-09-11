#include "zpp/arch/x86_64/vmx/vmcs.h"
#include <print>
#include <string_view>

namespace vx = zpp::arch::x86_64::vmx;
using field = vx::vmcs::field;

namespace
{
std::size_t checks{};
std::size_t failures{};

void check(bool condition, std::string_view what)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::println("FAIL: {}", what);
    }
}

void select(std::size_t cpu, std::uint64_t region)
{
    zpp::arch::x86_64::g_gs_qword = vx::vmcs_cache_token_magic | cpu;
    check(0 == vx::vmptrld(&region, cpu), "select modeled CPU's VMCS");
}

std::uint64_t & hardware(field encoding)
{
    return vx::g_vmcs_loaded[static_cast<std::uint64_t>(encoding)];
}

void writes_during_another_cpus_borrow()
{
    vx::vmcs vmcs;
    for (auto encoding :
         {field::guest_rip, field::guest_rsp, field::guest_cr3}) {
        select(0, 0x1000);
        vmcs.write(encoding, 0x1111);
        check(vmcs.read(encoding) == 0x1111, "CPU 0 cached initial value");
        select(1, 0x2000);
        vmcs.write(encoding, 0x2222);
        {
            // An early return from the shadow-copy scope need not change
            // the VMCS pointer or epoch. Interleave CPU 0's write before
            // CPU 1 leaves that scope; no real threads are needed.
            vx::vmcs_cache_borrow borrow;
            select(0, 0x1000);
            vmcs.write(encoding, 0x3333);
            check(hardware(encoding) == 0x3333,
                  "write reached CPU 0 VMCS");
            check(vmcs.read(encoding) == 0x3333,
                  "read during borrow sees the new value");
            select(1, 0x2000);
        }
        select(0, 0x1000);
        check(vmcs.read(encoding) == 0x3333,
              "read after foreign borrow cannot revive the old value");
        select(1, 0x2000);
        check(vmcs.read(encoding) == 0x2222,
              "other CPU's VMCS retains its own field");
    }
}

void borrowed_shadow_reads_never_fill_the_cache()
{
    vx::vmcs vmcs;
    select(0, 0x1000);
    vmcs.write(field::guest_rip, 0x4444);
    {
        vx::vmcs_cache_borrow borrow;
        select(0, 0x3000);
        hardware(field::guest_rip) = 0x5555;
        check(vmcs.read(field::guest_rip) == 0x5555, "first shadow read");
        hardware(field::guest_rip) = 0x6666;
        check(vmcs.read(field::guest_rip) == 0x6666,
              "shadow changes are read again during the borrow");
        std::uint64_t shadow = 0x3000;
        check(0 == vx::vmclear(&shadow), "clear borrowed shadow");
        select(0, 0x1000);
    }
    check(vmcs.read(field::guest_rip) == 0x4444,
          "shadow values never replace the original VMCS's values");
}

void exit_invalidates_only_the_current_vmcs()
{
    vx::vmcs vmcs;
    select(0, 0x1000);
    vmcs.write(field::guest_rip, 0x7777);
    select(0, 0x3000);
    vmcs.write(field::guest_rip, 0x8888);
    hardware(field::guest_rip) = 0x9999;
    vx::vmcs_cache_forget_current(0);
    check(vmcs.read(field::guest_rip) == 0x9999,
          "VM exit's hardware update replaces the cached guest RIP");
    auto reads =
        vx::g_vmread_field_count[static_cast<unsigned>(field::guest_rip)];
    select(0, 0x1000);
    check(vmcs.read(field::guest_rip) == 0x7777,
          "inactive VMCS keeps its independent state");
    check(vx::g_vmread_field_count[static_cast<unsigned>(
              field::guest_rip)] == reads,
          "VM exit does not discard another VMCS's cached state");
}
} // namespace

int main()
{
    static_assert(vx::vmcs_cache_enabled);
    writes_during_another_cpus_borrow();
    borrowed_shadow_reads_never_fill_the_cache();
    exit_invalidates_only_the_current_vmcs();
    std::println("vmcs_cache: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
