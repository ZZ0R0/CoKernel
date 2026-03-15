/* SPDX-License-Identifier: GPL-2.0 */
/*
 * resolve.c — Runtime kernel symbol resolution via kprobes
 *
 * Uses the kprobe registration mechanism to resolve symbols even
 * when KASLR is active and kallsyms_lookup_name is not exported.
 */

#include <linux/kprobes.h>

#include "resolve.h"

/*
 * ck_resolve_symbol — Resolve a kernel symbol address.
 *
 * Technique: register a kprobe on the symbol name, read its resolved
 * address, then immediately unregister. This is KASLR-safe because
 * kprobes internally uses kallsyms to resolve the name.
 */
unsigned long ck_resolve_symbol(const char *name)
{
    struct kprobe kp = {};
    unsigned long addr = 0;
    int ret;

    kp.symbol_name = name;

    ret = register_kprobe(&kp);
    if (ret < 0) {
        pr_warn("cokernel: failed to resolve '%s': %d\n", name, ret);
        return 0;
    }

    addr = (unsigned long)kp.addr;
    unregister_kprobe(&kp);

    pr_info("cokernel: resolved '%s' → 0x%lx\n", name, addr);
    return addr;
}
