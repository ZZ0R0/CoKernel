# V1 — Technical Specification (Cahier des Charges)

---

## 0. Scope

V1 adds **direct memory access** and a **shared communication page** to the V0
co-kernel. The goal is to verify the co-kernel can read Linux memory and report
results to userspace.

**Base**: V0 is complete and working. V1 MUST NOT break any V0 guarantee.

---

## 1. Invariants (inherited from V0, MUST NOT be broken)

| ID | Invariant | Verification |
|----|-----------|--------------|
| INV-1 | No entry in `/proc/modules` after loading | `lsmod \| wc -l` = 0 (busybox rootfs) |
| INV-2 | No entry in `/sys/module/parasite*` | `ls /sys/module/parasite* 2>&1 \| grep -c "No such"` = 1 |
| INV-3 | No visible memory consumption | `/proc/meminfo` MemTotal unchanged (±4 MB tolerance) |
| INV-4 | No visible CPU anomaly | `perf stat sleep 1` shows no unexpected PMI events |
| INV-5 | No kernel log trace | `dmesg \| grep -ci parasite` = 0 (except the comm_page_phys line printed by the loader — removed in V6) |
| INV-6 | PMI handler runs without crash | 10 minutes continuous operation, no kernel panic |

---

## 2. Hardware and VM Environment

| Parameter | Value | Notes |
|-----------|-------|-------|
| Hypervisor | QEMU 9.x + KVM | `-enable-kvm` |
| Machine | q35 | ICH9 chipset |
| CPU | `-cpu host` | **Single vCPU** (V1 is single-CPU) |
| RAM | 1 GB | `-m 1024` |
| NIC | e1000 | `-device e1000,netdev=net0` `-netdev user,id=net0` (not used by co-kernel in V1) |
| Disk | None (initramfs only) | |
| Console | Serial | `-nographic -append "console=ttyS0"` |
| Kernel | Debian vmlinuz from `/boot/` | No custom build |

---

## 3. Functional Requirements

### 3.1 Direct Map

| ID | Requirement | Details |
|----|-------------|---------|
| **DMAP-1** | Map physical RAM into co-kernel CR3 | Range 0 to `ram_size` at `0xFFFF888000000000`, using 2 MB huge pages. |
| **DMAP-2** | Linux kernel pointers dereferenceable | A pointer like `0xFFFF88800ABCDEF0` must be readable from the co-kernel. |
| **DMAP-3** | Read-only in V1 | The co-kernel MUST NOT write to Linux memory in V1 (except the comm page, which is part of its own allocation). |

**VERIFY-DMAP**: co-kernel reads `init_task + OFFSET_TASK_COMM` → result is
`"swapper/0\0"`, written to comm page.

### 3.2 Shared Communication Page

| ID | Requirement | Details |
|----|-------------|---------|
| **COMM-1** | Allocate comm page as the last page of the 4 MB allocation | `page + (nr_pages - 1)` |
| **COMM-2** | Do NOT call `set_direct_map_invalid` on the comm page | It stays in Linux's direct map, accessible by userspace via `/dev/mem`. |
| **COMM-3** | Map comm page into co-kernel CR3 at a fixed VA | Same page, mapped in both address spaces. |
| **COMM-4** | Comm page layout | `magic`(8) + `version`(8) + `tick_count`(8) + `last_tsc`(8) + `status`(4) + `data_seq`(4) + `init_comm`(16) + `data_buf`(4000) = **4056 bytes** (fits in 4096). |
| **COMM-5** | Co-kernel writes `tick_count` on every PMI | Heartbeat — proves co-kernel is alive. |
| **COMM-6** | Co-kernel writes `init_task.comm` on first tick | Proves direct map works. |
| **COMM-7** | Comm page physical address printed to dmesg | `pr_info("ck: comm_page_phys=0x%lx\n", phys)` — before module erasure. |

**VERIFY-COMM**: `ck_verify` reads `magic` = `0x434B434F4D4D0001`, `tick_count`
increasing, `init_comm` = `"swapper/0"`.

### 3.3 Bootstrap Data

