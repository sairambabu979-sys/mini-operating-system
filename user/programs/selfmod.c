// SPDX-License-Identifier: MIT
/* Writes to its own code: user text is read-only (W^X), so #PF. */
#include "libsys.h"

int main(void)
{
    printf("selfmod: patching my own text\n");
    *(volatile u8 *)(uptr)&main = 0xC3;
    printf("selfmod: text was writable?!\n");
    return 0;
}
