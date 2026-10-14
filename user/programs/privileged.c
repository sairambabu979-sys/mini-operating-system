// SPDX-License-Identifier: MIT
/* Executes a ring-0-only instruction: #GP, process killed. */
#include "libsys.h"

int main(void)
{
    printf("privileged: trying cli\n");
    __asm__ volatile("cli");
    printf("privileged: interrupts disabled from ring 3?!\n");
    return 0;
}