| ID | Requirement | Details |
|----|-------------|---------|
| **BOOT-1** | Struct layout | `magic`(8) + `init_task`(8) + `ram_size`(8) + `self_phys_base`(8) + `direct_map_base`(8) + `comm_page_va`(8) + `comm_page_phys`(8) = **56 bytes**. |
| **BOOT-2** | Written at byte 0 of `.data` section | Immediately after `.text` in the co-kernel binary. |
| **BOOT-3** | `init_task` resolved via kprobes | At load time, before module erasure. |
| **BOOT-4** | `ram_size` from `totalram_pages() * PAGE_SIZE` | |
| **BOOT-5** | `direct_map_base` = `0xFFFF888000000000` | Hardcoded constant. |
| **BOOT-6** | `comm_page_phys` = `page_to_phys(first_page + nr_pages - 1)` | |

**VERIFY-BOOT**: co-kernel reads `bootstrap_data.init_task` → non-zero.
Reads `*(pid_t *)(init_task + OFFSET_TASK_PID)` → 0 (swapper PID).

### 3.4 Userspace Verification Tool

| ID | Requirement | Details |
|----|-------------|---------|
| **TOOL-1** | Standalone C program | `tools/ck_verify.c`, statically linked, included in initramfs. |
| **TOOL-2** | Takes comm page physical address as hex argument | `ck_verify 0x<ADDR>` |
| **TOOL-3** | Opens `/dev/mem`, `mmap`s comm page | Read-only, `MAP_SHARED`. |
| **TOOL-4** | Displays: magic, version, tick_count, status, init_comm | Human-readable output. |
| **TOOL-5** | Optional: write results to file | `ck_verify 0x<ADDR> /tmp/out` |
| **TOOL-6** | Returns exit code 0 if magic is valid, 1 otherwise | For scripted testing. |

---

## 4. Internal Data Structures

### 4.1 `bootstrap_data` (written by loader, read by co-kernel)

```c
struct ck_bootstrap_data {
    uint64_t magic;             /* 0x434B424F4F540001 ("CKBOOT\x00\x01") */
    uint64_t init_task;         /* VA of init_task */
    uint64_t ram_size;          /* total physical RAM in bytes */
    uint64_t self_phys_base;    /* first co-kernel page physical address */
    uint64_t direct_map_base;   /* 0xFFFF888000000000 */
    uint64_t comm_page_va;      /* VA of comm page in co-kernel CR3 */
    uint64_t comm_page_phys;    /* physical address for /dev/mem access */
};
```

### 4.2 `comm_page` (written by co-kernel, read by userspace)

```c
struct ck_comm_page {
    uint64_t magic;           /* 0x434B434F4D4D0001 ("CKCOMM\x00\x01") */
    uint64_t version;         /* 1 */
    uint64_t tick_count;      /* incremented every PMI */
    uint64_t last_tsc;        /* rdtsc at last tick */
    uint32_t status;          /* 0 = init, 1 = running, 2 = error */
    uint32_t data_seq;        /* incremented when data_buf changes */
    char     init_comm[16];   /* "swapper/0\0" (from direct map read) */
    uint8_t  data_buf[4000];  /* reserved for V2+ structured output */
};
```

---

## 5. Kernel Structure Offsets (V1)

Only **one** offset is needed in V1:

| Structure | Field | Method |
|-----------|-------|--------|
| `task_struct` | `.comm` | `pahole -C task_struct vmlinux \| grep comm` |

More offsets (`.tasks`, `.pid`, `.cred`) are added in V2 when the co-kernel
walks the process list.

For V1, this single offset can be hardcoded for the target kernel and verified
at build time.

---

## 6. Memory Layout

```
Physical allocation: 4 MB (order 10, alloc_pages)

Pages 0..1022  → set_direct_map_invalid → invisible to Linux
Page  1023     → KEPT in direct map      → comm page

Co-kernel CR3:

0xFFFF800000000000  ┌──────────────────────┐
                    │  .text (code)         │  V0 + ~20 lines V1
                    ├──────────────────────┤
                    │  .data (bootstrap)    │  56 bytes
                    ├──────────────────────┤
                    │  .bss                 │  minimal
                    ├──────────────────────┤
                    │  Stack (grows down)   │  64 KB
                    ├──────────────────────┤
                    │  IDT (256 entries)    │  4 KB
                    ├──────────────────────┤
                    │  Page tables          │  ~128 KB (more than V0 due to physmem)
                    ├──────────────────────┤
                    │  Comm page            │  4 KB (last page of allocation)
                    ├──────────────────────┤
                    │  Reserved / free      │  ~3.8 MB
0xFFFF800000400000  └──────────────────────┘

0xFFFF888000000000  ┌──────────────────────┐  (2 MB huge pages)
                    │  Physical RAM mirror  │  1 GB
                    │  (Linux direct map)   │
0xFFFF888040000000  └──────────────────────┘

0xFEE00000          ┌──────────────────────┐
                    │  Local APIC           │  4 KB
                    └──────────────────────┘

trampoline_va       ┌──────────────────────┐
                    │  Trampoline page      │  4 KB
                    └──────────────────────┘
```

