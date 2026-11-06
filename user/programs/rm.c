// SPDX-License-Identifier: MIT
/*
 * Removes each file named (only /tmp's can be: the initrd is read-only).
 * A file that cannot be removed is reported on fd 2. Exit status 0, 1 if
 * any could not be, 2 with no names.
 */
#include "libsys.h"

int main(int argc, char **argv)
{
    int status = 0;

    if (argc < 2) {
        eprintf("usage: rm FILE...\n");
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        long err = sys_unlink(argv[i]);

        if (err < 0) {
            eprintf("rm: %s: %s\n", argv[i], sys_strerror(err));
            status = 1;
        }
    }
    return status;
}
