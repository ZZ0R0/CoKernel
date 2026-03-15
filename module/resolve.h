/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _COKERNEL_RESOLVE_H
#define _COKERNEL_RESOLVE_H

#include <linux/types.h>

/*
 * Resolve a kernel symbol address at runtime via kprobes.
 * This bypasses KASLR since kprobes resolves from kallsyms internally.
 *
 * @name: symbol name (e.g., "tick_handle_periodic")
 * Returns: address of the symbol, or 0 on failure.
 */
unsigned long ck_resolve_symbol(const char *name);

#endif /* _COKERNEL_RESOLVE_H */