---

## 7. File Structure (changes from V0)

```
Modified:
  cokernel/cokernel.c       ← heartbeat + direct map read of init_task
  module/memory.c           ← keep comm page in direct map
  module/pagetable.c        ← map physmem (2 MB huge pages)
  module/resolve.c          ← resolve init_task, write bootstrap_data
  module/parasite_main.c    ← orchestrate V1 additions
  include/shared.h          ← bootstrap_data, comm_page structs, OFFSET_TASK_COMM

New:
  tools/ck_verify.c         ← userspace comm page reader
  Makefile                  ← add tools/ build target
```

---

## 8. Build and Test Pipeline

### 8.1 Build Steps

```
1. make cokernel              → build cokernel.bin (freestanding)
2. make module                → build parasite.ko (out-of-tree)
3. make tools                 → build ck_verify (statically linked)
4. make rootfs                → initramfs with parasite.ko + cokernel.bin + ck_verify
5. make run                   → launch QEMU
```

### 8.2 In-Guest Test Sequence

```
(guest boots, shell available)

# 1. Load the co-kernel
insmod /parasite.ko
  → prints: "ck: comm_page_phys=0x<ADDR>"
  → module erases itself

# 2. Verify V0 invariants
lsmod                               → empty
ls /sys/module/parasite* 2>&1       → "No such file"
cat /proc/meminfo | head -1         → MemTotal reasonable

# 3. Verify V1: comm page
/ck_verify 0x<ADDR>
  → magic:      0x434b434f4d4d0001 (valid)
  → tick_count: 23456 (non-zero)
  → status:     1 (running)
  → init_comm:  swapper/0

# 4. Verify heartbeat is alive (run again, tick_count should be higher)
sleep 1
/ck_verify 0x<ADDR>
  → tick_count: 25456 (increased)

# 5. Write to file
/ck_verify 0x<ADDR> /tmp/ck_status.txt
cat /tmp/ck_status.txt
  → magic=0x434b434f4d4d0001
  → tick_count=25500
  → status=1
  → init_comm=swapper/0

# 6. Stability: wait 10 minutes
sleep 600
/ck_verify 0x<ADDR>
  → still running, no crash
```

### 8.3 Acceptance Criteria

| ID | Criterion | Pass Condition |
|----|-----------|----------------|
| **V1-1** | V0 invariants | INV-1 through INV-6 all pass |
| **V1-2** | Comm page valid | magic = `0x434B434F4D4D0001` |
| **V1-3** | Heartbeat alive | `tick_count` > 0 and increasing over time |
| **V1-4** | Direct map works | `init_comm` = `"swapper/0"` |
| **V1-5** | File output | `ck_verify <addr> /tmp/out && cat /tmp/out` shows valid data |
| **V1-6** | Stability | 10 min continuous operation, no kernel panic |

---

## 9. Constraints

| ID | Constraint |
|----|------------|
| C-1 | **Single CPU only**. Multi-CPU support is V6. |
| C-2 | **50 μs per tick maximum**. V1 does very little per tick (~1 μs). |
| C-3 | **No Linux kernel function calls** from co-kernel. Loader can call them before erasing. |
| C-4 | **No runtime memory allocation**. Comm page is pre-allocated at load time. |
| C-5 | **Build against Debian packaged kernel**. Headers from `linux-headers-*`. |
| C-6 | **Target: Debian 6.12.x amd64**. `OFFSET_TASK_COMM` specific to this kernel. |
| C-7 | **QEMU q35 + KVM**. |

---

## 10. Out of Scope (Not in V1)

| Item | Deferred to |
|------|-------------|
| Process list walking (`task_struct`) | V2 |
| File reading (page cache traversal) | V3 |
| File writing (page cache + dirty flag) | V4 |
| Network exfiltration (`sk_buff` injection) | V5 |
| Multi-CPU (SMP) | V6 |
| Keylogging, credential harvesting, network capture | V7 |
| Anti-forensics (dmesg scrub, PMU hide, comm page removal) | V8 |
| NIC-specific driver code | Never (V5 uses Linux's networking structures) |
