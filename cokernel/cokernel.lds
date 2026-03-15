/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Linker script for co-kernel flat binary.
 *
 * Produces a flat binary loaded at COKERNEL_VIRT_BASE (0xFFFF800000000000).
 * The loader copies this into hidden pages that are mapped at this address
 * in the co-kernel's own page tables.
 *
 * Output: cokernel.bin (raw binary, no ELF headers)
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)

SECTIONS
{
    /* Virtual address where the co-kernel will be mapped */
    . = 0xFFFF800000000000;

    .text : {
        _text_start = .;
        *(.text)
        *(.text.*)
        _text_end = .;
    }

    . = ALIGN(4096);

    .rodata : {
        _rodata_start = .;
        *(.rodata)
        *(.rodata.*)
        _rodata_end = .;
    }

    . = ALIGN(4096);

    .data : {
        _data_start = .;
        *(.data)
        *(.data.*)
        _data_end = .;
    }

    . = ALIGN(4096);

    .bss : {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        _bss_end = .;
    }

    _binary_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame*)
    }
}
