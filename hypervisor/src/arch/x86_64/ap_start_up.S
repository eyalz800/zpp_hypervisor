/*
 * The code an application processor begins executing when this VMM starts
 * it, and the only part of this project that runs outside long mode.
 *
 * A start-up IPI leaves its target in real mode at CS:IP =
 * (vector << 8):0, so the first instruction below runs sixteen bit with no
 * page tables, no stack and no descriptor tables worth the name. Its whole
 * job is to climb to long mode and hand over to C++.
 *
 * The blob is copied into a page below one megabyte, because a start-up
 * IPI vector is a page number and therefore cannot name anything higher.
 * It is copied rather than linked there, so it has to be position
 * independent, and it is: the assembler stores the two far pointers and
 * the GDT pointer as offsets from the start of the blob, and the sixteen
 * bit code turns each into a linear address by adding the page's own base.
 * Nothing here carries a relocation, and no instruction operand is patched
 * from outside - only the data area is written to before the blob is used.
 *
 * Real mode hands that base over for free. CS holds the vector shifted up
 * by eight, which is the page's linear base shifted down by four, so EBX
 * is loaded once at the top and every reference below goes through it.
 *
 * Every offset is a plain number rather than a difference between two
 * labels, because the assembler refuses more than one symbol in a memory
 * operand. The data area therefore has a fixed layout, mirrored by
 * ap_start_up_area in zpp/arch/x86_64/ap_start_up.h; the assertions at the
 * end are what keep the two from drifting apart silently.
 */
    .intel_syntax noprefix

    // Selectors into the GDT at the end of the blob.
    .set ap_code32_selector, 0x08
    .set ap_data_selector, 0x10
    .set ap_code64_selector, 0x18

    // The data area, and every field in it, as offsets from the start of
    // the blob. Kept well clear of the code, which is nowhere near this
    // long, so that the two can be reasoned about separately.
    .set ap_data_offset, 0xf00
    .set ap_host_cr3_offset, ap_data_offset + 0x00
    .set ap_host_cr0_offset, ap_data_offset + 0x08
    .set ap_host_cr4_offset, ap_data_offset + 0x10
    .set ap_entry_offset, ap_data_offset + 0x18
    .set ap_stack_top_offset, ap_data_offset + 0x20
    .set ap_argument_offset, ap_data_offset + 0x28
    .set ap_temporary_cr3_offset, ap_data_offset + 0x30
    .set ap_gdt_pointer_offset, ap_data_offset + 0x40
    .set ap_far_pointer32_offset, ap_data_offset + 0x48
    .set ap_far_pointer64_offset, ap_data_offset + 0x50
    .set ap_gdt_offset, ap_data_offset + 0x58

    .section .text.ap_start_up, "ax", @progbits
    .globl zpp_ap_start_up_begin
    .globl zpp_ap_start_up_end
    .balign 16

zpp_ap_start_up_begin:
    .code16

    // Interrupts are already masked coming out of a start-up IPI, but the
    // IDT is whatever happens to be at linear zero and must not be used.
    cli
    cld

    // Reach the data area with a plain displacement while still in real
    // mode, by pointing DS at this page the same way CS already is.
    mov ax, cs
    mov ds, ax

    // EBX = this page's linear base, from CS as described above. Held for
    // the rest of the climb, including across the switch to thirty-two and
    // then sixty-four bit code - a thirty-two bit write zeroes the upper
    // half, so RBX is already correct by the time long mode reads it.
    movzx ebx, ax
    shl ebx, 4

    // Relocate the GDT pointer's base and the two far pointers' offsets,
    // each of which the assembler left as an offset from the start of the
    // blob. These are the only absolute addresses it contains.
    mov eax, [ap_gdt_pointer_offset + 2]
    add eax, ebx
    mov [ap_gdt_pointer_offset + 2], eax
    mov eax, [ap_far_pointer32_offset]
    add eax, ebx
    mov [ap_far_pointer32_offset], eax
    mov eax, [ap_far_pointer64_offset]
    add eax, ebx
    mov [ap_far_pointer64_offset], eax

    lgdt [ap_gdt_pointer_offset]

    // Protected mode, then a far jump to make the new code segment take
    // effect. Indirect through memory because the offset is thirty-two bit
    // and was only just computed.
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp fword ptr [ap_far_pointer32_offset]

    .code32
