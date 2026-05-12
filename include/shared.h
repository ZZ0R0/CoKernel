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

    /* V3 additions: page cache reading */
    uint64_t target_inode_dm;       /* DM address of target file inode (0=none) */
    uint64_t offset_i_mapping;      /* offsetof(struct inode, i_mapping) */
    uint64_t offset_i_size;         /* offsetof(struct inode, i_size) */
    uint64_t offset_a_i_pages;      /* offsetof(struct address_space, i_pages) */
    uint64_t offset_xa_head;        /* offsetof(struct xarray, xa_head) */
    uint64_t offset_xa_node_slots;  /* offsetof(struct xa_node, slots) */
    uint64_t vmemmap_base;          /* (unsigned long)pfn_to_page(0) */
    uint64_t sizeof_struct_page;    /* sizeof(struct page) */
};
/* V3: 168 bytes (21 × 8). Written at COKERNEL_DATA_OFFSET. */

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
 * data_buf layout (V3 — split):
 *   Zone 1: Process snapshot [0..1999]
 *     [0..3]   uint32_t nr_tasks
 *     [4..7]   uint32_t reserved
 *     [8..]    ck_process_entry[0..61]
 *   Zone 2: File content [2000..3999]
 *     [2000..2003] uint32_t file_bytes
 *     [2004..2007] uint32_t file_status
 *     [2008..3999] uint8_t  file_content[1992]
 */
#define SNAPSHOT_INTERVAL       512    /* take snapshot every N ticks */
#define MAX_SNAPSHOT_PROCS      62     /* (2000 - 8) / 32 = 62 */
#define MAX_TASK_WALK           1024   /* safety limit on list traversal */

/* V3: file content zone in data_buf */
#define FILE_DATA_OFFSET        2000   /* offset in data_buf for file zone */
#define MAX_FILE_CONTENT_SIZE   1992   /* 4000 - 2000 - 8 */
#define XA_MAX_DEPTH            4      /* max xarray traversal depth */

/* file_status codes */
#define FILE_STATUS_OK          0
#define FILE_STATUS_NO_INODE    1
#define FILE_STATUS_NO_PAGE     2
#define FILE_STATUS_UNSUPPORTED 3
#define FILE_STATUS_BAD_PTR     4
#define FILE_STATUS_TOO_DEEP    5

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
