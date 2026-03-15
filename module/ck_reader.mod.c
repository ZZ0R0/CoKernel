#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
	.arch = MODULE_ARCH_INIT,
};



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x92997ed8, "_printk" },
	{ 0x7cd8d75e, "page_offset_base" },
	{ 0x5b8239ca, "__x86_return_thunk" },
	{ 0x755ebfa4, "param_ops_ulong" },
	{ 0xbdfb6dbb, "__fentry__" },
	{ 0xbf1981cb, "module_layout" },
};

MODULE_INFO(depends, "");

