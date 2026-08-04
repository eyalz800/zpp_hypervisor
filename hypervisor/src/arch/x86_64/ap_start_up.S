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
    .set ap_null_idt_offset, ap_data_offset + 0x38
    .set ap_far_host_offset, ap_data_offset + 0x78
    .set ap_host_gdtr_offset, ap_data_offset + 0x86
    .set ap_host_idtr_offset, ap_data_offset + 0x96
    .set ap_host_cs_offset, ap_data_offset + 0xa0
    .set ap_stage_offset, ap_data_offset + 0xa2

    // How far the climb got, written as each step completes.
    //
    // This is the only account of a failure before the host IDT is loaded
    // below. Until then a fault has no handler to reach and escalates to a
    // reset, which takes the evidence with it - so the evidence is written
    // down in advance instead. Mirrored by ap_start_up_stage in the
    // header.
    .set ap_stage_real_mode, 1
    .set ap_stage_protected_mode, 2
    .set ap_stage_long_mode, 3
    .set ap_stage_host_page_table, 4
    .set ap_stage_host_tables, 5
    .set ap_stage_entering_cpp, 6

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
    mov eax, [ap_far_host_offset]
    add eax, ebx
    mov [ap_far_host_offset], eax

    mov byte ptr [ap_stage_offset], ap_stage_real_mode

    // Both loads in their thirty-two bit form, which is what the operand
    // size prefix buys: the sixteen bit form of lgdt and lidt loads only a
    // twenty-four bit base. This blob always lives below one megabyte so a
    // truncated base would still be correct, but the wide form is the one
    // that is documented to load what is written here.
    //
    // The interrupt descriptor table is loaded null, and deliberately. It
    // cannot be a useful one - the host IDT's gates name a code segment
    // that does not exist yet - but an IDTR left holding whatever was at
    // linear zero is worse than one that is empty by construction.
    .byte 0x66
    lidt [ap_null_idt_offset]
    .byte 0x66
    lgdt [ap_gdt_pointer_offset]

    // Protected mode, then a far jump to make the new code segment take
    // effect. Indirect through memory because the offset is thirty-two bit
    // and was only just computed.
    //
    // CR0 is written as an absolute value rather than read, modified and
    // written back. Preserving what an INIT left in it is not wanted here:
    // the only bit that has to be set is protection enable, and the fewer
    // bits this transition changes at once the fewer of them can be
    // implicated when it does not survive.
    mov eax, 1
    mov cr0, eax
    jmp fword ptr [ap_far_pointer32_offset]

    .code32
ap_code32:
    // A flat data segment everywhere, and this has to come first.
    //
    // Entering protected mode does not touch the segment registers: DS
    // still holds the descriptor real mode gave it, base 0x9c000 with
    // limit 0xffff, until it is reloaded. Every reference below is
    // ebx-relative, so its offset is the blob's linear address - past that
    // limit, and a #GP the null interrupt descriptor table turns into a
    // triple fault. SDM 12.9.1 step 9 says to reload DS, SS, ES, FS and GS
    // immediately after the far jump, and it means immediately: a progress
    // marker written above these five instructions is what found this.
    //
    // Nothing here pushes, so SS is set only because leaving it holding a
    // real mode value is worse.
    mov ax, ap_data_selector
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov byte ptr [ebx + ap_stage_offset], ap_stage_protected_mode

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

    // IA32_EFER.LME, and NXE with it. Long mode becomes active when
    // paging is enabled with LME set.
    //
    // NXE is not optional here even though nothing on this path executes
    // from a no-execute mapping. The host page table sets the execute
    // disable bit, and SDM 5.5.4 makes bit 63 of a paging entry reserved
    // while NXE is clear - a reserved bit set makes the entry fault (5.7).
    // Measured on a processor started this way: EFER 0x500 against the boot
    // processor's 0xd00.
    mov ecx, 0xc0000080
    rdmsr
    or eax, (1 << 8) | (1 << 11)
    wrmsr

    mov eax, cr0
    or eax, (1 << 31)
    mov cr0, eax

    // Long mode is active but this is still a thirty-two bit code segment.
    // The far jump to a sixty-four bit one completes the transition.
    jmp fword ptr [ebx + ap_far_pointer64_offset]

    .code64
ap_code64:
    mov byte ptr [rbx + ap_stage_offset], ap_stage_long_mode

    // A stack, before anything that needs one. The trampoline above never
    // pushed; the descriptor table switch below does.
    mov rsp, [rbx + ap_stack_top_offset]

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
    mov byte ptr [rbx + ap_stage_offset], ap_stage_host_page_table
    mov rax, [rbx + ap_host_cr0_offset]
    mov cr0, rax

    // The host descriptor tables, and this is what makes a fault from here
    // on survivable rather than fatal.
    //
    // Until now this processor has had no usable interrupt descriptor
    // table: the one at linear zero belongs to nobody, so any fault
    // escalated to a triple fault and reset the machine, taking the
    // evidence with it. Loading the host IDT is not enough on its own,
    // because its gates name the host code segment - a selector that does
    // not exist in the small GDT this trampoline climbed with, so
    // delivering the fault would itself fault. The host GDT has to come
    // first, and CS has to be reloaded out of it, which is what the far
    // return below is for. Every other processor reaches this same state
    // on its first VM exit; this one just gets there before running any
    // C++.
    lgdt [rbx + ap_host_gdtr_offset]
    movzx rax, word ptr [rbx + ap_host_cs_offset]
    push rax
    mov eax, [rbx + ap_far_host_offset]
    push rax
    retfq

ap_host_code:
    lidt [rbx + ap_host_idtr_offset]
    mov byte ptr [rbx + ap_stage_offset], ap_stage_host_tables

    mov rdi, [rbx + ap_argument_offset]
    mov rax, [rbx + ap_entry_offset]
    mov byte ptr [rbx + ap_stage_offset], ap_stage_entering_cpp

    // Leave the stack as a call would have. The entry point is ordinary
    // C++ compiled for the SysV convention, which expects RSP eight past
    // sixteen byte alignment on entry - the return address a call would
    // have pushed. Jumping to it with RSP aligned puts every local eight
    // out, and the first aligned SSE store to one of them faults.
    // Measured: capture_context faulting on movdqa with its context
    // argument eight past alignment. Same reason launch_on_cpu simulates a
    // call when it builds a stack.
    sub rsp, 8
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

    // Where the far return lands once the host GDT is loaded. Stored as an
    // offset and relocated with the others.
    .space (ap_far_host_offset - (. - zpp_ap_start_up_begin))
    .long (ap_host_code - zpp_ap_start_up_begin)

    // The host descriptor table pointers and code selector, written before
    // this blob is used. Ten bytes each, a limit then a base, which is the
    // form lgdt and lidt take in long mode.
    .space (ap_host_gdtr_offset - (. - zpp_ap_start_up_begin))
    .word 0
    .quad 0

    .space (ap_host_idtr_offset - (. - zpp_ap_start_up_begin))
    .word 0
    .quad 0

    .space (ap_host_cs_offset - (. - zpp_ap_start_up_begin))
    .word 0

    .space (ap_stage_offset - (. - zpp_ap_start_up_begin))
    .byte 0

zpp_ap_start_up_end:
