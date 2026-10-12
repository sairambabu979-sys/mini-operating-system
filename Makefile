# SPDX-License-Identifier: MIT
#
# Top-level, non-recursive build. Everything lands in $(BUILD).
#
#   make                 build the kernel
#   make test            host unit tests
#   make lint            clang-tidy over kernel sources
#   make format          reformat all C sources in place
#   make check-format    fail if any C source is not clang-formatted
#
# CONFIG=debug (default) or CONFIG=release.

.SUFFIXES:
.DELETE_ON_ERROR:
MAKEFLAGS += --no-builtin-rules --no-builtin-variables

CONFIG  ?= debug
BUILD   ?= build/$(CONFIG)

CC      := clang
LD      := ld.lld
NASM    := nasm
OBJCOPY := llvm-objcopy
HOSTCC  := gcc
PYTHON  := python3

# Quiet output by default ("  CC  file.c"); `make V=1` prints full commands.
ifeq ($(V),1)
Q :=
msg = @:
else
Q := @
msg = @printf '  %-7s %s\n' $(1) $(2);
endif

ifeq ($(filter $(CONFIG),debug release),)
$(error CONFIG must be 'debug' or 'release', got '$(CONFIG)')
endif

# --------------------------------------------------------------------------
# Kernel flags. See docs/PLAN.md §6 for why each one is here.
# --------------------------------------------------------------------------

CLANG_RESOURCE_DIR := $(shell $(CC) -print-resource-dir)

KERNEL_INCLUDES := -Ikernel/include -Ikernel/arch/x86_64/include -I$(BUILD)/gen

KERNEL_CFLAGS := \
	--target=x86_64-unknown-none-elf -std=c17 \
	-ffreestanding -nostdinc -isystem $(CLANG_RESOURCE_DIR)/include \
	-mcmodel=kernel -mno-red-zone -mgeneral-regs-only -fno-pic -fno-pie \
	-fno-omit-frame-pointer -fno-strict-aliasing -fno-common \
	-fno-asynchronous-unwind-tables -fno-unwind-tables -fno-stack-protector \
	-Wall -Wextra -Werror -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
	-Wvla -Wcast-align -Wpointer-arith -Wundef -Wimplicit-fallthrough \
	-ffile-prefix-map=$(CURDIR)/= \
	$(KERNEL_INCLUDES)

ifeq ($(CONFIG),debug)
KERNEL_CFLAGS += -O2 -g -DCONFIG_DEBUG=1
else
KERNEL_CFLAGS += -O2 -DNDEBUG
endif

KERNEL_ASFLAGS := -f elf64 -w+all -Ikernel/arch/x86_64/ -I$(BUILD)/gen/
KERNEL_LDFLAGS := -nostdlib -static -z max-page-size=0x1000 -z noexecstack \
	--build-id=none -T kernel/linker.ld

# --------------------------------------------------------------------------
# Kernel
# --------------------------------------------------------------------------

KERNEL_C_SRCS   := $(sort $(shell find kernel -name '*.c'))
KERNEL_ASM_SRCS := $(sort $(shell find kernel -name '*.asm'))
KERNEL_OBJS     := $(KERNEL_C_SRCS:%.c=$(BUILD)/%.o) $(KERNEL_ASM_SRCS:%.asm=$(BUILD)/%.asm.o)

KERNEL_ELF := $(BUILD)/kernel.elf

.PHONY: all
all: $(KERNEL_ELF)

$(BUILD)/kernel/%.o: kernel/%.c
	@mkdir -p $(@D)
	$(call msg,CC,$<)
	$(Q)$(CC) $(KERNEL_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/kernel/%.asm.o: kernel/%.asm
	@mkdir -p $(@D)
	$(call msg,NASM,$<)
	$(Q)$(NASM) $(KERNEL_ASFLAGS) -MD $(@:.o=.d) -MP $< -o $@

$(KERNEL_ELF): $(KERNEL_OBJS) kernel/linker.ld
	$(call msg,LD,$@)
	$(Q)$(LD) $(KERNEL_LDFLAGS) $(KERNEL_OBJS) -o $@

# --------------------------------------------------------------------------
# Host unit tests: pure kernel modules compiled for Linux under ASan/UBSan.
# --------------------------------------------------------------------------

HOST_TEST_KERNEL_SRCS := kernel/lib/string.c kernel/lib/printf.c kernel/lib/cmdline.c
HOST_TEST_SRCS        := $(sort $(wildcard tests/host/*.c))
HOST_TEST_BIN         := $(BUILD)/tests/host/run_host_tests

HOST_CFLAGS := -std=gnu17 -O1 -g -fno-omit-frame-pointer -fno-builtin \
	-fsanitize=address,undefined -fno-sanitize-recover=all \
	-Wall -Wextra -Werror -Ikernel/include

HOST_TEST_OBJS := $(HOST_TEST_SRCS:%.c=$(BUILD)/host/%.o) \
	$(HOST_TEST_KERNEL_SRCS:%.c=$(BUILD)/host/%.o)

$(BUILD)/host/tests/%.o: tests/%.c
	@mkdir -p $(@D)
	$(call msg,HOSTCC,$<)
	$(Q)$(HOSTCC) $(HOST_CFLAGS) -MMD -MP -c $< -o $@

# Kernel sources get their libc-named symbols renamed (see kernel_shim.h).
$(BUILD)/host/kernel/%.o: kernel/%.c
	@mkdir -p $(@D)
	$(call msg,HOSTCC,$<)
	$(Q)$(HOSTCC) $(HOST_CFLAGS) -include tests/host/kernel_shim.h -MMD -MP -c $< -o $@

$(HOST_TEST_BIN): $(HOST_TEST_OBJS)
	@mkdir -p $(@D)
	$(call msg,HOSTLD,$@)
	$(Q)$(HOSTCC) $(HOST_CFLAGS) $^ -o $@

.PHONY: test host-test
test: host-test

host-test: $(HOST_TEST_BIN)
	$(Q)$(HOST_TEST_BIN)

# --------------------------------------------------------------------------
# Code quality
# --------------------------------------------------------------------------

FORMAT_SRCS := $(shell find kernel tests -name '*.c' -o -name '*.h')

.PHONY: format check-format lint
format:
	$(Q)clang-format -i $(FORMAT_SRCS)

check-format:
	$(Q)clang-format --dry-run --Werror $(FORMAT_SRCS)

lint:
	$(Q)clang-tidy --quiet $(KERNEL_C_SRCS) -- $(KERNEL_CFLAGS)

.PHONY: clean
clean:
	rm -rf build

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
