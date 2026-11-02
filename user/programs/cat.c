// SPDX-License-Identifier: MIT
#include "libsys.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: cat /path\n");
        return 1;
    }
    int fd = (int)sys_open(argv[1]);
    char bytes[512];
    long n;

    if (fd < 0) {
        printf("cat: %s: error %d\n", argv[1], fd);
        return 1;
    }
    while ((n = sys_read(fd, bytes, sizeof(bytes))) > 0) {
        if (sys_write(1, bytes, (usize)n) != n) {
            sys_close(fd);
            return 1;
        }
    }
    sys_close(fd);
    if (n < 0)
        printf("cat: %s: error %ld\n", argv[1], n);
    return n < 0;
}
