# SPDX-License-Identifier: MIT
#
# Top-level, non-recursive build. Everything lands in $(BUILD).
#
#   make                 build the kernel and disk images
#   make run             boot build/<config>/disk.img in QEMU (CMDLINE="..." to pass options)
#   make debug           same, halted at reset waiting for gdb on :1234
#   make test            host unit tests, binary checks, in-kernel tests under QEMU
#   make check           check-format + lint + test: what CI runs (gate commits on it)
#   make lint            clang-tidy over kernel sources
#   make format          reformat all C sources in place
#   make check-format    fail if any C source is not clang-formatted
#
# CONFIG=debug (default) or CONFIG=release.

.DEFAULT_GOAL := all
.SUFFIXES:
.DELETE_ON_ERROR:
# Keep chained intermediates (e.g. crt0.asm.o): deleting them forces relinks on every build.
.SECONDARY:
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

# Flags whose absence compiles cleanly but corrupts state at run time.
# (Interrupts clobber the red zone; SSE would clobber user FPU state.)
$(foreach f,-mno-red-zone -mgeneral-regs-only -mcmodel=kernel, \
  $(if $(filter $(f),$(KERNEL_CFLAGS)),,$(error kernel build requires $(f))))

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

# User programs: same freestanding toolchain, but SSE allowed (the kernel
# saves user FPU state), no kernel code model, linked at 4 MiB.
USER_CFLAGS := --target=x86_64-unknown-none-elf -std=c17 \
	-ffreestanding -nostdinc -isystem $(CLANG_RESOURCE_DIR)/include \
	-fno-pic -fno-pie -O2 -fno-stack-protector -fno-omit-frame-pointer \
	-fno-asynchronous-unwind-tables -fno-unwind-tables \
	-Wall -Wextra -Werror -Wshadow -Wstrict-prototypes -Wvla \
	-ffile-prefix-map=$(CURDIR)/= -Ikernel/include -Iuser/lib
USER_LDFLAGS := -nostdlib -static -z max-page-size=0x1000 -z noexecstack --build-id=none \
	--strip-all

# Rebuild everything when the flags change, not only when sources do.
FLAGS_STAMP := $(BUILD)/flags.stamp
FLAGS_SIG   := $(CC) $(KERNEL_CFLAGS) | $(HOSTCC) $(HOST_CFLAGS) | $(USER_CFLAGS)
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

# --------------------------------------------------------------------------
# User programs, embedded in the kernel image (kernel/proc/process.c)
# --------------------------------------------------------------------------

USER_LIB_OBJS := $(BUILD)/user/user/lib/libsys.o $(BUILD)/user/kernel/lib/printf.o \
	$(BUILD)/user/kernel/lib/string.o $(BUILD)/user/user/lib/crt0.asm.o
