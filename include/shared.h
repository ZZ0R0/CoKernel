/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _COKERNEL_SHARED_H
#define _COKERNEL_SHARED_H

/*
 * Shared definitions between the loader module and the co-kernel binary.
 *
 * Memory layout of the 4 MB allocated region:
 *
 *   Offset          Size        Content
 *   ──────────────────────────────────────────
 *   0x00000000      512 KB      Co-kernel .text + .rodata
 *   0x00080000      64  KB      Co-kernel .data + .bss (bootstrap_data at +0)
 *   0x00090000      64  KB      Page tables (PGD→PTE)
 *   0x000A0000      4   KB      IDT (256 entries × 16 bytes)
 *   0x000A1000      64  KB      Stack (grows down from top)
 *   0x000B1000      4   KB      Trampoline code
 *   0x000B2000      ~3.5 MB     Heap / buffers / reserved
 *   0x003FF000      4   KB      Shared communication page (last page)
 */

#define COKERNEL_TOTAL_SIZE     (4UL * 1024 * 1024)   /* 4 MB */
#define COKERNEL_TEXT_OFFSET    0x00000000UL
#define COKERNEL_TEXT_SIZE      (512UL * 1024)
#define COKERNEL_DATA_OFFSET    0x00080000UL
#define COKERNEL_DATA_SIZE      (64UL * 1024)
#define COKERNEL_PGT_OFFSET     0x00090000UL
#define COKERNEL_PGT_SIZE       (64UL * 1024)
#define COKERNEL_IDT_OFFSET     0x000A0000UL
#define COKERNEL_IDT_SIZE       (4UL * 1024)
#define COKERNEL_STACK_OFFSET   0x000A1000UL
#define COKERNEL_STACK_SIZE     (64UL * 1024)
#define COKERNEL_TRAMPOLINE_OFFSET 0x000B1000UL
#define COKERNEL_TRAMPOLINE_SIZE   (4UL * 1024)
#define COKERNEL_HEAP_OFFSET    0x000B2000UL
#define COKERNEL_HEAP_SIZE      (COKERNEL_TOTAL_SIZE - COKERNEL_HEAP_OFFSET - 4096UL)
#define COKERNEL_COMM_OFFSET    (COKERNEL_TOTAL_SIZE - 4096UL)  /* 0x3FF000 — last page */
#define COKERNEL_COMM_SIZE      (4UL * 1024)

/* Stack top: stack grows downward */
#define COKERNEL_STACK_TOP      (COKERNEL_STACK_OFFSET + COKERNEL_STACK_SIZE)

/* PMI configuration */
#define PMI_VECTOR              0x42
#define PMI_INTERVAL            1000000ULL     /* ~1M cycles between invocations */
#define PMI_RELOAD_VALUE        ((uint64_t)(-(int64_t)PMI_INTERVAL))

/* Virtual base address for co-kernel mapping in its own CR3 */
#define COKERNEL_VIRT_BASE      0xFFFF800000000000UL

/* Linux direct map base — physmem mapped here in co-kernel CR3 */
#define LINUX_DIRECT_MAP_BASE   0xFFFF888000000000UL

/*
 * Bootstrap data — written once by the loader at DATA_OFFSET,
 * read by the co-kernel on first tick.
 */
#define COKERNEL_BOOTSTRAP_MAGIC 0x434B424F4F540001ULL  /* "CKBOOT\x00\x01" */

struct ck_bootstrap_data {
    /* V1 fields (positions FIXED — do NOT reorder) */
    uint64_t magic;             /* COKERNEL_BOOTSTRAP_MAGIC */
    uint64_t init_task_dm;      /* init_task address in direct map space */
    uint64_t ram_size;          /* total physical RAM in bytes */
    uint64_t self_phys_base;    /* physical base of co-kernel allocation */
    uint64_t direct_map_base;   /* LINUX_DIRECT_MAP_BASE */
    uint64_t comm_page_va;      /* VA of comm page in co-kernel CR3 */
    uint64_t comm_page_phys;    /* physical address (for userspace /dev/mem) */
    uint64_t offset_task_comm;  /* offsetof(struct task_struct, comm) */

    /* V2 additions: task_struct traversal offsets */
    uint64_t offset_tasks;      /* offsetof(struct task_struct, tasks) */
    uint64_t offset_pid;        /* offsetof(struct task_struct, pid) */
    uint64_t offset_tgid;       /* offsetof(struct task_struct, tgid) */
    uint64_t offset_real_cred;  /* offsetof(struct task_struct, real_cred) */
    uint64_t offset_cred_uid;   /* offsetof(struct cred, uid) */
};
/* V2: 104 bytes (13 × 8). Written at COKERNEL_DATA_OFFSET. */

/*
 * Communication page — written by co-kernel on every PMI tick,
 * readable by userspace via /dev/mem at comm_page_phys.
 */
#define COKERNEL_COMM_MAGIC     0x434B434F4D4D0001ULL   /* "CKCOMM\x00\x01" */

struct ck_comm_page {
    volatile uint64_t magic;           /* COKERNEL_COMM_MAGIC */
    volatile uint64_t version;         /* Protocol version: 1 */
    volatile uint64_t tick_count;      /* Incremented each PMI */
    volatile uint64_t last_tsc;        /* TSC at last invocation */
    volatile uint32_t status;          /* 0=init, 1=running, 2=error */
    volatile uint32_t data_seq;        /* Incremented when data_buf changes */
    char              init_comm[16];   /* First task .comm ("swapper/0") */
    uint8_t           data_buf[4000];  /* V2: process snapshot data */
};
/* 4056 bytes. Fits in one 4096-byte page. */

/*
 * Process snapshot entry (V2) — one per process in data_buf.
 *
 * data_buf layout:
 *   [0..3]   uint32_t nr_tasks
 *   [4..7]   uint32_t reserved
 *   [8..]    ck_process_entry[0..N-1]
 */
#define SNAPSHOT_INTERVAL       512    /* take snapshot every N ticks */
#define MAX_SNAPSHOT_PROCS      124    /* (4000 - 8) / 32 = 124 */
#define MAX_TASK_WALK           1024   /* safety limit on list traversal */

struct ck_process_entry {
    int32_t  pid;
    int32_t  tgid;
    uint32_t uid;
    uint32_t reserved;
    char     comm[16];
};
/* 32 bytes per entry. */

/* MSR definitions (for use in assembly and C) */
#define MSR_IA32_FIXED_CTR_CTRL     0x38D
#define MSR_IA32_FIXED_CTR0         0x309
#define MSR_IA32_PERF_GLOBAL_CTRL   0x38F
#define MSR_IA32_PERF_GLOBAL_OVF    0x390

/* LAPIC registers (MMIO offsets from LAPIC base 0xFEE00000) */
#define LAPIC_BASE                  0xFEE00000UL
#define LAPIC_EOI_OFFSET            0xB0
#define LAPIC_LVTPC_OFFSET          0x340

#endif /* _COKERNEL_SHARED_H */
