/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cokernel.c — Co-kernel payload (V1)
 *
 * This is the core logic that executes inside the hidden memory region,
 * triggered by PMI hardware interrupts. It must:
 *   - Never call any Linux kernel function
 *   - Keep execution time < 50μs per invocation
 *   - Use only its own stack and heap
 *
 * Design: ZERO dependency on .bss or .data sections.
 * All pointers are computed from compile-time constants each call.
 * The comm page's own magic field serves as the "initialized" flag:
 *   magic == 0              → not yet initialized (GFP_ZERO)
 *   magic == COKERNEL_COMM_MAGIC → initialized, normal heartbeat
 */

#include "cokernel.h"

/*
 * Read the TSC (Time Stamp Counter) — inline, no kernel dependency.
 */
static inline uint64_t rdtsc_native(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/*
 * Minimal memcpy — no libc available in freestanding mode.
 */
static void memcpy_ck(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--)
        *d++ = *s++;
}

/*
 * component_tick — Sole entry point, called on each PMI.
 *
 * Self-initializing: on the very first call the comm page magic is 0
 * (memory was zeroed by GFP_ZERO). We detect this and run one-time
 * initialization before entering the normal heartbeat path.
 *
 * No static variables are used — everything is derived from
 * compile-time constants (COKERNEL_VIRT_BASE + offsets).
 */
__attribute__((section(".text.entry")))
void component_tick(void)
{
    /* Comm page: always at a fixed, known address */
    volatile struct ck_comm_page *comm =
        (volatile struct ck_comm_page *)(COKERNEL_VIRT_BASE + COKERNEL_COMM_OFFSET);

    /* Heartbeat — written EVERY tick, unconditionally */
    comm->tick_count++;
    comm->last_tsc = rdtsc_native();

    /* First-tick initialization: magic is 0 (page was zeroed) */
    if (comm->magic != COKERNEL_COMM_MAGIC) {
        struct ck_bootstrap_data *boot =
            (struct ck_bootstrap_data *)(COKERNEL_VIRT_BASE + COKERNEL_DATA_OFFSET);

        comm->magic   = COKERNEL_COMM_MAGIC;
        comm->version = 1;
        comm->status  = 1;  /* running */

        /* Read init_task.comm via Linux direct map */
        if (boot->magic == COKERNEL_BOOTSTRAP_MAGIC) {
            const char *task_comm = (const char *)
                (boot->init_task_dm + boot->offset_task_comm);
            memcpy_ck((void *)comm->init_comm, task_comm, 16);
            comm->data_seq = 1;
        }
    }
}
