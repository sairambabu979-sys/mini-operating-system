// SPDX-License-Identifier: MIT
/*
 * Reads one line of console input and checks read()'s contract. Not in
 * init's tour: it waits for input, so only tests that supply some run
 * it. Exit status:
 *   0    read exactly "ok\n", and nothing past the count was written;
 *   2    a bad buffer was not rejected with -EFAULT;
 *   3    read() failed some other way;
 *   4    wrong bytes, or bytes written past the count;
 *   100  no input device (-ENODEV).
 */
#include "libsys.h"

#include <lib/errno.h>
#include <lib/string.h>

static const char read_only[4] = "ro";

int main(void)
{
    char buf[8], line[8];
    usize len = 0;

    if (sys_read(0, (void *)(unsigned long)read_only, 4) != -EFAULT)
        return 2;
    if (sys_read(0, (void *)0x10000000ull, 4) != -EFAULT)
        return 2;
    while (len < sizeof(line) && (len == 0 || line[len - 1] != '\n')) {
        memset(buf, 0x55, sizeof(buf));
        long n = sys_read(0, buf, sizeof(line) - len);

        if (n == -ENODEV)
            return 100;
        if (n <= 0)
            return 3;
        for (usize i = (usize)n; i < sizeof(buf); i++)
            if ((unsigned char)buf[i] != 0x55)
                return 4;
        memcpy(line + len, buf, (usize)n);
        len += (usize)n;
    }
    return len == 3 && memcmp(line, "ok\n", 3) == 0 ? 0 : 4;
}
