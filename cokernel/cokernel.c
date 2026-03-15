/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cokernel.c — Co-kernel payload
 *
 * This is the core logic that executes inside the hidden memory region,
 * triggered by PMI hardware interrupts. It must:
 *   - Never call any Linux kernel function
 *   - Never access Linux data structures
 *   - Keep execution time < 5μs per invocation
 *   - Use only its own stack and heap
 */

#include "cokernel.h"

/*
 * The communication page is at a fixed offset from our virtual base.
 * We compute the pointer using our base address. The loader places us
 * at COKERNEL_VIRT_BASE + COKERNEL_TEXT_OFFSET. The comm page is at
 * COKERNEL_VIRT_BASE + COKERNEL_COMM_OFFSET.
 *
 * Since we're compiled position-independent and loaded at a known
 * address, we use absolute addresses.
 */
static volatile struct cokernel_comm *comm =
    (volatile struct cokernel_comm *)(COKERNEL_VIRT_BASE + COKERNEL_COMM_OFFSET);

/* Internal tick counter (redundant with comm, but local cache) */
static uint64_t local_ticks;

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
 * cokernel_init — One-time initialization.
 * Called by the loader module after copying the binary into hidden pages.
 */
void cokernel_init(void)
{
    local_ticks = 0;
}

/*
 * component_tick — Main entry point called on each PMI.
 *
 * This function must be fast and deterministic. It:
 *   1. Increments the tick counter
 *   2. Records the current TSC
 *   3. Updates the status field
 *
 * For PoC purposes, this demonstrates that the co-kernel is alive
 * and executing periodically via hardware interrupts.
 */
void component_tick(void)
{
    local_ticks++;

    comm->tick_count = local_ticks;
    comm->last_tsc = rdtsc_native();
    comm->status = 1;  /* running */
    comm->magic = COKERNEL_MAGIC;
}
