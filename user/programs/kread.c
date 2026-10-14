// SPDX-License-Identifier: MIT
/* Reads kernel memory: mapped, but supervisor-only, so a protection #PF. */
#include "libsys.h"

int main(void)
{
    printf("kread: reading the kernel image at 0xffffffff80100000\n");
    u64 v = *(volatile u64 *)0xFFFFFFFF80100000ull;

    printf("kread: read %#lx from the kernel?!\n", v);
    return 0;
}
