#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# run_qemu.sh — Launch QEMU VM for co-kernel PoC testing
#
# Boots a Debian-packaged kernel inside QEMU (not the running host kernel).
# KVER is auto-detected or can be overridden:
#   KVER=6.12.57+deb13-amd64 ./scripts/run_qemu.sh
#
# Prerequisites:
#   - build/rootfs.cpio.gz (from build_rootfs.sh)
#   - QEMU installed with KVM support
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"

# shellcheck source=config.sh
source "${SCRIPT_DIR}/config.sh"

INITRD="${BUILD_DIR}/rootfs.cpio.gz"

# ── Verify prerequisites ────────────────────────────────────────
missing=0
if [ ! -f "${BZIMAGE}" ]; then
    echo "ERROR: ${BZIMAGE} not found."
    echo "Available kernels:"
    ls /boot/vmlinuz-* 2>/dev/null || echo "  (none)"
    missing=1
fi
if [ ! -f "${INITRD}" ]; then
    echo "ERROR: ${INITRD} not found. Run: make rootfs"
    missing=1
fi
if [ "${missing}" -eq 1 ]; then
    exit 1
fi

# ── Check KVM availability ──────────────────────────────────────
KVM_FLAG=""
if [ -c /dev/kvm ] && [ -w /dev/kvm ]; then
    KVM_FLAG="-enable-kvm"
    echo "[*] KVM available — hardware acceleration enabled"
else
    echo "[!] KVM not available — falling back to software emulation"
    echo "[!] PMU/PMI features may not work correctly without KVM"
fi

# ── Rebuild rootfs with latest module if available ──────────────
if [ -f "${PROJECT_DIR}/module/parasite.ko" ]; then
    echo "[*] Updating rootfs with latest parasite.ko..."
    ROOTFS_DIR="${BUILD_DIR}/rootfs"
    if [ -d "${ROOTFS_DIR}" ]; then
        cp "${PROJECT_DIR}/module/parasite.ko" "${ROOTFS_DIR}/lib/modules/"
        # Also copy ck_reader.ko if built
        if [ -f "${PROJECT_DIR}/module/ck_reader.ko" ]; then
            cp "${PROJECT_DIR}/module/ck_reader.ko" "${ROOTFS_DIR}/lib/modules/"
        fi
        # Also update ck_verify if built
        if [ -f "${PROJECT_DIR}/build/ck_verify" ]; then
            mkdir -p "${ROOTFS_DIR}/usr/bin"
            cp "${PROJECT_DIR}/build/ck_verify" "${ROOTFS_DIR}/usr/bin/"
            chmod +x "${ROOTFS_DIR}/usr/bin/ck_verify"
        fi
        cd "${ROOTFS_DIR}"
        find . -print0 | cpio --null -ov --format=newc 2>/dev/null | \
            gzip -9 > "${BUILD_DIR}/rootfs.cpio.gz"
        cd "${PROJECT_DIR}"
    fi
fi

# ── QEMU command line ───────────────────────────────────────────
#
# Key options:
#   -machine q35      : Modern chipset with IOAPIC
#   -cpu host         : Pass through host CPU features (PMU, CET, etc.)
#   -smp 1            : Single CPU (simplifies PMI handling)
#   -m 1024           : 1 GB RAM
#   -nographic        : Serial console only
#   nokaslr           : Disable KASLR for easier debugging
#                       Remove this for stealth testing
#
# For debugging, add:
#   -s -S             : GDB server on :1234, wait for connection
#

echo ""
echo "=== Launching QEMU ==="
echo "  Kernel:  ${BZIMAGE}"
echo "  Initrd:  ${INITRD}"
echo "  RAM:     1024 MB"
echo "  CPUs:    1"
echo "  KVM:     $([ -n "${KVM_FLAG}" ] && echo "yes" || echo "no")"
echo ""
echo "Press Ctrl-A X to exit QEMU."
echo ""

exec qemu-system-x86_64 \
    -machine q35 \
    -cpu host \
    ${KVM_FLAG} \
    -smp 1 \
    -m 1024 \
    -kernel "${BZIMAGE}" \
    -initrd "${INITRD}" \
    -append "console=ttyS0 nokaslr iomem=relaxed autotest" \
    -nographic \
    -no-reboot \
    "$@"
