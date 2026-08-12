/*
 * What KVM offers a nested guest, read from KVM itself.
 *
 * The rig boots this Windows installation under KVM and does not boot it
 * under this VMM, so the difference is something presented rather than
 * something done wrong. This asks the other side of that comparison
 * directly: KVM_GET_MSRS on /dev/kvm answers the VMX capability MSRs a
 * guest hypervisor would read, which is exactly the menu Hyper-V
 * configures itself from.
 *
 * Freestanding with raw syscalls because the rig has no compiler and no
 * libc to link against - it runs from RAM and carries neither. Built by
 * scripts/kvm-nested-msrs.sh, which cross-compiles it from the Mac.
 */

typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long s64;

static s64 syscall3(s64 n, s64 a, s64 b, s64 c)
{
    s64 result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return result;
}

struct msr_entry
{
    u32 index;
    u32 reserved;
    u64 data;
};

struct msrs
{
    u32 count;
    u32 pad;
    struct msr_entry entries[32];
};

static char out[4096];
static int used;

static void put(const char *s)
{
    while (*s) {
        out[used++] = *s++;
    }
}

static void put_hex(u64 value, int digits)
{
    static const char table[] = "0123456789abcdef";
    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4) {
        out[used++] = table[(value >> shift) & 0xf];
    }
}

void _start(void)
{
    /* The names are the SDM's, so the output can be read beside it. */
    static const char *const names[] = {
        "BASIC", "PINBASED_CTLS", "PROCBASED_CTLS", "EXIT_CTLS",
        "ENTRY_CTLS", "MISC", "CR0_FIXED0", "CR0_FIXED1",
        "CR4_FIXED0", "CR4_FIXED1", "VMCS_ENUM", "PROCBASED_CTLS2",
        "EPT_VPID_CAP", "TRUE_PINBASED_CTLS", "TRUE_PROCBASED_CTLS",
        "TRUE_EXIT_CTLS", "TRUE_ENTRY_CTLS", "VMFUNC",
    };
    const int count = 18;

    long fd = syscall3(2, (s64) "/dev/kvm", 2 /* O_RDWR */, 0);
    if (fd < 0) {
        put("cannot open /dev/kvm\n");
        syscall3(1, 1, (s64)out, used);
        syscall3(231, 1, 0, 0);
    }

    static struct msrs request;
    request.count = count;
    for (int i = 0; i < count; ++i) {
        request.entries[i].index = 0x480 + i;
    }

    /* KVM_GET_MSRS on the system fd answers the feature MSRs, which is
     * what the capability MSRs are - no VM and no vCPU needed. */
    long got = syscall3(16, fd, 0xc008ae88ull, (s64)&request);
    if (got < 0) {
        put("KVM_GET_MSRS failed, errno ");
        put_hex((u64)(-got), 4);
        put("\n");
        syscall3(1, 1, (s64)out, used);
        syscall3(231, 1, 0, 0);
    }

    for (int i = 0; i < (int)got; ++i) {
        put_hex(request.entries[i].index, 3);
        put(" ");
        put_hex(request.entries[i].data, 16);
        put(" ");
        put(names[i]);
        put("\n");
    }
    syscall3(1, 1, (s64)out, used);
    syscall3(231, 0, 0, 0);
}
