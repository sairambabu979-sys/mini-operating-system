// SPDX-License-Identifier: MIT
#include <arch/cpu.h>
#include <arch/io.h>
#include <drivers/qemu.h>

__noreturn void qemu_exit(enum qemu_exit_code code)
{
    outl(QEMU_DEBUG_EXIT_PORT, code);
    cpu_halt_forever();
}
