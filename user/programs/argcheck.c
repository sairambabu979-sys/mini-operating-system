// SPDX-License-Identifier: MIT
/*
 * Checks how the kernel handed it its arguments, then prints them in
 * brackets ("argcheck: [argcheck] [one] [two two]"). Not in init's tour.
 * Exit status: argc if all is well, else
 *   100  argv[argc] is not NULL;
 *   101  argv[0] is not "argcheck";
 *   102  a string is not in the stack's top page, below USER_STACK_TOP;
 *   103  a string cannot be written;
 *   104  main's frame is not 16-byte aligned;
 *   105  argv itself is not 8-byte aligned.
 */
#include "libsys.h"

#include <lib/string.h>

#define STACK_TOP 0x00007FFFFFFF0000ull /* USER_STACK_TOP */

int main(int argc, char **argv)
{
    if (((unsigned long)__builtin_frame_address(0) & 15) != 0)
        return 104;
    if (((unsigned long)argv & 7) != 0)
        return 105;
    if (argv[argc] != NULL)
        return 100;
    if (strcmp(argv[0], "argcheck") != 0)
        return 101;
    for (int i = 0; i < argc; i++) {
        unsigned long at = (unsigned long)argv[i];

        if (at < STACK_TOP - 4096 || at + strlen(argv[i]) >= STACK_TOP)
            return 102;
        char saved = argv[i][0];

        argv[i][0] = 'X';
        if (argv[i][0] != 'X')
            return 103;
        argv[i][0] = saved;
    }
    char line[SPAWN_ARGS_SIZE + 3 * (SPAWN_ARGS_MAX + 1) + 16] = "argcheck:";
    usize n = strlen(line);

    for (int i = 0; i < argc; i++) {
        usize len = strlen(argv[i]);

        line[n++] = ' ';
        line[n++] = '[';
        memcpy(line + n, argv[i], len);
        n += len;
        line[n++] = ']';
    }
    line[n++] = '\n';
    sys_write(1, line, n);
    return argc;
}
