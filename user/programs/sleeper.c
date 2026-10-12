// SPDX-License-Identifier: MIT
#include "libsys.h"

int main(void)
{
    long start = sys_time_ms();

    for (int i = 0; i < 3; i++)
        sys_sleep_ms(40);
    long elapsed = sys_time_ms() - start;

    printf("sleeper: slept 3 x 40 ms, %ld ms elapsed\n", elapsed);
    return elapsed >= 120 ? 0 : 1;
}
