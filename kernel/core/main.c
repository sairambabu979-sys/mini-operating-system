// SPDX-License-Identifier: MIT
/*
 * Kernel C entry point and boot sequence.
 */
#include <acpi/acpi.h>
#include <arch/cpu.h>
#include <arch/cpuid.h>
#include <arch/fpu.h>
#include <arch/paging.h>
#include <arch/percpu.h>
#include <arch/segments.h>
#include <arch/trap.h>
#include <boot/boot_info.h>
#include <core/bench.h>
#include <core/boot.h>
#include <core/input.h>
#include <core/irq.h>
#include <core/klog.h>
#include <core/ktest.h>
#include <core/panic.h>
#include <core/printk.h>
#include <core/sections.h>
#include <core/time.h>
#include <drivers/qemu.h>
#include <drivers/screen.h>
#include <drivers/serial.h>
#include <fs/initrd.h>
#include <lib/cmdline.h>
#include <lib/string.h>
#include <mm/mm.h>
#include <proc/process.h>
#include <sched/sched.h>
#include <version.h>

__noreturn void kmain(u64 boot_info_phys);

/*
 * The kernel's own copy. The loader's page may be ordinary RAM (the UEFI
 * loader takes any page below 1 GiB the firmware has free), which the
 * PMM hands out once it runs.
 */
static struct boot_info boot_info_copy;
static const struct boot_info *boot_info;
extern u64 loader_rflags; /* entry.asm: RFLAGS exactly as the loader jumped in */

const struct boot_info *boot_info_get(void)
{
    return boot_info;
}

u64 boot_entry_rflags(void)
{
    return loader_rflags;
}

static const char *e820_type_name(u32 type)
{
    switch (type) {
    case E820_USABLE:
        return "usable";
    case E820_RESERVED:
        return "reserved";
    case E820_ACPI_RECLAIMABLE:
        return "ACPI reclaimable";
    case E820_ACPI_NVS:
        return "ACPI NVS";
    case E820_BAD:
        return "bad memory";
    default:
        return "unknown";
    }
}

static void print_memory_map(const struct boot_info *bi)
{
    u64 usable = 0;

    kprintf("memory map (%u entries, BIOS order):\n", bi->mmap_count);
    for (u32 i = 0; i < bi->mmap_count; i++) {
        const struct e820_entry *e = &bi->mmap[i];

        kprintf("  [0x%016llx-0x%016llx] %s\n", (unsigned long long)e->base,
                (unsigned long long)(e->base + e->length - 1), e820_type_name(e->type));
        if (e->type == E820_USABLE)
            usable += e->length;
    }
    kprintf("usable RAM: %llu KiB\n", (unsigned long long)(usable / 1024));
}

/*
 * The loader writes `magic` last, so a good magic means a complete record.
 * Without a valid boot_info nothing else can be trusted: fail loudly.
 */
static const struct boot_info *validate_boot_info(u64 phys)
{
    /* Read through the loaders' window onto the first GiB (phys_to_kimage). */
    if (phys == 0 || !IS_ALIGNED(phys, 8) || phys > BOOT_WINDOW_SIZE - sizeof(struct boot_info)) {
        kprintf("FATAL: boot_info at %#llx is outside the boot window\n", (unsigned long long)phys);
        qemu_exit(QEMU_EXIT_FAILURE);
    }

    const struct boot_info *bi = phys_to_kimage(phys);

    if (bi->magic != BOOT_INFO_MAGIC) {
        kprintf("FATAL: no valid boot_info at %#llx\n", (unsigned long long)phys);
        qemu_exit(QEMU_EXIT_FAILURE);
    }
    if (bi->version != BOOT_INFO_VERSION) {
        kprintf("FATAL: boot_info version %u, kernel expects %u\n", bi->version, BOOT_INFO_VERSION);
        qemu_exit(QEMU_EXIT_FAILURE);
    }
    memcpy(&boot_info_copy, bi, sizeof(boot_info_copy));
    boot_info_copy.mmap_count = MIN(boot_info_copy.mmap_count, BOOT_MMAP_MAX);
    boot_info_copy.cmdline[BOOT_CMDLINE_MAX - 1] = '\0';

    /* An initrd the kernel could not reach is no initrd: nothing reserves or reads it. */
    u64 rd = boot_info_copy.initrd_phys, rd_size = boot_info_copy.initrd_size;

    if (rd_size &&
        (!IS_ALIGNED(rd, PAGE_SIZE_4K) || rd_size > HHDM_SIZE || rd > HHDM_SIZE - rd_size)) {
        pr_warn("initrd at %#llx (%llu bytes) is unaligned or outside the direct map: ignored\n",
                (unsigned long long)rd, (unsigned long long)rd_size);
        rd_size = 0;
    }
    boot_info_copy.initrd_phys = rd_size ? rd : 0;
    boot_info_copy.initrd_size = rd_size;
    return &boot_info_copy;
}

