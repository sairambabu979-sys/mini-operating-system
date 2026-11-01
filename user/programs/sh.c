// SPDX-License-Identifier: MIT
/*
 * The shell: reads lines from the console (the PS/2 keyboard or COM1,
 * through read()), runs the program each one names with the words after
 * it as arguments (split at spaces, no quoting), and reports how it
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

static void put(const char *s, usize n)
{
    if (n)
        sys_write(1, s, n);
}

/* The programs are whatever /bin holds: listed live, so a new one needs no change here. */
static void help(void)
{
    struct dirent d;

    printf("builtins: help exit\nprograms (in /bin):\n");
    for (u64 i = 0; sys_readdir("/bin", i, &d) > 0; i++)
        if (d.type == DIRENT_FILE)
            printf("  %s\n", d.name);
}

static void run(const char *name, const char *const *args)
{
    long pid = sys_spawnv(name, args);
    int status;

    if (pid < 0) {
        if (pid == -ENOENT)
            printf("sh: %s: not found\n", name);
        else if (pid == -ENAMETOOLONG)
            printf("sh: name too long (15 characters at most)\n");
        else if (pid == -EAGAIN)
            printf("sh: too many tasks\n");
        else if (pid == -E2BIG)
            printf("sh: too many arguments (%d at most)\n", SPAWN_ARGS_MAX);
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
    const char *words[LE_LINE_MAX / 2 + 2]; /* every other character a word, then NULL */
    int n = 0;

    for (char *p = line; *p;) { /* split at runs of spaces, in place */
        while (*p == ' ')
            *p++ = '\0';
        if (*p)
            words[n++] = p;
        while (*p && *p != ' ')
            p++;
    }
    words[n] = NULL;
    if (n == 0)
        return false;
    if (strcmp(words[0], "exit") == 0 || strcmp(words[0], "help") == 0) {
        if (n > 1)
            printf("sh: %s takes no arguments\n", words[0]);
        else if (words[0][0] == 'e')
            return true;
        else
            help();
        return false;
    }
    if (n - 1 > SPAWN_ARGS_MAX)
        printf("sh: too many arguments (%d at most)\n", SPAWN_ARGS_MAX);
    else
        run(words[0], words + 1);
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
