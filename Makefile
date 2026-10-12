# SPDX-License-Identifier: MIT
#
# Top-level, non-recursive build. Everything lands in $(BUILD).
#
#   make                 build the kernel and disk images
#   make run             boot build/<config>/disk.img in QEMU (CMDLINE="..." to pass options)
#   make debug           same, halted at reset waiting for gdb on :1234
#   make test            host unit tests, binary checks, in-kernel tests under QEMU
#   make lint            clang-tidy over kernel sources
#   make format          reformat all C sources in place
#   make check-format    fail if any C source is not clang-formatted
#
# CONFIG=debug (default) or CONFIG=release.

.DEFAULT_GOAL := all
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
	-fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fstack-protector-strong -mstack-protector-guard=global \
	-Wall -Wextra -Werror -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
	-Wvla -Wcast-align -Wpointer-arith -Wundef -Wimplicit-fallthrough \
	-ffile-prefix-map=$(CURDIR)/= \
	$(KERNEL_INCLUDES)

ifeq ($(CONFIG),debug)
KERNEL_CFLAGS += -O2 -g -DCONFIG_DEBUG=1
# Kernel UBSan; the runtime is kernel/core/ubsan.c. -fsanitize=function needs
# type metadata a freestanding kernel does not provide. Handlers are the
# recoverable variants: they panic by default, but tests can observe them.
KERNEL_UBSAN_FLAGS := -fsanitize=undefined -fno-sanitize=function
KERNEL_CFLAGS += $(KERNEL_UBSAN_FLAGS)
else
KERNEL_CFLAGS += -O2 -DNDEBUG
endif

# Host unit-test flags: kernel library code built for Linux under ASan/UBSan.
HOST_CFLAGS := -std=gnu17 -O1 -g -fno-omit-frame-pointer -fno-builtin \
	-fsanitize=address,undefined -fno-sanitize-recover=all \
	-Wall -Wextra -Werror -Ikernel/include

# Rebuild everything when the flags change, not only when sources do.
FLAGS_STAMP := $(BUILD)/flags.stamp
FLAGS_SIG   := $(CC) $(KERNEL_CFLAGS) | $(HOSTCC) $(HOST_CFLAGS)
$(shell mkdir -p $(BUILD) && { printf '%s' '$(FLAGS_SIG)' | cmp -s - $(FLAGS_STAMP) || \
	printf '%s' '$(FLAGS_SIG)' > $(FLAGS_STAMP); })

KERNEL_ASFLAGS := -f elf64 -w+all -Ikernel/arch/x86_64/ -I$(BUILD)/gen/
KERNEL_LDFLAGS := -nostdlib -static -z max-page-size=0x1000 -z noexecstack \
	--build-id=none -T kernel/linker.ld

# --------------------------------------------------------------------------
# Generated headers: C struct layouts exported to NASM (kernel/asm-offsets.c)
# --------------------------------------------------------------------------

ASM_OFFSETS := $(BUILD)/gen/asm-offsets.inc

$(BUILD)/gen/asm-offsets.s: kernel/asm-offsets.c $(FLAGS_STAMP)
	@mkdir -p $(@D)
	$(call msg,GEN,$@)
	$(Q)$(CC) $(KERNEL_CFLAGS) -MMD -MP -MT $@ -S $< -o $@

$(ASM_OFFSETS): $(BUILD)/gen/asm-offsets.s
	$(call msg,GEN,$@)
	$(Q){ echo '; Generated from kernel/asm-offsets.c. Do not edit.'; \
	  sed -n 's/^.*->\([A-Za-z0-9_]*\) \(-\{0,1\}[0-9][0-9]*\).*$$/%define \1 \2/p' $<; } > $@

# --------------------------------------------------------------------------
# Kernel
# --------------------------------------------------------------------------