ap_code32:
    // A flat data segment everywhere. Nothing here pushes, so SS is set
    // only because leaving it holding a real mode value is worse.
    mov ax, ap_data_selector
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    // Physical address extension, without which long mode cannot be
    // entered at all.
    mov eax, cr4
    or eax, (1 << 5)
    mov cr4, eax

    // A temporary page table, live only for the instant between turning
    // paging on and reaching sixty-four bit code. The real one cannot be
    // used here: a thirty-two bit write to CR3 can only express a root
    // below four gigabytes and the module may be mapped anywhere. It is
    // replaced below, where a full width write is available.
    mov eax, [ebx + ap_temporary_cr3_offset]
    mov cr3, eax

    // IA32_EFER.LME. Long mode becomes active when paging is enabled with
    // this set.
    mov ecx, 0xc0000080
    rdmsr
    or eax, (1 << 8)
    wrmsr

    mov eax, cr0
    or eax, (1 << 31)
    mov cr0, eax

    // Long mode is active but this is still a thirty-two bit code segment.
    // The far jump to a sixty-four bit one completes the transition.
    jmp fword ptr [ebx + ap_far_pointer64_offset]

    .code64
ap_code64:
    // The host's own control registers, so the C++ below runs in the same
    // environment every other processor's host side does. CR4 first,
    // because CR3 has to be loaded with the extension bits already as the
    // host keeps them; CR0 last, as it is the one still carrying the
    // temporary table's paging.
    //
    // The load of CR3 is only survivable because this page is mapped in
    // the host page table as well - the very next instruction is fetched
    // through it. The hypervisor arranges that before sending the IPI.
    mov rax, [rbx + ap_host_cr4_offset]
    mov cr4, rax
    mov rax, [rbx + ap_host_cr3_offset]
    mov cr3, rax
    mov rax, [rbx + ap_host_cr0_offset]
    mov cr0, rax

    mov rsp, [rbx + ap_stack_top_offset]
    mov rdi, [rbx + ap_argument_offset]
    mov rax, [rbx + ap_entry_offset]
    jmp rax

    // The code must not reach into the data area.
    .if (. - zpp_ap_start_up_begin) > ap_data_offset
    .error "the application processor start-up code overflows its data"
    .endif

    .space (ap_data_offset - (. - zpp_ap_start_up_begin))

    // Everything from here is written before the blob is used, except the
    // descriptors and the two selectors, which are constant.
ap_data:
    .quad 0                     // host_cr3
    .quad 0                     // host_cr0
    .quad 0                     // host_cr4
    .quad 0                     // entry
    .quad 0                     // stack_top
    .quad 0                     // argument
    .quad 0                     // temporary_cr3

    // Padded to the declared offsets rather than merely asserted to be at
    // them. A field that outgrew its slot makes the count below negative,
    // which the assembler refuses - so the layout here and the offsets
    // declared at the top cannot drift apart without the build breaking.
    .space (ap_gdt_pointer_offset - (. - zpp_ap_start_up_begin))
ap_gdt_pointer:
    .word (ap_gdt_end - ap_gdt - 1)
    .long (ap_gdt - zpp_ap_start_up_begin)

    .space (ap_far_pointer32_offset - (. - zpp_ap_start_up_begin))
ap_far_pointer32:
    .long (ap_code32 - zpp_ap_start_up_begin)
    .word ap_code32_selector

    .space (ap_far_pointer64_offset - (. - zpp_ap_start_up_begin))
ap_far_pointer64:
    .long (ap_code64 - zpp_ap_start_up_begin)
    .word ap_code64_selector

    .space (ap_gdt_offset - (. - zpp_ap_start_up_begin))
    // Flat descriptors, so none of them needs a base filled in later. The
    // thirty-two bit code segment is page granular over four gigabytes;
    // the sixty-four bit one sets the long mode bit and clears the default
    // operation size, as that pair is required to be.
ap_gdt:
    .quad 0
    .quad 0x00cf9a000000ffff
    .quad 0x00cf92000000ffff
    .quad 0x00af9a000000ffff
ap_gdt_end:

zpp_ap_start_up_end:
