/*
 * Exception capture for the guest-side coverage tests.
 *
 * The tests in zpp/guest_tests.h have to be able to say "this instruction
 * faulted, with this vector, and RIP did not move". There is no way to
 * observe that through the firmware: EDK2 installs its own handler for
 * every vector and that handler prints a register dump and hangs, so a
 * deliberate #UD ends the run rather than producing a result.
 *
 * So the tests install an interrupt descriptor table of their own for the
 * duration, and every one of its 256 gates lands here. The handler does
 * not attempt to skip the faulting instruction - that would need the
 * instruction's length, which is exactly what a fault does not tell you -
 * it rewrites the interrupt frame's RIP and RSP to a recovery point the
 * caller recorded, and IRETs to it. That is the same shape the
 * hypervisor's own host IDT uses for its recovery point, and it is the
 * only shape that works without a decoder.
 *
 * Two things are recorded for the caller: the vector and the error code,
 * and the RIP the fault was reported at. The last is what makes "RIP
 * unadvanced" checkable, which is half of what SDM 28.1.1 promises about
 * a fault taken instead of a VM exit.
 *
 * Written as assembly rather than a naked function so nothing depends on
 * how clang lays a naked function out under the MSVC ABI. The loader's
 * build already globs *.S, so this needs no CMake change.
 *
 * The whole file is behind the switch, not merely unreferenced when it is
 * off. Sources here are globbed, so a file that assembles unconditionally
 * puts a kilobyte of interrupt stubs and an armed recovery point into
 * every loader that ships - and CLAUDE.md's account of what
 * ZPP_VERIFY_HYPERVISOR cost is precisely about a test facility reaching a
 * build that did not ask for it.
 */

#if ZPP_GUEST_TESTS

    .text

/*
 * Per-vector entry points.
 *
 * Vectors 8, 10, 11, 12, 13, 14, 17, 21, 29 and 30 push an error code and
 * the rest do not (SDM Vol. 3A Table 6-1, "Protected-Mode Exceptions and
 * Interrupts", which carries the error-code column). The two macros below
 * normalise that away by pushing a zero where the processor pushed
 * nothing, so the common tail always sees the same frame.
 */

.macro stub_no_error vector
    .globl zpp_guest_test_stub_\vector
zpp_guest_test_stub_\vector:
    pushq $0
    pushq $\vector
    jmp zpp_guest_test_common
.endm

.macro stub_error vector
    .globl zpp_guest_test_stub_\vector
zpp_guest_test_stub_\vector:
    pushq $\vector
    jmp zpp_guest_test_common
.endm

    stub_no_error 0
    stub_no_error 1
    stub_no_error 2
    stub_no_error 3
    stub_no_error 4
    stub_no_error 5
    stub_no_error 6
    stub_no_error 7
    stub_error    8
    stub_no_error 9
    stub_error    10
    stub_error    11
    stub_error    12
    stub_error    13
    stub_error    14
    stub_no_error 15
    stub_no_error 16
    stub_error    17
    stub_no_error 18
    stub_no_error 19
    stub_no_error 20
    stub_error    21
    stub_no_error 22
    stub_no_error 23
    stub_no_error 24
    stub_no_error 25
    stub_no_error 26
    stub_no_error 27
    stub_no_error 28
    stub_error    29
    stub_error    30
    stub_no_error 31

/*
 * Everything from 32 up, which is the external-interrupt range. The tests
 * run with interrupts disabled so nothing should arrive, and one that does
 * is a result rather than something to swallow silently - so it lands here
 * and is reported as vector 32 regardless of which it really was.
 */
    stub_no_error 32

/*
 * The common tail.
 *
 * On entry the stack is, from RSP upwards:
 *   0  vector          pushed above
 *   8  error code      pushed by the processor or by the macro
 *   16 RIP             the faulting instruction
 *   24 CS
 *   32 RFLAGS
 *   40 RSP
 *   48 SS
 *
 * IRET in 64-bit mode always pops SS:RSP (SDM Vol. 2A, IRET/IRETD/IRETQ:
 * "in 64-bit mode ... the SS:RSP is popped unconditionally"), so
 * rewriting slots 16 and 40 is enough to land anywhere with any stack.
 */
    .globl zpp_guest_test_common
zpp_guest_test_common:
    movq (%rsp), %rax
    movq %rax, zpp_guest_test_vector(%rip)
    movq 8(%rsp), %rax
    movq %rax, zpp_guest_test_error(%rip)
    movq 16(%rsp), %rax
    movq %rax, zpp_guest_test_fault_rip(%rip)

    /*
     * Refuse to recover when nothing armed a recovery point. Returning to
     * a zero RIP would turn a stray fault into a second, stranger one, and
     * the whole value of this table is that a fault outside a probe is
     * still reported at the place it happened. So park instead: the
     * harness times out and the Bochs log carries the register dump.
     */
    movq zpp_guest_test_recovery_rip(%rip), %rax
    testq %rax, %rax
    jz zpp_guest_test_park

    movq %rax, 16(%rsp)
    movq zpp_guest_test_recovery_rsp(%rip), %rax
    movq %rax, 40(%rsp)

    addq $16, %rsp
    iretq

zpp_guest_test_park:
    cli
    hlt
    jmp zpp_guest_test_park

/*
 * int zpp_guest_test_try(void (*body)(void))
 *
 * Runs body and returns the vector it faulted with, or -1 when it
 * returned normally. Microsoft x64 calling convention, so the argument
 * arrives in RCX and 32 bytes of shadow space belong to the callee.
 *
 * The recovery RSP is recorded *after* the shadow space is reserved and
 * the recovery label sits *before* it is released, so the faulting path
 * and the returning path arrive at the same instruction with the same
 * RSP. Getting that the other way round leaks or over-pops 32 bytes on
 * exactly one of the two paths, which is the kind of imbalance that shows
 * up as a crash three calls later.
 */
    .globl zpp_guest_test_try
zpp_guest_test_try:
    pushq %rbp
    movq %rsp, %rbp
    pushq %rbx
    pushq %rsi
    pushq %rdi
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* No fault yet. -1 rather than 0, since 0 is a real vector (#DE). */
    movq $-1, zpp_guest_test_vector(%rip)
    movq $0, zpp_guest_test_error(%rip)
    movq $0, zpp_guest_test_fault_rip(%rip)

    subq $32, %rsp
    leaq zpp_guest_test_recovered(%rip), %rax
    movq %rax, zpp_guest_test_recovery_rip(%rip)
    movq %rsp, zpp_guest_test_recovery_rsp(%rip)

    callq *%rcx

zpp_guest_test_recovered:
    addq $32, %rsp

    /*
     * Disarmed on the way out, so a fault taken outside a probe parks
     * loudly rather than jumping back into a frame that no longer exists.
     */
    movq $0, zpp_guest_test_recovery_rip(%rip)

    movq zpp_guest_test_vector(%rip), %rax

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rdi
    popq %rsi
    popq %rbx
    popq %rbp
    retq

    .data
    .p2align 3

    .globl zpp_guest_test_vector
zpp_guest_test_vector:
    .quad -1

    .globl zpp_guest_test_error
zpp_guest_test_error:
    .quad 0

    .globl zpp_guest_test_fault_rip
zpp_guest_test_fault_rip:
    .quad 0

    .globl zpp_guest_test_recovery_rip
zpp_guest_test_recovery_rip:
    .quad 0

    .globl zpp_guest_test_recovery_rsp
zpp_guest_test_recovery_rsp:
    .quad 0

#endif /* ZPP_GUEST_TESTS */
