/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ck_reader.c — Diagnostic reader for co-kernel comm page
 *
 * This tiny module reads the shared communication page via phys_to_virt()
 * (the page is kept in Linux's direct map), prints its contents to dmesg,
 * then returns an error so it auto-unloads immediately.
 *
 * Usage from init script:
 *   insmod /lib/modules/ck_reader.ko comm_phys=0x5bff000
 *   dmesg | grep ck_reader
 *
 * The module can be loaded repeatedly (it never stays loaded).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/io.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Co-kernel comm page diagnostic reader");

static unsigned long comm_phys;
module_param(comm_phys, ulong, 0444);
MODULE_PARM_DESC(comm_phys, "Physical address of the comm page");

#define COKERNEL_COMM_MAGIC  0x434B434F4D4D0001ULL
#define MAX_SNAPSHOT_PROCS   124

struct ck_process_entry {
	int32_t  pid;
	int32_t  tgid;
	uint32_t uid;
	uint32_t reserved;
	char     comm[16];
};

struct ck_comm_page {
	uint64_t magic;
	uint64_t version;
	uint64_t tick_count;
	uint64_t last_tsc;
	uint32_t status;
	uint32_t data_seq;
	char     init_comm[16];
	uint8_t  data_buf[4000];
};

static int __init ck_reader_init(void)
{
	volatile struct ck_comm_page *cp;

	if (!comm_phys) {
		pr_err("ck_reader: comm_phys parameter required\n");
		return -EINVAL;
	}

	cp = (volatile struct ck_comm_page *)phys_to_virt(comm_phys);

	pr_info("ck_reader: === Comm Page @ phys 0x%lx (virt %px) ===\n",
		comm_phys, cp);
	pr_info("ck_reader: magic=0x%016llx %s\n",
		cp->magic,
		cp->magic == COKERNEL_COMM_MAGIC ? "(valid)" : "(INVALID)");
	pr_info("ck_reader: version=%llu\n", cp->version);
	pr_info("ck_reader: tick_count=%llu\n", cp->tick_count);
	pr_info("ck_reader: last_tsc=%llu\n", cp->last_tsc);
	pr_info("ck_reader: status=%u\n", cp->status);
	pr_info("ck_reader: data_seq=%u\n", cp->data_seq);
	pr_info("ck_reader: init_comm=%.16s\n", cp->init_comm);

	/* V2: decode process snapshot from data_buf */
	{
		uint32_t nr_tasks = *(uint32_t *)&cp->data_buf[0];
		const struct ck_process_entry *entries =
			(const struct ck_process_entry *)&cp->data_buf[8];
		unsigned int i;

		pr_info("ck_reader: --- Process Snapshot (nr_tasks=%u) ---\n",
			nr_tasks);

		if (nr_tasks > MAX_SNAPSHOT_PROCS)
			nr_tasks = MAX_SNAPSHOT_PROCS;

		for (i = 0; i < nr_tasks; i++) {
			pr_info("ck_reader:   [%4d] %.16s (uid=%u)\n",
				entries[i].pid,
				entries[i].comm,
				entries[i].uid);
		}

		pr_info("ck_reader: --- End Snapshot ---\n");
	}

	/* Return error to auto-unload — we only need the printk output */
	return -ENODEV;
}

module_init(ck_reader_init);
