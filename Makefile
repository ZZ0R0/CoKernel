# SPDX-License-Identifier: GPL-2.0
#
# Top-level Makefile for Co-kernel PoC
#
# Uses a Debian-packaged kernel — no kernel compilation needed.
# The target kernel version is auto-detected from /boot/ or overridden:
#   KVER=6.12.57+deb13-amd64 make all
#
# Targets:
#   make module    — Build co-kernel binary + parasite.ko
#   make rootfs    — Build busybox-based initramfs
#   make run       — Launch QEMU VM with Debian kernel
#   make all       — Full build: module → rootfs
#   make clean     — Clean build artifacts
#

SHELL    := /bin/bash
SCRIPTS  := scripts

.PHONY: all rootfs module run clean help

help:
	@echo "Co-kernel PoC — Build System"
	@echo ""
	@echo "  make module   Build co-kernel binary + parasite.ko"
	@echo "  make rootfs   Build initramfs (includes module if built)"
	@echo "  make run      Launch QEMU VM with Debian kernel"
	@echo "  make all      Full build: module → rootfs"
	@echo "  make clean    Remove build artifacts"
	@echo ""
	@echo "Override kernel: KVER=x.y.z make all"
	@echo ""
	@echo "Available kernels:"
	@ls -1 /boot/vmlinuz-* 2>/dev/null | sed 's|/boot/vmlinuz-|  |' || echo "  (none)"

all: module rootfs
	@echo ""
	@echo "=== Full build complete ==="
	@echo "Run 'make run' to launch the VM."

module:
	@chmod +x $(SCRIPTS)/build_module.sh
	$(SCRIPTS)/build_module.sh

rootfs:
	@chmod +x $(SCRIPTS)/build_rootfs.sh
	$(SCRIPTS)/build_rootfs.sh

run:
	@chmod +x $(SCRIPTS)/run_qemu.sh
	$(SCRIPTS)/run_qemu.sh

clean:
	rm -rf build/
	$(MAKE) -C cokernel clean
	$(MAKE) -C module clean KDIR=/dev/null 2>/dev/null || true
	rm -f module/cokernel_blob.h
	@echo "Clean complete."
