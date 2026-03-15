#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0xedc03953, "iounmap" },
	{ 0x94961283, "vunmap" },
	{ 0x37a0cba, "kfree" },
	{ 0xbdfb6dbb, "__fentry__" },
	{ 0x65487097, "__x86_indirect_thunk_rax" },
	{ 0x92997ed8, "_printk" },
	{ 0xf0fdf6cb, "__stack_chk_fail" },
	{ 0x3d94b075, "__free_pages" },
	{ 0x7cd8d75e, "page_offset_base" },
	{ 0x4c9d28b0, "phys_base" },
	{ 0x8f0831b6, "vmap" },
	{ 0x950eb34e, "__list_del_entry_valid_or_report" },
	{ 0xde80cd09, "ioremap" },
	{ 0xd38cd261, "__default_kernel_pte_mask" },
	{ 0x5b8239ca, "__x86_return_thunk" },
	{ 0x62177c31, "pv_ops" },
	{ 0x97651e6c, "vmemmap_base" },
	{ 0xd5c8d765, "kobject_del" },
	{ 0x3f66a26e, "register_kprobe" },
	{ 0xddf890c1, "alloc_pages_noprof" },
	{ 0xbb10e61d, "unregister_kprobe" },
	{ 0xccf69887, "get_free_pages_noprof" },
	{ 0xd4ec10e6, "BUG_func" },
	{ 0xf9a482f9, "msleep" },
	{ 0x8a35b432, "sme_me_mask" },
	{ 0x52c5c991, "__kmalloc_noprof" },
	{ 0xbf1981cb, "module_layout" },
};

MODULE_INFO(depends, "");

