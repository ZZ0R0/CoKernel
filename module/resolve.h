/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _COKERNEL_RESOLVE_H
#define _COKERNEL_RESOLVE_H

#include <linux/types.h>

/*
 * Bootstrap the symbol resolver. Must be called once before
 * ck_resolve_symbol(). Uses kprobes to obtain kallsyms_lookup_name.
 */
int ck_resolve_init(void);

/*
 * Resolve a kernel symbol address at runtime.
 * Works for both function and data symbols (init_task, max_pfn, etc.).
 *
 * @name: symbol name (e.g., "init_task")
 * Returns: address of the symbol, or 0 on failure.
 */
unsigned long ck_resolve_symbol(const char *name);

#endif /* _COKERNEL_RESOLVE_H */
