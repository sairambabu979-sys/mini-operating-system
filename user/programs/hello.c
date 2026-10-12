// SPDX-License-Identifier: MIT
#include "libsys.h"

int main(void)
{
    printf("hello: hello from ring 3, pid %ld\n", sys_getpid());
    return 42;
}
