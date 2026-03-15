/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ck_verify.c — Userspace verification tool for the co-kernel
 *
 * Reads the shared communication page via /dev/mem and displays
 * the co-kernel's reported state. This proves the co-kernel is alive
 * and can read Linux's memory via the direct map.
 *
 * Usage:
 *   ck_verify <comm_page_phys_hex>              — display status
 *   ck_verify <comm_page_phys_hex> <output_file> — display + save to file
 *
 * Returns 0 if magic is valid, 1 otherwise.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>

#define COKERNEL_COMM_MAGIC  0x434B434F4D4D0001ULL

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

/*
 * Read the comm page via pread() on /dev/mem.
 * This avoids mmap which can be blocked by CONFIG_IO_STRICT_DEVMEM
 * on Debian kernels even with iomem=relaxed.
 */
static int read_comm_page_pread(int fd, uint64_t phys, struct ck_comm_page *out)
{
    ssize_t n;
    off_t off = (off_t)phys;

    n = pread(fd, out, sizeof(*out), off);
    if (n < 0)
        return -1;
    if ((size_t)n < sizeof(*out)) {
        errno = EIO;
        return -1;
    }
    return 0;
}

/*
 * Read the comm page via mmap() on /dev/mem.
 * Preferred method but may fail on strict kernels.
 */
static int read_comm_page_mmap(int fd, uint64_t phys, struct ck_comm_page *out)
{
    void *map;
    off_t page_off = (off_t)(phys & ~0xFFFUL);
    size_t page_delta = (size_t)(phys & 0xFFF);

    map = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, page_off);
    if (map == MAP_FAILED)
        return -1;

    memcpy(out, (char *)map + page_delta, sizeof(*out));
    munmap(map, 4096);
    return 0;
}

static void display_comm_page(const struct ck_comm_page *cp, uint64_t phys)
{
    int valid = (cp->magic == COKERNEL_COMM_MAGIC);

    printf("=== Co-kernel Comm Page @ 0x%lx ===\n", (unsigned long)phys);
    printf("magic:      0x%016lx %s\n", (unsigned long)cp->magic,
           valid ? "(valid)" : "(INVALID)");
    printf("version:    %lu\n", (unsigned long)cp->version);
    printf("tick_count: %lu\n", (unsigned long)cp->tick_count);
    printf("last_tsc:   %lu\n", (unsigned long)cp->last_tsc);
    printf("status:     %u (%s)\n", cp->status,
           cp->status == 0 ? "init" :
           cp->status == 1 ? "running" : "error");
    printf("data_seq:   %u\n", cp->data_seq);
    printf("init_comm:  %.16s\n", cp->init_comm);
}

int main(int argc, char **argv)
{
    uint64_t phys;
    int fd;
    struct ck_comm_page cp;
    int valid;
    const char *method;

    if (argc < 2) {
        fprintf(stderr, "usage: ck_verify <comm_page_phys_hex> [output_file]\n");
        return 1;
    }

    phys = strtoull(argv[1], NULL, 16);
    if (phys == 0) {
        fprintf(stderr, "error: invalid physical address\n");
        return 1;
    }

    fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) {
        perror("/dev/mem");
        return 1;
    }

    memset(&cp, 0, sizeof(cp));

    /* Try pread first (works on strict kernels), fall back to mmap */
    if (read_comm_page_pread(fd, phys, &cp) == 0) {
        method = "pread";
    } else {
        fprintf(stderr, "pread failed: %s, trying mmap...\n", strerror(errno));
        if (read_comm_page_mmap(fd, phys, &cp) == 0) {
            method = "mmap";
        } else {
            fprintf(stderr, "mmap also failed: %s\n", strerror(errno));
            close(fd);
            return 1;
        }
    }

    printf("[read via %s]\n", method);
    display_comm_page(&cp, phys);

    valid = (cp.magic == COKERNEL_COMM_MAGIC);

    /* Optional: write results to file */
    if (argc > 2) {
        FILE *f = fopen(argv[2], "w");
        if (f) {
            fprintf(f, "magic=0x%016lx\n", (unsigned long)cp.magic);
            fprintf(f, "version=%lu\n", (unsigned long)cp.version);
            fprintf(f, "tick_count=%lu\n", (unsigned long)cp.tick_count);
            fprintf(f, "status=%u\n", cp.status);
            fprintf(f, "init_comm=%.16s\n", cp.init_comm);
            fclose(f);
            printf("Written to %s\n", argv[2]);
        } else {
            perror(argv[2]);
        }
    }

    close(fd);
    return valid ? 0 : 1;
}
