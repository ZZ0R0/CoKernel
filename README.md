# CoKernel — a parallel co-kernel PoC for Linux x86-64

**Proof of concept**: an independent execution component — a "co-kernel" — that coexists
with a running Linux kernel inside a QEMU x86-64 VM, aiming for *invisibility across three
surfaces*: **memory** (pages removed from Linux's direct map), **execution** (driven by a
hardware PMI on the PMU, no kernel function hooked, no text patched), and **behaviour**
(the loader module self-hides from `lsmod` / sysfs / kallsyms), while staying compatible
with CET (IBT + shadow stack).

[![Language](https://img.shields.io/badge/C%20%2F%20x86--64%20asm-555)]()
[![Target](https://img.shields.io/badge/Linux-x86--64%20kernel%20module-FCC624)]()
[![Run](https://img.shields.io/badge/runs%20in-QEMU%20%2F%20KVM-EC1C24)]()
[![License](https://img.shields.io/badge/license-GPL--2.0-blue)](LICENSE)

> ## ⚠️ Research / authorized lab use only
> This is offensive-security / OS-internals **research**. It loads an unsigned kernel
> module that manipulates page tables, the IDT and the PMU, and hides itself — run it
> **only inside a throwaway QEMU VM you control**, never on a machine you care about or
> don't own. No exploit, no persistence mechanism beyond the PoC, no targets included.
> Provided as-is for learning how stealth-execution techniques work (and therefore how to
> detect them — see *Residual traces* below).

## Architecture

```mermaid
flowchart TB
    US["USERSPACE — insmod parasite.ko"] --> DRV
    subgraph DRV["Loader module — Ring 0, ~100 ms one-shot init (parasite_main.c, 6 steps)"]
        M1["memory.c — alloc_pages + set_direct_map_invalid_noflush + SetPageReserved<br/>→ ~32 MB physical pages with no Linux virtual mapping"]
        M2["pagetable.c — build PGD→P4D→PUD→PMD→PTE for the hidden pages at COKERNEL_VIRT_BASE<br/>→ a clean CR3 the co-kernel runs under"]
        M3["execution.c + pmi_entry.S — preset IA32_FIXED_CTR0, LAPIC LVT_PC → vector 0x42,<br/>IDT[0x42] = component_pmi_entry (ENDBR64 · save · CR3 switch · call · restore · iretq)"]
        M4["resolve.c — symbol resolution via kprobes (no exported-symbol dependency)"]
        M5["stealth.c — list_del(THIS_MODULE->list) · kobject_del(mkobj) · clear syms<br/>→ invisible to lsmod / /sys/module / kallsyms"]
        M1 --> M2 --> M3 --> M4 --> M5
    end
    DRV -. "hardware PMI every ~1M cycles" .-> CK
    subgraph CK["Co-kernel — cokernel.c (flat binary, raw .lds)"]
        T["tick handler — runs on its own CR3, on pages outside Linux's direct map"]
    end
    CK -. "iretq (not RET) → shadow stack untouched; ENDBR64 → IBT satisfied" .-> US
```

## Repository layout

```text
Makefile                top-level build orchestrator
include/shared.h         shared definitions (memory layout, constants)
cokernel/                the co-kernel payload — cokernel.c/.h, cokernel.lds (flat binary), Makefile
module/                  the loader (out-of-tree Kbuild module):
                           parasite_main.c (orchestrator) · memory.c/.h · pagetable.c/.h ·
                           execution.c/.h · resolve.c/.h · stealth.c/.h · pmi_entry.S · ck_reader.c · Makefile
scripts/                 build_module.sh · build_rootfs.sh (busybox initramfs) · config.sh · run_qemu.sh
markdown/                design doc (V0) + per-version notes (V1–V3) + Roadmap
```

> Build artifacts (`*.o`, `*.ko`, `*.mod*`, `.*.cmd`, `Module.symvers`, `cokernel.bin/.elf`,
> the generated `module/cokernel_blob.h`, `build/`) are not tracked — they're produced by `make`.

## Prerequisites

Debian/Ubuntu x86-64 host with the headers for the running kernel:

```bash
sudo apt-get install -y build-essential gcc make libelf-dev libssl-dev \
    linux-headers-$(uname -r) qemu-system-x86 wget cpio xxd gzip
```

## Build & run

```bash
make all          # build the co-kernel binary + parasite.ko, then the busybox initramfs
make run          # launch QEMU with the host kernel (/boot/vmlinuz-$(uname -r))
# step by step: make module ; make rootfs ; make run
# debug:        make run -- -s -S   (then gdb → target remote :1234)
# exit QEMU:    Ctrl-A X   (or `poweroff -f` inside)
```

Inside the VM the init script: shows the pre-load state → `insmod parasite.ko` → shows the
post-load state (the module should be gone) → runs the verification script → drops to a
shell. Manual checks: `lsmod`, `cat /proc/modules`, `ls /sys/module/`, `cat /proc/iomem`,
`cat /proc/vmallocinfo`, `dmesg | grep cokernel`.

## How it works (short)

1. **Invisible memory** — `alloc_pages` a contiguous block, `set_direct_map_invalid_noflush`
   it out of `ffff888…`, flush TLB, `SetPageReserved` so the buddy allocator won't reclaim it.
2. **Custom page tables** — a full PGD→PTE hierarchy mapping those pages at
   `COKERNEL_VIRT_BASE`; the PMI handler loads that PGD into CR3.
3. **Hardware execution** — `IA32_FIXED_CTR0` overflows after ~1M cycles → PMI → LAPIC
   delivers vector `0x42` → `IDT[0x42]` is the handler. No function hooked, no text patched.
4. **Self-hiding** — unlink from the module list, drop the kobject, clear the symbol table.
5. **CET-friendly** — handler starts with `ENDBR64` (IBT), ends with `iretq` (shadow stack
   never consulted).

## Residual traces (so you know what *would* detect it)

| Trace | Detectable by | Risk |
|---|---|---|
| `IDT[0x42]` modified | full IDT scan | low |
| PMU MSRs modified | `rdmsr` / `perf stat` | very low |
| ~32 MB "used" in `MemFree` | `/proc/meminfo` | none (indistinguishable) |
| `PageReserved` struct pages | exhaustive vmemmap scan | very low |

## Status

PoC. Runs in a QEMU x86-64 VM with KVM. Versioned design notes in `markdown/` (V0 design
doc → V1–V3 + Roadmap).

## License

[GPL-2.0](LICENSE) — Linux kernel module; research/educational purposes only.

---

<sub>Part of my work — more at <a href="https://zz0r0.fr">zz0r0.fr</a>.</sub>