static void apply_cmdline(const char *cmdline)
{
    char value[16];
    enum log_level level;

    if (cmdline_get(cmdline, "loglevel", value, sizeof(value))) {
        if (klog_parse_level(value, &level))
            klog_set_level(level);
        else
            pr_warn("unknown loglevel '%s' ignored\n", value);
    }
    /* Under test, a panic must end the run with a failure, not hang it. */
    panic_set_exit_on_panic(cmdline_has_flag(cmdline, "ktest") ||
                            cmdline_get(cmdline, "selftest", value, sizeof(value)) ||
                            cmdline_has_flag(cmdline, "exit-after-init"));
}

/*
 * selftest=panic: crash on purpose so the QEMU suite can check the panic
 * path end to end (decode, registers, symbolized backtrace, exit code).
 * noinline keeps this frame visible in the backtrace.
 */
static __attribute__((noinline)) void selftest_page_fault(void)
{
    *(volatile u64 *)0x80000000ull = 0xDEAD; /* canonical but unmapped */
}

static void run_selftest(const char *cmdline)
{
    char which[16];

    if (!cmdline_get(cmdline, "selftest", which, sizeof(which)))
        return;
    if (strcmp(which, "panic") == 0) {
        pr_info("selftest: provoking a kernel page fault\n");
        selftest_page_fault();
    }
    /* The next two exist to prove the QEMU test runner fails when it should. */
    if (strcmp(which, "hang") == 0) {
        pr_info("selftest: hanging with interrupts off\n");
        cpu_halt_forever();
    }
    if (strcmp(which, "triplefault") == 0) {
        static const struct {
            u16 limit;
            u64 base;
        } __packed empty_idt = {0, 0};

        pr_info("selftest: triple fault via an empty IDT\n");
        __asm__ volatile("lidt %0; int3" : : "m"(empty_idt));
    }
    panic("selftest '%s' did not panic", which);
}

/*
 * Starts user space and becomes its reaper: init's orphans are adopted by
 * this task, so it waits for anything, forever. The first program is
 * "init" unless "init=NAME" names another (the shell images). With
 * "exit-after-init" on the command line (the demo and shell tests), its
 * exit status ends the QEMU run.
 */
static __noreturn void run_init(const char *cmdline)
{
    char name[TASK_NAME_MAX + 1] = "init"; /* one more than fits, to catch a truncation */
    int status;

    if (cmdline_get(cmdline, "init", name, sizeof(name)) &&
        (name[0] == '\0' || strlen(name) >= TASK_NAME_MAX))
        panic("init=: a program name has 1 to %d characters", TASK_NAME_MAX - 1);
    int init_pid = process_spawn(name);

    if (init_pid < 0)
        panic("cannot start init '%s': %d", name, init_pid);
    for (;;) {
        int pid = task_wait(-1, &status);

        if (pid == init_pid) {
            pr_info("init exited with status %d\n", status);
            if (cmdline_has_flag(cmdline, "exit-after-init"))
                qemu_exit(status == 0 ? QEMU_EXIT_SUCCESS : QEMU_EXIT_FAILURE);
        }
        if (pid < 0)
            sleep_ms(1000); /* no children left: idle along */
    }
}

__noreturn void kmain(u64 boot_info_phys)
{
    serial_init();
    boot_info = validate_boot_info(boot_info_phys);
    apply_cmdline(boot_info->cmdline);

    /* CPU tables first: from here on, any fault produces a decoded panic. */
    gdt_init();
    idt_init();
    nmi_enable();
    run_selftest(boot_info->cmdline);

    cpu_detect_features();
    mm_init(boot_info);
    initrd_init(boot_info);

    screen_init(boot_info); /* replays everything logged so far */
    acpi_init(boot_info);
    irq_init();
    time_init();
    percpu_init();
    fpu_init();
    syscall_init();
    sched_init();
    cpu_irq_enable();
    console_go_live(); /* screen drawing leaves interrupts on from here */
    console_start_thread();

    kprintf("%s %s (x86_64, %s build)\n", MINIOS_NAME, MINIOS_VERSION, MINIOS_BUILD);
    kprintf("loader: %s\n", boot_info->loader == BOOT_LOADER_UEFI ? "UEFI" : "BIOS");
    kprintf("cmdline: \"%s\"\n", boot_info->cmdline);
    kprintf("kernel: phys [%#llx-%#llx), virt %p\n",
            (unsigned long long)boot_info->kernel_phys_start,
            (unsigned long long)boot_info->kernel_phys_end, (void *)__kernel_start);
    print_memory_map(boot_info);
    input_init();

    if (cmdline_has_flag(boot_info->cmdline, "bench")) {
        bench_run_all();
        qemu_exit(QEMU_EXIT_SUCCESS);
    }

    if (cmdline_has_flag(boot_info->cmdline, "ktest")) {
        int failed = ktest_run_all();

        qemu_exit(failed ? QEMU_EXIT_FAILURE : QEMU_EXIT_SUCCESS);
    }

    run_init(boot_info->cmdline);
}
