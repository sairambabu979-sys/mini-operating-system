// SPDX-License-Identifier: MIT
/*
 * Asks read() for far more than it returns at once. The test queues 256
 * bytes, byte i being (u8)(i * 3 + 1); read() must return all of them
 * and no more, and write nothing past them. Not in init's tour: it needs
 * that input. Exit status:
 *   0  read exactly the 256 bytes, and nothing past them was written;
 *   3  read() failed;
 *   4  the wrong number of bytes;
 *   5  the wrong bytes;
 *   6  bytes written past the count.
 */
#include "libsys.h"

#include <lib/string.h>

#define WANT 256 /* the kernel's INPUT_READ_MAX */

static unsigned char buf[4096];

int main(void)
{
    memset(buf, 0x55, sizeof(buf));
    long n = sys_read(0, buf, sizeof(buf));

    if (n < 0)
        return 3;
    if (n != WANT)
        return 4;
    for (usize i = 0; i < WANT; i++)
        if (buf[i] != (unsigned char)(i * 3 + 1))
            return 5;
    for (usize i = WANT; i < sizeof(buf); i++)
        if (buf[i] != 0x55)
            return 6;
    return 0;
}
