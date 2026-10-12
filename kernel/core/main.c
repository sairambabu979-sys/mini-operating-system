// SPDX-License-Identifier: MIT
/*
 * Kernel C entry point.
 */
#include <compiler.h>
#include <types.h>

__noreturn void kmain(u64 boot_info_phys);

__noreturn void kmain(u64 boot_info_phys)
{
    (void)boot_info_phys;
    for (;;)
        __asm__ volatile("cli; hlt");
}