USER_PROGRAMS := $(sort $(basename $(notdir $(wildcard user/programs/*.c user/programs/*.asm))))
USER_ELFS     := $(USER_PROGRAMS:%=$(BUILD)/user/bin/%.elf)
USER_IMAGES   := $(BUILD)/gen/user_images.o

$(BUILD)/user/%.o: %.c $(FLAGS_STAMP)
	@mkdir -p $(@D)
	$(call msg,CC,$<)
	$(Q)$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/%.asm.o: %.asm
	@mkdir -p $(@D)
	$(call msg,NASM,$<)
	$(Q)$(NASM) -f elf64 -w+all $< -o $@

# C programs link against the runtime; assembly programs bring their own script.
$(BUILD)/user/bin/%.elf: $(BUILD)/user/user/programs/%.o $(USER_LIB_OBJS) user/linker.ld
	@mkdir -p $(@D)
	$(call msg,LD,$@)
	$(Q)$(LD) $(USER_LDFLAGS) -T user/linker.ld $(BUILD)/user/user/lib/crt0.asm.o $< \
	  $(filter-out %crt0.asm.o,$(USER_LIB_OBJS)) -o $@

$(BUILD)/user/bin/%.elf: $(BUILD)/user/user/programs/%.asm.o user/programs/%.ld
	@mkdir -p $(@D)
	$(call msg,LD,$@)
	$(Q)$(LD) $(USER_LDFLAGS) -T user/programs/$*.ld $< -o $@

$(BUILD)/gen/user_images.asm: $(USER_ELFS) tools/gen_user_images.py
	@mkdir -p $(@D)
	$(call msg,GEN,$@)
	$(Q)$(PYTHON) tools/gen_user_images.py -o $@ \
	  $(foreach p,$(USER_PROGRAMS),$(p)=$(BUILD)/user/bin/$(p).elf)

$(USER_IMAGES): $(BUILD)/gen/user_images.asm $(USER_ELFS)
	$(call msg,NASM,$<)
	$(Q)$(NASM) -f elf64 $< -o $@

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

$(KERNEL_PASS1): $(KERNEL_OBJS) $(USER_IMAGES) $(BUILD)/gen/ksyms_empty.o kernel/linker.ld
	$(call msg,LD,$@)
	$(Q)$(LD) $(KERNEL_LDFLAGS) $(KERNEL_OBJS) $(USER_IMAGES) $(BUILD)/gen/ksyms_empty.o -o $@

$(KERNEL_ELF): $(KERNEL_OBJS) $(USER_IMAGES) $(BUILD)/gen/ksyms.o kernel/linker.ld
	$(call msg,LD,$@)
	$(Q)$(LD) $(KERNEL_LDFLAGS) $(KERNEL_OBJS) $(USER_IMAGES) $(BUILD)/gen/ksyms.o -o $@
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

PANIC_IMG       := $(BUILD)/disk-panic.img
DEMO_IMG        := $(BUILD)/disk-demo.img
BENCH_IMG       := $(BUILD)/disk-bench.img
HANG_IMG        := $(BUILD)/disk-selftest-hang.img
TRIPLEFAULT_IMG := $(BUILD)/disk-selftest-triplefault.img

# $(call bios_image,<image>,<command line>): a BIOS disk image of the same kernel.
define bios_image
$(1): $$(STAGE1) $$(STAGE2) $$(KERNEL_STRIPPED) tools/mkimage.py
	$$(call msg,IMAGE,$$@)
	$$(Q)$$(PYTHON) tools/mkimage.py --stage1 $$(STAGE1) --stage2 $$(STAGE2) \
	  --kernel $$(KERNEL_STRIPPED) --cmdline "$(2)" -o $$@
endef

$(eval $(call bios_image,$(TEST_IMG),ktest))
$(eval $(call bios_image,$(PANIC_IMG),selftest=panic))
$(eval $(call bios_image,$(DEMO_IMG),exit-after-init))
$(eval $(call bios_image,$(BENCH_IMG),bench))
$(eval $(call bios_image,$(HANG_IMG),selftest=hang))
$(eval $(call bios_image,$(TRIPLEFAULT_IMG),selftest=triplefault))

# --------------------------------------------------------------------------
# UEFI loader (boot/efi) and UEFI boot images
# --------------------------------------------------------------------------

EFI_CFLAGS := --target=x86_64-unknown-windows -std=c17 \
	-ffreestanding -nostdinc -isystem $(CLANG_RESOURCE_DIR)/include \
	-mno-red-zone -mgeneral-regs-only -mno-stack-arg-probe -fno-stack-protector -O2 \
	-Wall -Wextra -Werror -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla \
	-ffile-prefix-map=$(CURDIR)/= -Ikernel/include
EFI_SRCS   := boot/efi/loader.c kernel/lib/printf.c kernel/lib/string.c
EFI_OBJS   := $(EFI_SRCS:%.c=$(BUILD)/efi/%.o)
EFI_LOADER := $(BUILD)/efi/BOOTX64.EFI

$(BUILD)/efi/%.o: %.c $(FLAGS_STAMP)
	@mkdir -p $(@D)
	$(call msg,CC-EFI,$<)
	$(Q)$(CC) $(EFI_CFLAGS) -MMD -MP -c $< -o $@

$(EFI_LOADER): $(EFI_OBJS)
	$(call msg,LD,$@)
	$(Q)lld-link /nologo /subsystem:efi_application /entry:efi_main /nodefaultlib \
	  /machine:x64 /out:$@ $(EFI_OBJS)

UEFI_IMG       := $(BUILD)/disk-uefi.img
UEFI_TEST_IMG  := $(BUILD)/disk-uefi-ktest.img
UEFI_DEMO_IMG  := $(BUILD)/disk-uefi-demo.img

# $(call uefi_image,<image>,<command line>): the same kernel behind the UEFI loader.
define uefi_image
$(1): $$(EFI_LOADER) $$(KERNEL_STRIPPED) tools/mkesp.py $(3)
	$$(call msg,ESP,$$@)
	$$(Q)$$(PYTHON) tools/mkesp.py --loader $$(EFI_LOADER) --kernel $$(KERNEL_STRIPPED) \
	  --cmdline "$(2)" -o $$@
endef

$(eval $(call uefi_image,$(UEFI_TEST_IMG),ktest))
$(eval $(call uefi_image,$(UEFI_DEMO_IMG),exit-after-init))

# The run image embeds $(CMDLINE); this stamp changes only when it does.
CMDLINE_STAMP := $(BUILD)/cmdline.stamp
$(shell mkdir -p $(BUILD) && { printf '%s' '$(CMDLINE)' | cmp -s - $(CMDLINE_STAMP) || \
	printf '%s' '$(CMDLINE)' > $(CMDLINE_STAMP); })

$(eval $(call uefi_image,$(UEFI_IMG),$(CMDLINE),$(CMDLINE_STAMP)))

$(DISK_IMG): $(STAGE1) $(STAGE2) $(KERNEL_STRIPPED) tools/mkimage.py $(CMDLINE_STAMP)
	$(call msg,IMAGE,$@)
	$(Q)$(PYTHON) tools/mkimage.py --stage1 $(STAGE1) --stage2 $(STAGE2) \
	  --kernel $(KERNEL_STRIPPED) --cmdline "$(CMDLINE)" -o $@

.PHONY: all
all: $(DISK_IMG) $(TEST_IMG) $(PANIC_IMG) $(DEMO_IMG) $(BENCH_IMG) $(HANG_IMG) \
	$(TRIPLEFAULT_IMG) $(UEFI_IMG) $(UEFI_TEST_IMG) $(UEFI_DEMO_IMG) $(KERNEL_ELF)

# --------------------------------------------------------------------------
# Running
# --------------------------------------------------------------------------

QEMU       := qemu-system-x86_64
QEMU_FLAGS := -m 128M -cpu max -no-reboot -serial stdio -display none -monitor none \
              -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
              -drive file=$(DISK_IMG),format=raw,if=ide

.PHONY: run run-uefi debug bench

# Boots disk-uefi.img through OVMF (apt install ovmf).
OVMF_CODE ?= $(firstword $(wildcard /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd))
run-uefi: $(UEFI_IMG)
	@test -n "$(OVMF_CODE)" || { echo "OVMF not found: apt install ovmf"; exit 1; }
	@cp $(subst CODE,VARS,$(OVMF_CODE)) $(BUILD)/OVMF_VARS.fd
	$(QEMU) $(subst $(DISK_IMG),$(UEFI_IMG),$(QEMU_FLAGS)) \
	  -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	  -drive if=pflash,format=raw,file=$(BUILD)/OVMF_VARS.fd $(QEMU_EXTRA)
# Numbers under TCG reflect the emulator too; use QEMU_EXTRA="-enable-kvm -cpu host" for real ones.
bench: $(BENCH_IMG)
	$(Q)$(QEMU) $(subst $(DISK_IMG),$(BENCH_IMG),$(QEMU_FLAGS)) $(QEMU_EXTRA) | grep --line-buffered 'bench:' || true

run: $(DISK_IMG)
	$(QEMU) $(QEMU_FLAGS) $(QEMU_EXTRA)

# Waits for gdb on :1234 before the first instruction. In another shell:
#   gdb -x tools/gdb/gdbinit $(KERNEL_ELF)
debug: $(DISK_IMG) $(KERNEL_ELF)
	$(QEMU) $(QEMU_FLAGS) -s -S -d int,cpu_reset -D $(BUILD)/qemu-debug.log $(QEMU_EXTRA)

# --------------------------------------------------------------------------
# Host unit tests: pure kernel modules compiled for Linux under ASan/UBSan.
# --------------------------------------------------------------------------

HOST_TEST_KERNEL_SRCS := kernel/lib/string.c kernel/lib/printf.c kernel/lib/cmdline.c \
	kernel/mm/memmap.c kernel/mm/buddy.c kernel/proc/elf.c
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
# The 4G run puts RAM above the 4 GiB PCI hole, exercising a sparse memory map.
qemu-test: $(TEST_IMG) $(PANIC_IMG) $(DEMO_IMG) $(BENCH_IMG) $(HANG_IMG) $(TRIPLEFAULT_IMG) \
	  $(UEFI_TEST_IMG) $(UEFI_DEMO_IMG)
	$(Q)$(PYTHON) tests/qemu/check_runner.py --hang-image $(HANG_IMG) \
	  --triplefault-image $(TRIPLEFAULT_IMG) --log-dir $(BUILD)/logs
	$(Q)$(PYTHON) tests/qemu/run_tests.py --image $(TEST_IMG) --log-dir $(BUILD)/logs
	$(Q)$(PYTHON) tests/qemu/run_tests.py --image $(TEST_IMG) --log-dir $(BUILD)/logs --memory 4G
	$(Q)$(PYTHON) tests/qemu/run_tests.py --image $(PANIC_IMG) --log-dir $(BUILD)/logs \
	  --expect-panic \
	  --expect 'KERNEL PANIC' \
	  --expect 'exception 14 \(#PF page fault\), error code 0x2, from kernel mode' \
	  --expect 'page fault: write hit a non-present page in kernel mode' \
	  --expect 'CR2 0000000080000000' \
	  --expect '#0 +0x[0-9a-f]+ selftest_page_fault\+0x' \
	  --expect '#[0-9]+ +0x[0-9a-f]+ kmain\+0x' \
	  --expect 'end of panic'
	$(Q)$(PYTHON) tests/qemu/run_tests.py --image $(DEMO_IMG) --log-dir $(BUILD)/logs --no-tap \
	  --expect 'init: pid [0-9]+, starting 11 programs' \
	  --expect 'init: all children reaped, 0 unexpected' \
	  --expect 'init exited with status 0'
	$(Q)$(PYTHON) tests/qemu/run_tests.py --uefi --image $(UEFI_TEST_IMG) --log-dir $(BUILD)/logs
	$(Q)$(PYTHON) tests/qemu/run_tests.py --uefi --image $(UEFI_DEMO_IMG) --log-dir $(BUILD)/logs \
	  --no-tap --expect 'loader: UEFI' --expect 'init: all children reaped, 0 unexpected' \
	  --expect 'init exited with status 0'
	$(Q)$(PYTHON) tests/qemu/run_tests.py --image $(BENCH_IMG) --log-dir $(BUILD)/logs --no-tap \
	  --expect 'bench: context switch' --expect 'bench: syscall round trip' \
	  --expect 'user benchmark exit 0'

# Static checks on the linked kernel (e.g. no SSE instructions slipped in).
check-binary: $(KERNEL_ELF)
	$(Q)$(PYTHON) tools/check_kernel.py $(KERNEL_ELF)

# --------------------------------------------------------------------------
# Code quality
# --------------------------------------------------------------------------

FORMAT_SRCS := $(shell find boot kernel tests user -name '*.c' -o -name '*.h')

# Everything CI checks for this configuration, in CI's order. Run before every commit.
.PHONY: check
check: check-format lint
	$(Q)$(MAKE) --no-print-directory test

.PHONY: format check-format lint
format:
	$(Q)clang-format -i $(FORMAT_SRCS)

check-format:
	$(Q)clang-format --dry-run --Werror $(FORMAT_SRCS)

lint: $(ASM_OFFSETS)
	$(Q)clang-tidy --quiet $(KERNEL_C_SRCS) -- $(KERNEL_CFLAGS)
	$(Q)clang-tidy --quiet $(wildcard user/lib/*.c user/programs/*.c) -- $(USER_CFLAGS)
	$(Q)clang-tidy --quiet boot/efi/loader.c -- $(EFI_CFLAGS)

.PHONY: clean
clean:
	rm -rf build

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
