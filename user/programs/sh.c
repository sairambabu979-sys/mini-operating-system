// SPDX-License-Identifier: MIT
/*
 * The shell: reads lines from the console (the PS/2 keyboard or COM1,
 * through read()), runs the program each one names, and reports how it
 * ended. The kernel delivers raw bytes and never echoes: lineedit.c edits
 * the line and says what to echo. Not in init's tour: boot it with
 * init=sh (the shell images).
 *
 * It reads one byte at a time, so whatever was typed after a command's
 * Enter stays in the kernel's queue for the program that command starts:
 * a paste of "readkeys\nok\n" gives readkeys its "ok".
 *
 * Every way out ends its output with "\n", so whatever prints next (a TAP
 * line, the kernel's "init exited") starts on a fresh line. The shell
 * always waits for the program it started, so it never holds more than
 * one child. Exit status: 0 on exit or Ctrl-D, 1 if read() fails.
 */
#include "libsys.h"
#include "lineedit.h"

#include <lib/errno.h>
#include <lib/string.h>
#include <user_programs.h> /* generated: USER_PROGRAM_NAMES */

static const char *const programs[] = {USER_PROGRAM_NAMES};

static void put(const char *s, usize n)
{
    if (n)
        sys_write(1, s, n);
}

static void help(void)
{
    printf("builtins: help exit\nprograms:\n");
    for (usize i = 0; i < sizeof(programs) / sizeof(programs[0]); i++)
        printf("  %s\n", programs[i]);
}

static void run(const char *name)
{
    long pid = sys_spawn(name);
    int status;

    if (pid < 0) {
        if (pid == -ENOENT)
            printf("sh: %s: not found\n", name);
        else if (pid == -ENAMETOOLONG)
            printf("sh: name too long (15 characters at most)\n");
        else if (pid == -EAGAIN)
            printf("sh: too many tasks\n");
        else
            printf("sh: %s: cannot start (%ld)\n", name, pid);
        return;
    }
    long got = sys_wait((int)pid, &status);

    if (got != pid)
        printf("sh: wait for %ld failed (%ld)\n", pid, got);
    else
        printf("[exit %d]\n", status);
}

/* Runs one finished line; true if it was "exit". */
static bool run_line(char *line)
{
    usize n;

    while (*line == ' ')
        line++;
    n = strlen(line);
    while (n && line[n - 1] == ' ')
        line[--n] = '\0';
    if (n == 0)
        return false;
    if (strcmp(line, "exit") == 0)
        return true;
    if (strcmp(line, "help") == 0)
        help();
    else if (strchr(line, ' '))
        printf("sh: programs take no arguments\n");
    else
        run(line);
    return false;
}

int main(void)
{
    static struct lineedit le;
    char echo[LE_ECHO_MAX];

    le_reset(&le);
    printf("sh: type a program name to run it; 'help' lists them; 'exit' or Ctrl-D leaves\n");
    put("sh> ", 4);
    for (;;) {
        u8 c;
        long got = sys_read(0, &c, 1);

        if (got != 1) {
            printf("\nsh: cannot read input (%ld)\n", got);
            return 1;
        }
        usize echo_len;
        enum le_event ev = le_feed(&le, c, echo, &echo_len);

        put(echo, echo_len); /* one write per byte fed: the echo never splits */
        switch (ev) {
        case LE_DONE:
            if (run_line(le.buf))
                return 0; /* the Enter after "exit" already ended the line */
            put("sh> ", 4);
            break;
        case LE_CANCEL:
            put("sh> ", 4);
            break;
        case LE_EOF:
            put("\n", 1);
            return 0;
        case LE_MORE:
            break;
        }
    }
}
