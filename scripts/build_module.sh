#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# build_module.sh — Build the co-kernel binary and kernel module
#
# Uses a Debian-packaged kernel (no compilation needed).
# KVER is auto-detected or can be overridden:
#   KVER=6.12.57+deb13-amd64 ./scripts/build_module.sh
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"

# shellcheck source=config.sh
source "${SCRIPT_DIR}/config.sh"

if [ ! -d "${KDIR}" ]; then
    echo "ERROR: Kernel headers not found at ${KDIR}"
    echo "Install them with: sudo apt install linux-headers-${KVER}"
    echo ""
    echo "Available kernels on this system:"
    ls -1 /boot/vmlinuz-* 2>/dev/null | sed 's|/boot/vmlinuz-||' || echo "  (none)"
    exit 1
fi

mkdir -p "${BUILD_DIR}"

echo "=== Co-kernel PoC — Module Build ==="
echo "Target kernel: ${KVER}"
echo "Headers:       ${KDIR}"
echo ""

# ── Step 1: Build co-kernel flat binary ─────────────────────────
echo "[1/3] Building co-kernel binary..."
make -C "${PROJECT_DIR}/cokernel" clean
make -C "${PROJECT_DIR}/cokernel"

echo ""

# ── Step 2: Generate blob header ────────────────────────────────
echo "[2/3] Generating cokernel_blob.h..."

COKERNEL_BIN="${PROJECT_DIR}/cokernel/cokernel.bin"
BLOB_HEADER="${PROJECT_DIR}/module/cokernel_blob.h"

if [ ! -f "${COKERNEL_BIN}" ]; then
    echo "ERROR: cokernel.bin not found"
    exit 1
fi

# Generate C array from binary
{
    echo "/* Auto-generated — do not edit */"
    echo "/* Co-kernel binary blob */"
    echo ""
    xxd -i "${COKERNEL_BIN}" | \
        sed "s|unsigned char .*\[\]|static const unsigned char cokernel_bin[]|" | \
        sed "s|unsigned int .*_len|static const unsigned int cokernel_bin_len|"
} > "${BLOB_HEADER}"

echo "Generated ${BLOB_HEADER} ($(wc -c < "${COKERNEL_BIN}") bytes payload)"
echo ""

# ── Step 3: Build kernel module ─────────────────────────────────
echo "[3/3] Building kernel module against ${KVER}..."
make -C "${KDIR}" M="${PROJECT_DIR}/module" modules

echo ""

# Verify output
if [ -f "${PROJECT_DIR}/module/parasite.ko" ]; then
    echo "=== Build complete ==="
    echo "Module: ${PROJECT_DIR}/module/parasite.ko"
    echo "Size:   $(du -h "${PROJECT_DIR}/module/parasite.ko" | cut -f1)"
    modinfo "${PROJECT_DIR}/module/parasite.ko" 2>/dev/null || true
    if [ -f "${PROJECT_DIR}/module/ck_reader.ko" ]; then
        echo "Reader: ${PROJECT_DIR}/module/ck_reader.ko"
        echo "Size:   $(du -h "${PROJECT_DIR}/module/ck_reader.ko" | cut -f1)"
    fi
else
    echo "ERROR: parasite.ko not produced"
    exit 1
fi
