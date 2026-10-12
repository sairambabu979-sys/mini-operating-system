// SPDX-License-Identifier: MIT
/* Dereferences NULL: the kernel must kill this process and nothing else. */
#include "libsys.h"

int main(void)
{
    printf("faulter: writing to address 0\n");
    *(volatile int *)0 = 1; // NOLINT(clang-analyzer-core.NullDereference): the point
    printf("faulter: still alive?!\n");
    return 0;
}
