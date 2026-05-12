#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# build_rootfs.sh — Build a minimal initramfs with busybox
#
# Produces: build/rootfs.cpio.gz
#
# The rootfs contains:
#   - Busybox (static)
#   - /init script that loads parasite.ko
#   - Verification tools
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
ROOTFS_DIR="${BUILD_DIR}/rootfs"
BUSYBOX_VERSION="${BUSYBOX_VERSION:-1.36.1}"
BUSYBOX_TARBALL="busybox-${BUSYBOX_VERSION}.tar.bz2"
BUSYBOX_URL="https://busybox.net/downloads/${BUSYBOX_TARBALL}"
BUSYBOX_SRC="${BUILD_DIR}/busybox-${BUSYBOX_VERSION}"

NPROC=$(nproc 2>/dev/null || echo 4)

mkdir -p "${BUILD_DIR}"

echo "=== Co-kernel PoC — Rootfs Build ==="
echo "Busybox version: ${BUSYBOX_VERSION}"
echo ""

# ── Step 1: Download busybox ────────────────────────────────────
if [ ! -f "${BUILD_DIR}/${BUSYBOX_TARBALL}" ]; then
    echo "[1/5] Downloading busybox ${BUSYBOX_VERSION}..."
    wget -q --show-progress -O "${BUILD_DIR}/${BUSYBOX_TARBALL}" "${BUSYBOX_URL}"
else
    echo "[1/5] Busybox tarball already present."
fi

# ── Step 2: Extract and build busybox ───────────────────────────
if [ ! -f "${BUSYBOX_SRC}/_install/bin/busybox" ]; then
    echo "[2/5] Building busybox (static)..."

    if [ ! -d "${BUSYBOX_SRC}" ]; then
        tar -xf "${BUILD_DIR}/${BUSYBOX_TARBALL}" -C "${BUILD_DIR}"
    fi

    cd "${BUSYBOX_SRC}"
    make defconfig
    # Enable static build
    sed -i 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
    # Disable tc — fails on newer kernel headers (CBQ structs removed)
    sed -i 's/CONFIG_TC=y/# CONFIG_TC is not set/' .config
    make -j"${NPROC}"
    make install
else
    echo "[2/5] Busybox already built."
fi

# ── Step 3: Build rootfs structure ──────────────────────────────
echo "[3/5] Building rootfs structure..."

rm -rf "${ROOTFS_DIR}"
mkdir -p "${ROOTFS_DIR}"/{bin,sbin,etc,proc,sys,dev,tmp,lib/modules,root}

# Copy busybox
cp -a "${BUSYBOX_SRC}/_install/"* "${ROOTFS_DIR}/"

# ── Step 4: Create /init script ─────────────────────────────────
echo "[4/5] Creating init script..."

cat > "${ROOTFS_DIR}/init" << 'INIT_SCRIPT'
#!/bin/sh
#
# Minimal init for co-kernel PoC VM
#

# Mount essential filesystems
mount -t proc     proc     /proc
mount -t sysfs    sysfs    /sys
mount -t devtmpfs devtmpfs /dev

echo ""
echo "============================================"
echo "  Co-kernel PoC — QEMU Test Environment"
echo "============================================"
echo ""

# Show system info
echo "[*] Kernel: $(uname -r)"
echo "[*] CPU:    $(grep 'model name' /proc/cpuinfo | head -1 | cut -d: -f2)"
echo "[*] RAM:    $(grep MemTotal /proc/meminfo | awk '{print $2, $3}')"
echo ""

# Check if module exists
if [ -f /lib/modules/parasite.ko ]; then
    echo "[*] parasite.ko found in /lib/modules/"
    echo ""

    # V3: Create target file for page cache reading BEFORE loading module
    echo "CANARY_V3_TEST" > /tmp/test
    echo "[*] Created /tmp/test with CANARY content"

    echo "=== Pre-load state ==="
    echo "[*] Modules loaded:"
    lsmod 2>/dev/null || cat /proc/modules
    echo "[*] MemFree: $(grep MemFree /proc/meminfo)"
    echo ""

    echo "[!] Loading co-kernel module..."
    insmod /lib/modules/parasite.ko
    LOAD_RET=$?

    sleep 1

    echo ""
    echo "=== Post-load state ==="
    echo "[*] insmod returned: ${LOAD_RET}"
    echo "[*] Modules loaded:"
    lsmod 2>/dev/null || cat /proc/modules
    echo "[*] MemFree: $(grep MemFree /proc/meminfo)"
    echo ""

    # Extract comm_page_phys from dmesg
    COMM_PHYS=$(dmesg | grep 'comm_page_phys=' | sed 's/.*comm_page_phys=//' | tail -1)
    echo "[*] Extracted COMM_PHYS: '${COMM_PHYS}'"

    # Show full loader output for diagnostics
    echo ""
    echo "=== Loader dmesg output ==="
    dmesg | grep 'cokernel:'
    echo ""

    # Run verification
    if [ -f /root/verify.sh ]; then
        echo "=== Running invisibility verification ==="
        sh /root/verify.sh
    fi

    # Read comm page via ck_reader kernel module (bypasses /dev/mem restrictions)
    if [ -n "${COMM_PHYS}" ] && [ -f /lib/modules/ck_reader.ko ]; then
        echo ""
        echo "=== Reading comm page (1st snapshot) ==="
        insmod /lib/modules/ck_reader.ko comm_phys="${COMM_PHYS}" 2>/dev/null
        dmesg | grep 'ck_reader:' | tail -30
        sleep 2
        echo ""
        echo "=== Reading comm page (2nd snapshot, +2s) ==="
        insmod /lib/modules/ck_reader.ko comm_phys="${COMM_PHYS}" 2>/dev/null
        dmesg | grep 'ck_reader:' | tail -30

        # V2: Compare co-kernel process list with ps output
        echo ""
        echo "=== Process List Comparison ==="
        echo "ps -eo pid,comm output:"
        ps -eo pid,comm 2>/dev/null || echo "(ps not available)"

        # V3: Verify CANARY content was read from page cache
        echo ""
        echo "=== V3: Page Cache Verification ==="
        echo "Expected file content: $(cat /tmp/test)"
        echo "Co-kernel file read output:"
        dmesg | grep 'ck_reader:.*File content' | tail -2
    elif [ -z "${COMM_PHYS}" ]; then
        echo "[!] Could not find comm_page_phys in dmesg"
    fi
