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

void clearing_a_private_shadow_preserves_unrelated_rows()
{
    vx::vmcs vmcs;
    select(0, 0x1000);
    vmcs.write(field::guest_rip, 0xaaaa);
    select(1, 0x2000);
    vmcs.write(field::guest_rip, 0xbbbb);
    select(0, 0x3000);
    vmcs.write(field::guest_rip, 0xcccc);
    select(0, 0x1000);
    {
        vx::vmcs_cache_borrow borrow;
        select(0, 0x3000);
        // Model a shadow VMCS changed by the guest's VMWRITE. Its old
        // cached value must be discarded, even though other VMCSs survive.
        hardware(field::guest_rip) = 0xdddd;
        std::uint64_t shadow = 0x3000;
        check(0 == vx::vmclear_owned(&shadow, 0), "clear private shadow");
        select(0, 0x1000);
    }
    auto & reads =
        vx::g_vmread_field_count[static_cast<unsigned>(field::guest_rip)];
    auto before = reads;
    check(vmcs.read(field::guest_rip) == 0xaaaa,
          "original VMCS retains its value after shadow clear");
    check(reads == before, "shadow clear preserves original VMCS cache");
    select(1, 0x2000);
    before = reads;
    check(vmcs.read(field::guest_rip) == 0xbbbb,
          "other CPU retains its value after shadow clear");
    check(reads == before, "shadow clear preserves other CPU's cache");
    select(0, 0x3000);
    check(vmcs.read(field::guest_rip) == 0xdddd,
          "cleared shadow cannot reuse its old cached value");
}

void zero_physical_clear_preserves_enlightened_selection()
{
    select(0, 0x1000);
    vx::vmcs_cache_select_enlightened(0x4000, 0);
    std::uint64_t physical{};
    check(0 == vx::vmclear_owned(&physical, 0),
          "clear physical page zero");
    check(vx::vmcs_cache_current_enlightened() == 0x4000,
          "clearing physical page zero does not select a hardware VMCS");
}

void access_rights_reads_follow_the_processors_reserved_bit_behavior()
{
    vx::vmcs vmcs;
    for (auto mask : {0xffffffffu, 0x1f0ffu}) {
        vx::g_vmwrite_access_rights_mask = mask;
        for (auto encoding : {field::guest_es_access_rights,
                              field::guest_cs_access_rights,
                              field::guest_tr_access_rights}) {
            select(0, 0x1000);
            vmcs.write(encoding, 0xffffffffu);
            check(hardware(encoding) == mask,
                  "modeled processor applied its access-rights mask");
            check(
                vmcs.read(encoding) == hardware(encoding),
                "cached access rights match the processor after VMWRITE");
            auto observed_reads =
                vx::g_vmread_field_count[static_cast<unsigned>(encoding)];
            check(vmcs.read(encoding) == mask,
                  "repeated access-rights read keeps the observed value");
            check(vx::g_vmread_field_count[static_cast<unsigned>(
                      encoding)] == observed_reads,
                  "the processor's access-rights value can be cached");
            vmcs.write(encoding, 0x10000);
            auto before =
                vx::g_vmread_field_count[static_cast<unsigned>(encoding)];
            check(vmcs.read(encoding) == 0x10000,
                  "defined unusable-segment bit is retained");
            check(vx::g_vmread_field_count[static_cast<unsigned>(
                      encoding)] == before,
                  "writes without reserved bits still fill the cache");
        }
        vmcs.write(field::guest_gdtr_limit, 0xffffffffu);
        check(vmcs.read(field::guest_gdtr_limit) == 0xffffffffu,
              "other 32-bit fields retain their full width");
    }
    vx::g_vmwrite_access_rights_mask = 0xffffffffu;
}
void repeated_writes_need_a_current_observation_of_the_same_field()
{
    vx::vmcs vmcs;
    for (auto encoding :
         {field::guest_rip, field::guest_rsp, field::host_rsp}) {
        select(0, 0x1000);
        vx::vmcs_cache_forget_current(0);
        vmcs.write(encoding, 0x1111);
        auto & writes =
            vx::g_vmwrite_field_count[static_cast<unsigned>(encoding)];
        auto before = writes;
        vmcs.write(encoding, 0x1111);
        check(hardware(encoding) == 0x1111,
              "repeated write retains value");
        check(writes == before, "known identical write avoids VMWRITE");
        before = writes;
        vmcs.write(encoding, 0x2222);
        check(hardware(encoding) == 0x2222, "changed write reaches VMCS");
        check(writes == before + 1, "changed write executes VMWRITE");

        hardware(encoding) = 0x3333;
        vx::vmcs_cache_forget_current(0);
        check(vmcs.read(encoding) == 0x3333,
              "read observes hardware update");
        before = writes;
        vmcs.write(encoding, 0x3333);
        check(hardware(encoding) == 0x3333,
              "read-then-write retains value");
        check(writes == before, "observed identical value avoids VMWRITE");

        hardware(encoding) = 0x4444;
        vx::vmcs_cache_forget_current(0);
        before = writes;
        vmcs.write(encoding, 0x3333);
        check(hardware(encoding) == 0x3333,
              "a VM exit prevents reuse of the previous observation");
        check(writes == before + 1,
              "write after VM exit reaches hardware");
        {
            vx::vmcs_cache_borrow borrow;
            before = writes;
            vmcs.write(encoding, 0x3333);
            check(writes == before + 1,
                  "writes during a borrow bypass any cached equality");
        }
    }

    // Full and high-half encodings share a slot but are different writes.
    vmcs.write(field::vmcs_link_pointer, 0x22);
    auto high = static_cast<field>(
        static_cast<unsigned>(field::vmcs_link_pointer) | 1);
    auto before = vx::g_vmwrite_field_count[static_cast<unsigned>(high)];
    vmcs.write(high, 0x22);
    check(vx::g_vmwrite_field_count[static_cast<unsigned>(high)] ==
              before + 1,
          "equal values in full/high aliases do not suppress a write");
}

void cached_read_only_fields_must_still_report_write_failure()
{
    vx::vmcs vmcs;
    select(0, 0x1000);
    vx::vmcs_cache_forget_current(0);
    constexpr auto encoding = static_cast<field>(0x4402); // VM-exit reason
    hardware(encoding) = 0x12;
    check(vmcs.read(encoding) == 0x12, "cache a readable VM-exit field");
    vx::g_vmwrite_readonly_allowed = false;
    auto failures_before = vx::vmcs_write_failures;
    vmcs.write(encoding, 0x12);
    check(vx::vmcs_write_failures == failures_before + 1,
          "same value cannot hide VMWRITE to a read-only field");
    check(hardware(encoding) == 0x12, "refused write preserves hardware");
    vx::g_vmwrite_readonly_allowed = true;
}
} // namespace

int main()
{
    static_assert(vx::vmcs_cache_enabled);
    writes_during_another_cpus_borrow();
    borrowed_shadow_reads_never_fill_the_cache();
    exit_invalidates_only_the_current_vmcs();
    clearing_a_private_shadow_preserves_unrelated_rows();
    zero_physical_clear_preserves_enlightened_selection();
    access_rights_reads_follow_the_processors_reserved_bit_behavior();
    repeated_writes_need_a_current_observation_of_the_same_field();
    cached_read_only_fields_must_still_report_write_failure();
    std::println("vmcs_cache: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
