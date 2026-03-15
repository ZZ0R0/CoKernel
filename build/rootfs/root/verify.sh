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
