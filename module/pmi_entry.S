/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pmi_entry.S — PMI (Performance Monitoring Interrupt) handler
 *
 * This is the assembly trampoline installed in IDT vector 0x42.
 * It lives in a dedicated page that is mapped at the SAME virtual
 * address in BOTH Linux's and the co-kernel's page tables. This
 * allows the CR3 switch to be transparent to the instruction flow.
 *
 * Flow:
 *   1. ENDBR64 (satisfy IBT/CET)
 *   2. Save all GPRs + RFLAGS on Linux stack
 *   3. Save Linux RSP and CR3 into data block
 *   4. Switch CR3 to co-kernel page tables
 *   5. Switch RSP to co-kernel stack
 *   6. Call component_tick()
 *   7. Reload data block pointer (r12 is callee-saved, safe)
 *   8. Restore Linux RSP and CR3
 *   9. Reload PMI counter
 *  10. Clear overflow status
 *  11. Send LAPIC EOI
 *  12. Restore all GPRs + RFLAGS
 *  13. IRETQ
 *
 * The data block at the end of this code is patched by the loader.
 * All necessary addresses (CR3, stack, function pointer) are stored
 * there and accessed via RIP-relative addressing.
 */

.text
.globl component_pmi_entry
.globl pmi_trampoline_end
.globl trampoline_data
.align 16

component_pmi_entry:
    /* CET IBT compliance — must be very first instruction */
    .byte 0xF3, 0x0F, 0x1E, 0xFA   /* ENDBR64 */

    /* ── Save all general-purpose registers + flags ──────────── */
    pushq   %rax
    pushq   %rbx
    pushq   %rcx
    pushq   %rdx
    pushq   %rsi
    pushq   %rdi
    pushq   %rbp
    pushq   %r8
    pushq   %r9
    pushq   %r10
    pushq   %r11
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    pushfq

    /* ── Load data block pointer ─────────────────────────────── */
    leaq    trampoline_data(%rip), %r12

    /* ── Save Linux state into data block ────────────────────── */
    movq    %rsp, 24(%r12)          /* save Linux RSP */
    movq    %cr3, %rax
    movq    %rax, 32(%r12)          /* save Linux CR3 */

    /* ── Switch to co-kernel address space ───────────────────── */
    movq    0(%r12), %rax           /* cokernel_cr3 */
    testq   %rax, %rax
    jz      .Lskip_cr3_load
    movq    %rax, %cr3              /* now in co-kernel space */
.Lskip_cr3_load:

    /* ── Switch to co-kernel stack ───────────────────────────── */
    movq    8(%r12), %rsp           /* cokernel_stack_top */

    /* ── Call co-kernel tick function ─────────────────────────── */
    /*
     * r12 is callee-saved (System V ABI), so component_tick()
     * will preserve it. On return, r12 still points to our data.
     */
    callq   *16(%r12)              /* component_tick() */

    /* ── Restore Linux state ─────────────────────────────────── */
    /*
     * r12 is intact (callee-saved). However, we're still under
     * co-kernel's CR3. The trampoline page is mapped identically
     * in both address spaces, so this code is reachable.
     */
    movq    24(%r12), %rsp          /* restore Linux RSP */
    movq    32(%r12), %rax          /* restore Linux CR3 */
    movq    %rax, %cr3              /* back in Linux space */

    /* ── Reload PMI counter for next invocation ──────────────── */
    /*
     * MSR_IA32_FIXED_CTR0 = 0x309
     * Preset to -(1000000) = 0xFFFFFFFFFFF0BDC0
     * WRMSR: ECX=MSR, EDX:EAX=value
     */
    movl    $0x309, %ecx
    movl    $0xFFF0BDC0, %eax       /* low 32 bits of -1000000 */
    movl    $0xFFFFFFFF, %edx       /* high 32 bits */
    wrmsr

    /* ── Clear PMU overflow status ───────────────────────────── */
    /*
     * IA32_PERF_GLOBAL_STATUS_RESET (0x390)
     * Set bit 32 to clear Fixed Counter 0 overflow flag
     */
    movl    $0x390, %ecx
    movl    $0x00000000, %eax
    movl    $0x00000001, %edx       /* bit 32 = fixed ctr 0 */
    wrmsr

    /* ── Send EOI to LAPIC ───────────────────────────────────── */
    movq    $0xFEE000B0, %rax
    movl    $0, (%rax)

    /* ── Restore all registers ───────────────────────────────── */
    popfq
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %r11
    popq    %r10
    popq    %r9
    popq    %r8
    popq    %rbp
    popq    %rdi
    popq    %rsi
    popq    %rdx
    popq    %rcx
    popq    %rbx
    popq    %rax

    iretq

/*
 * ── Data block ──────────────────────────────────────────────────
 *
 * Patched by the loader at install time.
 * Accessed via RIP-relative from the handler above.
 * Must be in the same page as the code (mapped in both CR3s).
 */
.align 8
trampoline_data:
    .quad   0       /* +0  : cokernel_cr3       (phys addr of PGD)     */
    .quad   0       /* +8  : cokernel_stack_top  (VA in co-kernel space)*/
    .quad   0       /* +16 : cokernel_tick_fn    (VA of component_tick) */
    .quad   0       /* +24 : saved_linux_rsp     (runtime scratch)      */
    .quad   0       /* +32 : saved_linux_cr3     (runtime scratch)      */

pmi_trampoline_end:
    /* Size marker — loader uses &pmi_trampoline_end - &component_pmi_entry */