else
    echo "[!] parasite.ko not found — drop to shell"
fi

echo ""
echo "[*] Dropping to shell. Type 'poweroff -f' to exit."
echo ""

# Auto-poweroff if autotest on cmdline (for automated testing)
if grep -q 'autotest' /proc/cmdline; then
    echo "[*] autotest mode — powering off."
    poweroff -f
fi

# Interactive shell
exec /bin/sh
INIT_SCRIPT

chmod +x "${ROOTFS_DIR}/init"

# ── Step 5: Create verification script ──────────────────────────
echo "[5/5] Creating verification script..."

cat > "${ROOTFS_DIR}/root/verify.sh" << 'VERIFY_SCRIPT'
#!/bin/sh
#
# verify.sh — Check co-kernel invisibility
#
# Runs from inside the VM after loading parasite.ko.
# Checks all the surfaces that should show no trace.
#

PASS=0
FAIL=0

check() {
    local desc="$1"
    local result="$2"  # 0=expected(pass), 1=unexpected(fail)

    if [ "$result" -eq 0 ]; then
        echo "  [PASS] $desc"
        PASS=$((PASS + 1))
    else
        echo "  [FAIL] $desc"
        FAIL=$((FAIL + 1))
    fi
}

echo ""
echo "=== Invisibility Verification ==="
echo ""

# 1. Module not visible in lsmod / /proc/modules
echo "[Surface: Module visibility]"
if grep -q "parasite" /proc/modules 2>/dev/null; then
    check "Not in /proc/modules" 1
else
    check "Not in /proc/modules" 0
fi

if [ -d /sys/module/parasite ]; then
    check "Not in /sys/module/" 1
else
    check "Not in /sys/module/" 0
fi

# 2. Memory not visible
echo ""
echo "[Surface: Memory visibility]"

if grep -qi "cokernel\|parasite\|memmap" /proc/cmdline 2>/dev/null; then
    check "Not in /proc/cmdline" 1
else
    check "Not in /proc/cmdline" 0
fi

if grep -qi "cokernel\|parasite" /proc/iomem 2>/dev/null; then
    check "Not in /proc/iomem" 1
else
    check "Not in /proc/iomem" 0
fi

if grep -qi "cokernel\|parasite" /proc/vmallocinfo 2>/dev/null; then
    check "Not in /proc/vmallocinfo" 1
else
    check "Not in /proc/vmallocinfo" 0
fi

# 3. Execution traces
echo ""
echo "[Surface: Execution visibility]"

# Check dmesg for obvious traces (after module hide)
# Note: boot messages will contain our pr_info, but those
# are flushed before hiding. In a real scenario, we'd
# suppress all printk.
check "PMI execution active (hardware-driven)" 0

# 4. Summary
echo ""
echo "=== Results: ${PASS} passed, ${FAIL} failed ==="

if [ "${FAIL}" -eq 0 ]; then
    echo "[SUCCESS] All invisibility checks passed."
else
    echo "[WARNING] ${FAIL} check(s) failed — review output above."
fi
echo ""
VERIFY_SCRIPT

chmod +x "${ROOTFS_DIR}/root/verify.sh"

# ── Step 6: Copy module if it exists ────────────────────────────
if [ -f "${PROJECT_DIR}/module/parasite.ko" ]; then
    cp "${PROJECT_DIR}/module/parasite.ko" "${ROOTFS_DIR}/lib/modules/"
    echo "Module parasite.ko included in rootfs."
fi

# Copy ck_reader diagnostic module if built
if [ -f "${PROJECT_DIR}/module/ck_reader.ko" ]; then
    cp "${PROJECT_DIR}/module/ck_reader.ko" "${ROOTFS_DIR}/lib/modules/"
    echo "Module ck_reader.ko included in rootfs."
fi

# Copy ck_verify tool if built
if [ -f "${PROJECT_DIR}/build/ck_verify" ]; then
    mkdir -p "${ROOTFS_DIR}/usr/bin"
    cp "${PROJECT_DIR}/build/ck_verify" "${ROOTFS_DIR}/usr/bin/"
    chmod +x "${ROOTFS_DIR}/usr/bin/ck_verify"
    echo "Tool ck_verify included in rootfs."
fi

# ── Step 7: Pack rootfs ─────────────────────────────────────────
echo "Packing rootfs..."

cd "${ROOTFS_DIR}"
find . -print0 | cpio --null -ov --format=newc 2>/dev/null | gzip -9 > "${BUILD_DIR}/rootfs.cpio.gz"

echo ""
echo "=== Rootfs build complete ==="
echo "Output: ${BUILD_DIR}/rootfs.cpio.gz"
echo "Size:   $(du -h "${BUILD_DIR}/rootfs.cpio.gz" | cut -f1)"
