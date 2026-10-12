// SPDX-License-Identifier: MIT
/* Burns CPU without ever making a system call: only preemption can stop it. */
#include "libsys.h"

int main(void)
{
    long pid = sys_getpid();
    long start = sys_time_ms();
    volatile u64 work = 0;

    while (sys_time_ms() - start < 150) {
        for (int i = 0; i < 100000; i++)
            work++;
    }
    printf("spinner[%ld]: done after %ld ms of busy work\n", pid, sys_time_ms() - start);
    return 0;
}
