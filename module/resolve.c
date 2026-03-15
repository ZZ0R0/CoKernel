/* SPDX-License-Identifier: GPL-2.0 */
/*
 * resolve.c — Runtime kernel symbol resolution
 *
 * Two-stage approach:
 *   1. Use kprobes to resolve kallsyms_lookup_name (a function symbol)
 *   2. Use kallsyms_lookup_name to resolve any symbol (including data)
 *
 * kprobes alone cannot resolve data symbols like init_task or max_pfn.
 */

#include <linux/kprobes.h>

#include "resolve.h"

typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);
static kallsyms_lookup_name_t fn_kallsyms_lookup_name;

/*
 * ck_resolve_init — Bootstrap: resolve kallsyms_lookup_name via kprobes.
 * Must be called once before any ck_resolve_symbol() call.
 */
int ck_resolve_init(void)
{
    struct kprobe kp = {};
    int ret;

    if (fn_kallsyms_lookup_name)
        return 0;  /* already initialized */

    kp.symbol_name = "kallsyms_lookup_name";
    ret = register_kprobe(&kp);
    if (ret < 0) {
        pr_err("cokernel: failed to resolve kallsyms_lookup_name: %d\n", ret);
        return ret;
    }

    fn_kallsyms_lookup_name = (kallsyms_lookup_name_t)kp.addr;
    unregister_kprobe(&kp);

    pr_info("cokernel: kallsyms_lookup_name → 0x%lx\n",
            (unsigned long)fn_kallsyms_lookup_name);
    return 0;
}

/*
 * ck_resolve_symbol — Resolve any kernel symbol address (functions + data).
 *
 * Uses kallsyms_lookup_name obtained via kprobe bootstrap.
 * Falls back to kprobe-only for function symbols if bootstrap failed.
 */
unsigned long ck_resolve_symbol(const char *name)
{
    unsigned long addr;

    if (fn_kallsyms_lookup_name) {
        addr = fn_kallsyms_lookup_name(name);
        if (addr) {
            pr_info("cokernel: resolved '%s' → 0x%lx\n", name, addr);
            return addr;
        }
    }

    /* Fallback: try kprobe (only works for function symbols) */
    {
        struct kprobe kp = {};
        int ret;

        kp.symbol_name = name;
        ret = register_kprobe(&kp);
        if (ret < 0) {
            pr_warn("cokernel: failed to resolve '%s': %d\n", name, ret);
            return 0;
        }

        addr = (unsigned long)kp.addr;
        unregister_kprobe(&kp);

        pr_info("cokernel: resolved '%s' → 0x%lx (kprobe)\n", name, addr);
        return addr;
    }
}
