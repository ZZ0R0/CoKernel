/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pmi_entry.S — PMI (Performance Monitoring Interrupt) handler
 *
 * This is the assembly trampoline installed in IDT vector 0x42.
 * It lives in a dedicated page that is mapped at the SAME virtual
 * address in BOTH Linux's and the co-kernel's page tables. This
 * allows the CR3 switch to be transparent to the instruction flow.
 *
 * The data block at the end is patched by the loader with:
 *   - Co-kernel CR3, stack, tick function
 *   - Counter MSR address (Intel 0x309 or AMD 0xC0010201)
 *   - APIC mode flag (0=xAPIC MMIO, 1=x2APIC MSR)
 *   - Reload value for the counter
 *
 * Flow:
 *   1. ENDBR64 (satisfy IBT/CET)
 *   2. Save all GPRs + RFLAGS on Linux stack
 *   3. Save Linux RSP and CR3 into data block
 *   4. Switch CR3 to co-kernel page tables
 *   5. Switch RSP to co-kernel stack
 *   6. Call component_tick()
 *   7. Restore Linux RSP and CR3
 *   8. Reload PMI counter (AMD or Intel MSR)
 *   9. Clear overflow / unmask LVTPC
 *  10. Send LAPIC EOI (x2APIC MSR or xAPIC MMIO)
 *  11. Restore all GPRs + RFLAGS
 *  12. IRETQ
 */

.text
.globl component_pmi_entry
.globl pmi_trampoline_end
.globl trampoline_data
.align 16

component_pmi_entry:
    /* CET IBT compliance */
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
    movq    %rax, %cr3
.Lskip_cr3_load:

    /* ── Switch to co-kernel stack ───────────────────────────── */
    movq    8(%r12), %rsp

    /* ── Call co-kernel tick function ─────────────────────────── */
    callq   *16(%r12)              /* component_tick() */

    /* ── Restore Linux state ─────────────────────────────────── */
    movq    24(%r12), %rsp          /* restore Linux RSP */
    movq    32(%r12), %rax
    movq    %rax, %cr3              /* back in Linux space */

    /* ── Reload PMI counter ──────────────────────────────────── */
    /*
     * counter_msr (offset 40) holds the MSR address:
     *   Intel: 0x309 (IA32_FIXED_CTR0)
     *   AMD:   0xC0010201 (PerfCtr0)
     * reload_value (offset 48) holds the preset value.
     */
    movl    40(%r12), %ecx          /* counter_msr */
    movl    52(%r12), %edx          /* reload_value high 32 bits */
    movl    48(%r12), %eax          /* reload_value low 32 bits */
    wrmsr

    /* ── Clear overflow / unmask LVTPC ───────────────────────── */
    /*
     * ovf_clear_msr (offset 56): MSR to write for clearing overflow.
     *   Intel: 0x390 (IA32_PERF_GLOBAL_STATUS_RESET)
     *   AMD:   0 (not used — AMD doesn't need this step)
     * ovf_clear_val (offset 64): value to write.
     *   Intel: bit 32 set
     *   AMD:   unused
     */
    movl    56(%r12), %ecx          /* ovf_clear_msr */
    testl   %ecx, %ecx
    jz      .Lskip_ovf_clear
    movl    60(%r12), %eax          /* ovf_clear_val low */
    movl    64(%r12), %edx          /* ovf_clear_val high */
    wrmsr
.Lskip_ovf_clear:

    /* ── Unmask LAPIC LVTPC (re-enable for next PMI) ─────────── */
    /*
     * After PMI delivery, some CPUs mask LVTPC. Unmask it.
     * apic_mode (offset 72): 0=xAPIC, 1=x2APIC
     */
    cmpl    $0, 72(%r12)
    jne     .Lx2apic_unmask
    /* xAPIC MMIO: write LVT_PC register at offset 0x340 */
    movq    $0xFEE00340, %rax
    movl    $0x42, (%rax)           /* PMI_VECTOR, unmasked */
    jmp     .Ldo_eoi
.Lx2apic_unmask:
    /* x2APIC MSR: LVTPC is MSR 0x834 (0x800 + 0x340/16) */
    movl    $0x834, %ecx
    xorl    %edx, %edx
    movl    $0x42, %eax             /* PMI_VECTOR, unmasked */
    wrmsr

.Ldo_eoi:
    /* ── Send LAPIC EOI ──────────────────────────────────────── */
    cmpl    $0, 72(%r12)
    jne     .Lx2apic_eoi
    /* xAPIC MMIO EOI */
    movq    $0xFEE000B0, %rax
    movl    $0, (%rax)
    jmp     .Leoi_done
.Lx2apic_eoi:
    /* x2APIC MSR EOI: MSR 0x80B */
    movl    $0x80B, %ecx
    xorl    %eax, %eax
    xorl    %edx, %edx
    wrmsr
.Leoi_done:

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
 *
 * Extended layout (compared to V0):
 *   +0   cokernel_cr3
 *   +8   cokernel_stack_top
 *   +16  cokernel_tick_fn
 *   +24  saved_linux_rsp     (runtime scratch)
 *   +32  saved_linux_cr3     (runtime scratch)
 *   +40  counter_msr         (u32: Intel 0x309, AMD 0xC0010201)
 *   +44  (pad)
 *   +48  reload_value        (u64: -(PMI_INTERVAL))
 *   +56  ovf_clear_msr       (u32: Intel 0x390, AMD 0)
 *   +60  ovf_clear_val_lo    (u32)
 *   +64  ovf_clear_val_hi    (u32)
 *   +68  (pad)
 *   +72  apic_mode           (u32: 0=xAPIC, 1=x2APIC)
 */
.align 8
trampoline_data:
    .quad   0       /* +0  : cokernel_cr3       */
    .quad   0       /* +8  : cokernel_stack_top  */
    .quad   0       /* +16 : cokernel_tick_fn    */
    .quad   0       /* +24 : saved_linux_rsp     */
    .quad   0       /* +32 : saved_linux_cr3     */
    .long   0       /* +40 : counter_msr         */
    .long   0       /* +44 : pad                 */
    .quad   0       /* +48 : reload_value        */
    .long   0       /* +56 : ovf_clear_msr       */
    .long   0       /* +60 : ovf_clear_val_lo    */
    .long   0       /* +64 : ovf_clear_val_hi    */
    .long   0       /* +68 : pad                 */
    .long   0       /* +72 : apic_mode           */

pmi_trampoline_end:
    /* Size marker */
