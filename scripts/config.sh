#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# config.sh — Shared configuration for all build/run scripts
#
# KVER selects which Debian kernel to use for:
#   - Compiling the module (against /lib/modules/$KVER/build)
#   - Booting QEMU        (with /boot/vmlinuz-$KVER)
#
# Override: KVER=6.12.57+deb13-amd64 make all
#
# If not set, auto-detects the latest installed kernel in /boot/.
#

if [ -z "${KVER:-}" ]; then
    # Auto-detect: pick the latest vmlinuz in /boot
    KVER="$(ls -1 /boot/vmlinuz-* 2>/dev/null \
            | sed 's|/boot/vmlinuz-||' \
            | sort -V \
            | tail -1)"
    if [ -z "${KVER}" ]; then
        echo "ERROR: No kernel found in /boot/"
        exit 1
    fi
fi

export KVER
export KDIR="/lib/modules/${KVER}/build"
export BZIMAGE="/boot/vmlinuz-${KVER}"
