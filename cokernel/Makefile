# SPDX-License-Identifier: GPL-2.0
#
# Makefile for co-kernel flat binary
#

CC      := gcc
LD      := ld
OBJCOPY := objcopy

# GCC ships freestanding headers (stdint.h, stddef.h, etc.) — we need them
GCC_INCLUDES := -isystem $(shell $(CC) -print-file-name=include)

CFLAGS  := -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
           -mno-red-zone -mno-sse -mno-mmx -mno-avx \
           -nostdlib -nostdinc $(GCC_INCLUDES) -O2 -Wall -Wextra \
           -I../include \
           -mcmodel=large

LDFLAGS := -T cokernel.lds -nostdlib

SRCS    := cokernel.c
OBJS    := $(SRCS:.c=.o)
TARGET  := cokernel.bin
TARGET_ELF := cokernel.elf

.PHONY: all clean

all: $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET_ELF): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

$(TARGET): $(TARGET_ELF)
	$(OBJCOPY) -O binary $< $@
	@echo "Co-kernel binary: $@ ($$(stat -c%s $@) bytes)"

clean:
	rm -f $(OBJS) $(TARGET_ELF) $(TARGET)