KERNEL_C_SRCS   := $(sort $(filter-out kernel/asm-offsets.c,$(shell find kernel -name '*.c')) \
                          $(wildcard tests/kernel/*.c))
KERNEL_ASM_SRCS := $(sort $(shell find kernel -name '*.asm'))
KERNEL_OBJS     := $(KERNEL_C_SRCS:%.c=$(BUILD)/%.o) $(KERNEL_ASM_SRCS:%.asm=$(BUILD)/%.asm.o)

# kernel.elf keeps debug info for gdb; the stripped copy is what goes on disk.
KERNEL_ELF      := $(BUILD)/kernel.elf
KERNEL_STRIPPED := $(BUILD)/kernel.stripped.elf

KERNEL_C_OBJS := $(KERNEL_C_SRCS:%.c=$(BUILD)/%.o)

$(KERNEL_C_OBJS): $(BUILD)/%.o: %.c $(FLAGS_STAMP)
	@mkdir -p $(@D)
	$(call msg,CC,$<)
	$(Q)$(CC) $(KERNEL_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/kernel/%.asm.o: kernel/%.asm | $(ASM_OFFSETS)
	@mkdir -p $(@D)
	$(call msg,NASM,$<)
	$(Q)$(NASM) $(KERNEL_ASFLAGS) -MD $(@:.o=.d) -MP $< -o $@

# Code that UBSan reports through must not itself be instrumented.
$(BUILD)/kernel/core/ubsan.o: KERNEL_CFLAGS += -fno-sanitize=all

# Symbol table: link once with an empty table, generate the real one from
# that binary, link again, then prove no function moved (tools/gen_ksyms.py).
KERNEL_PASS1 := $(BUILD)/kernel.pass1.elf
KSYMS_EMPTY  := $(BUILD)/gen/ksyms_empty.c
KSYMS        := $(BUILD)/gen/ksyms.c

$(KSYMS_EMPTY): tools/gen_ksyms.py
	@mkdir -p $(@D)
	$(call msg,GEN,$@)
	$(Q)$(PYTHON) tools/gen_ksyms.py --empty -o $@

$(KSYMS): $(KERNEL_PASS1) tools/gen_ksyms.py
	$(call msg,GEN,$@)
	$(Q)$(PYTHON) tools/gen_ksyms.py $< -o $@

$(BUILD)/gen/%.o: $(BUILD)/gen/%.c $(FLAGS_STAMP)
	$(call msg,CC,$<)
	$(Q)$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_PASS1): $(KERNEL_OBJS) $(BUILD)/gen/ksyms_empty.o kernel/linker.ld
	$(call msg,LD,$@)
	$(Q)$(LD) $(KERNEL_LDFLAGS) $(KERNEL_OBJS) $(BUILD)/gen/ksyms_empty.o -o $@

$(KERNEL_ELF): $(KERNEL_OBJS) $(BUILD)/gen/ksyms.o kernel/linker.ld
	$(call msg,LD,$@)
	$(Q)$(LD) $(KERNEL_LDFLAGS) $(KERNEL_OBJS) $(BUILD)/gen/ksyms.o -o $@
	$(Q)$(PYTHON) tools/gen_ksyms.py --check $@ $(KSYMS)

$(KERNEL_STRIPPED): $(KERNEL_ELF)
	$(call msg,STRIP,$@)
	$(Q)$(OBJCOPY) --strip-all $< $@

# --------------------------------------------------------------------------
# Bootloader and disk images
# --------------------------------------------------------------------------

BOOT_ASFLAGS := -f bin -w+all -Iboot/inc/ -I$(BUILD)/gen/
STAGE1 := $(BUILD)/boot/stage1.bin
STAGE2 := $(BUILD)/boot/stage2.bin

$(BUILD)/boot/%.bin: boot/%.asm | $(ASM_OFFSETS)
	@mkdir -p $(@D)
	$(call msg,NASM,$<)
	$(Q)$(NASM) $(BOOT_ASFLAGS) -MD $(@:.bin=.d) -MP -MT $@ $< -o $@

# Same kernel binary in both images; only the command line differs, so the
# tests exercise exactly what `make run` boots.
DISK_IMG := $(BUILD)/disk.img
TEST_IMG := $(BUILD)/disk-ktest.img
CMDLINE  ?=

PANIC_IMG := $(BUILD)/disk-panic.img

$(PANIC_IMG): $(STAGE1) $(STAGE2) $(KERNEL_STRIPPED) tools/mkimage.py
	$(call msg,IMAGE,$@)
	$(Q)$(PYTHON) tools/mkimage.py --stage1 $(STAGE1) --stage2 $(STAGE2) \
	  --kernel $(KERNEL_STRIPPED) --cmdline "selftest=panic" -o $@

# The run image embeds $(CMDLINE); this stamp changes only when it does.
CMDLINE_STAMP := $(BUILD)/cmdline.stamp
$(shell mkdir -p $(BUILD) && { printf '%s' '$(CMDLINE)' | cmp -s - $(CMDLINE_STAMP) || \
	printf '%s' '$(CMDLINE)' > $(CMDLINE_STAMP); })

$(DISK_IMG): $(STAGE1) $(STAGE2) $(KERNEL_STRIPPED) tools/mkimage.py $(CMDLINE_STAMP)
	$(call msg,IMAGE,$@)
	$(Q)$(PYTHON) tools/mkimage.py --stage1 $(STAGE1) --stage2 $(STAGE2) \
	  --kernel $(KERNEL_STRIPPED) --cmdline "$(CMDLINE)" -o $@

$(TEST_IMG): $(STAGE1) $(STAGE2) $(KERNEL_STRIPPED) tools/mkimage.py
	$(call msg,IMAGE,$@)
	$(Q)$(PYTHON) tools/mkimage.py --stage1 $(STAGE1) --stage2 $(STAGE2) \
	  --kernel $(KERNEL_STRIPPED) --cmdline "ktest" -o $@

.PHONY: all
all: $(DISK_IMG) $(TEST_IMG) $(PANIC_IMG) $(KERNEL_ELF)

# --------------------------------------------------------------------------
# Running
# --------------------------------------------------------------------------

QEMU       := qemu-system-x86_64
QEMU_FLAGS := -m 128M -cpu max -no-reboot -serial stdio -display none -monitor none \
              -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
              -drive file=$(DISK_IMG),format=raw,if=ide

.PHONY: run debug
run: $(DISK_IMG)
	$(QEMU) $(QEMU_FLAGS) $(QEMU_EXTRA)

# Waits for gdb on :1234 before the first instruction. In another shell:
#   gdb -x tools/gdb/gdbinit $(KERNEL_ELF)
debug: $(DISK_IMG) $(KERNEL_ELF)
	$(QEMU) $(QEMU_FLAGS) -s -S -d int,cpu_reset -D $(BUILD)/qemu-debug.log $(QEMU_EXTRA)

# --------------------------------------------------------------------------
# Host unit tests: pure kernel modules compiled for Linux under ASan/UBSan.
# --------------------------------------------------------------------------

HOST_TEST_KERNEL_SRCS := kernel/lib/string.c kernel/lib/printf.c kernel/lib/cmdline.c
HOST_TEST_SRCS        := $(sort $(wildcard tests/host/*.c))
HOST_TEST_BIN         := $(BUILD)/tests/host/run_host_tests


HOST_TEST_OBJS := $(HOST_TEST_SRCS:%.c=$(BUILD)/host/%.o) \
	$(HOST_TEST_KERNEL_SRCS:%.c=$(BUILD)/host/%.o)

$(BUILD)/host/tests/%.o: tests/%.c $(FLAGS_STAMP)
	@mkdir -p $(@D)
	$(call msg,HOSTCC,$<)
	$(Q)$(HOSTCC) $(HOST_CFLAGS) -MMD -MP -c $< -o $@

# Kernel sources get their libc-named symbols renamed (see kernel_shim.h).
$(BUILD)/host/kernel/%.o: kernel/%.c $(FLAGS_STAMP)
	@mkdir -p $(@D)
	$(call msg,HOSTCC,$<)
	$(Q)$(HOSTCC) $(HOST_CFLAGS) -include tests/host/kernel_shim.h -MMD -MP -c $< -o $@

$(HOST_TEST_BIN): $(HOST_TEST_OBJS)
	@mkdir -p $(@D)
	$(call msg,HOSTLD,$@)
	$(Q)$(HOSTCC) $(HOST_CFLAGS) $^ -o $@

.PHONY: test host-test qemu-test check-binary
test: host-test check-binary qemu-test

host-test: $(HOST_TEST_BIN)
	$(Q)$(HOST_TEST_BIN)

# The panic run checks the whole crash path: decode, CR2, registers and a
# backtrace symbolized through the faulting function back to kmain.
qemu-test: $(TEST_IMG) $(PANIC_IMG)
	$(Q)$(PYTHON) tests/qemu/run_tests.py --image $(TEST_IMG) --log-dir $(BUILD)/logs
	$(Q)$(PYTHON) tests/qemu/run_tests.py --image $(PANIC_IMG) --log-dir $(BUILD)/logs \
	  --expect-panic \
	  --expect 'KERNEL PANIC' \
	  --expect 'exception 14 \(#PF page fault\), error code 0x2, from kernel mode' \
	  --expect 'page fault: write hit a non-present page in kernel mode' \
	  --expect 'CR2 0000000080000000' \
	  --expect '#0 +0x[0-9a-f]+ selftest_page_fault\+0x' \
	  --expect '#[0-9]+ +0x[0-9a-f]+ kmain\+0x' \
	  --expect 'end of panic'

# Static checks on the linked kernel (e.g. no SSE instructions slipped in).
check-binary: $(KERNEL_ELF)
	$(Q)$(PYTHON) tools/check_kernel.py $(KERNEL_ELF)

# --------------------------------------------------------------------------
# Code quality
# --------------------------------------------------------------------------

FORMAT_SRCS := $(shell find kernel tests -name '*.c' -o -name '*.h')

.PHONY: format check-format lint
format:
	$(Q)clang-format -i $(FORMAT_SRCS)

check-format:
	$(Q)clang-format --dry-run --Werror $(FORMAT_SRCS)

lint: $(ASM_OFFSETS)
	$(Q)clang-tidy --quiet $(KERNEL_C_SRCS) -- $(KERNEL_CFLAGS)

.PHONY: clean
clean:
	rm -rf build

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
