// SPDX-License-Identifier: MIT
/*
 * The shell: reads lines from the console (the PS/2 keyboard or COM1,
 * through read()), runs the program each one names with the words after
 * it as arguments (split at spaces, no quoting), and reports how it
 * ended. "a | b | c" runs a pipeline: each program's output is the next
 * one's input, through a pipe. The kernel delivers raw bytes and never
 * echoes: lineedit.c edits the line and says what to echo. Not in init's
 * tour: boot it with init=sh (the shell images).
 *
 * It reads one byte at a time, so whatever was typed after a command's
 * Enter stays in the kernel's queue for the program that command starts:
 * a paste of "readkeys\nok\n" gives readkeys its "ok". In a pipeline, the
 * first program has the console as its input.
 *
 * Every way out ends its output with "\n", so whatever prints next (a TAP
 * line, the kernel's "init exited") starts on a fresh line. The shell
 * always waits for every program it started before it reads again. Exit
 * status: 0 on exit or Ctrl-D, 1 if read() fails.
 */
#include "libsys.h"
#include "lineedit.h"

#include <lib/errno.h>
#include <lib/printf.h>
#include <lib/string.h>

#define PIPELINE_MAX 8 /* programs in one pipeline */

/* The bar between a pipeline's programs: a word of its own, however it was typed. */
static const char bar[] = "|";

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
    printf("a | b runs a with its output going to b\n");
}

/* Starts one program with these fds 0-2: its pid, or -1 after saying why not. */
static long start(const char *name, const char *const *args, const int stdio[3])
{
    long pid = sys_spawn_stdio(name, args, stdio);

    if (pid >= 0)
        return pid;
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
    return -1;
}

/*
 * Splits a line into words in place: runs of spaces separate them, and
 * each "|" is a word of its own (`bar`), with or without spaces around
 * it. Returns how many, with words[count] NULL.
 */
static int split(char *line, const char **words)
{
    int n = 0;

    for (char *p = line; *p;) {
        if (*p == ' ') {
            *p++ = '\0';
        } else if (*p == '|') {
            *p++ = '\0';
            words[n++] = bar;
        } else {
            words[n++] = p;
            while (*p && *p != ' ' && *p != '|')
                p++;
        }
    }
    words[n] = NULL;
    return n;
}

static bool is_builtin(const char *word)
{
    return strcmp(word, "exit") == 0 || strcmp(word, "help") == 0;
}

/*
 * Cuts words[0..n) at each bar into at most PIPELINE_MAX programs, each a
 * NULL-terminated word list in cmd[]. Their count, or 0 after saying why
 * the line cannot run.
 */
static int cut(const char **words, int n, const char **cmd[PIPELINE_MAX])
{
    int count = 0, first = 0;

    for (int i = 0; i <= n; i++) {
        if (i < n && words[i] != bar)
            continue;
        if (i == first) {
            printf("sh: a pipeline needs a program on each side of every |\n");
            return 0;
        }
        if (count == PIPELINE_MAX) {
            printf("sh: too many programs in a pipeline (%d at most)\n", PIPELINE_MAX);
            return 0;
        }
        if (is_builtin(words[first])) {
            printf("sh: %s cannot be in a pipeline\n", words[first]);
            return 0;
        }
        if (i - first - 1 > SPAWN_ARGS_MAX) {
            printf("sh: too many arguments (%d at most)\n", SPAWN_ARGS_MAX);
            return 0;
        }
        cmd[count++] = words + first;
        words[i] = NULL; /* ends this program's arguments */
        first = i + 1;
    }
    return count;
}

/*
 * Runs cmd[0] | cmd[1] | ...: each program's fd 1 is a pipe to the next
 * one's fd 0. The shell closes its own ends as it goes, so each reader
 * sees the end once its writer exits, and a writer gets -EPIPE once its
 * reader is gone. A program that cannot start is reported and the rest
 * still run (the one after it reads an empty pipe). If a pipe cannot be
 * made, no program from there on starts. Then the shell waits for all of
 * them and reports each status in order: "[exit 0 1]", with "-" for a
 * program that never started (no report at all for a lone program that
 * did not).
 */
static void run(const char **cmd[], int count)
{
    long pids[PIPELINE_MAX];
    int in = 0; /* the next program's fd 0: the console, then each pipe's read end */
    char line[8 + PIPELINE_MAX * 12];
    int len;

    for (int i = 0; i < count; i++) {
        int fds[2] = {-1, -1};

        if (i < count - 1) {
            long err = sys_pipe(fds);

            if (err < 0) {
                printf("sh: cannot make a pipe: %s\n", sys_strerror(err));
                for (int j = i; j < count; j++)
                    pids[j] = -1; /* not started: "-" */
                break;
            }
        }
        int stdio[3] = {in, fds[1] >= 0 ? fds[1] : 1, 2};

        pids[i] = start(cmd[i][0], cmd[i] + 1, stdio);
        if (in != 0)
            sys_close(in);
        if (fds[1] >= 0)
            sys_close(fds[1]);
        in = fds[0] >= 0 ? fds[0] : 0;
    }
    if (in != 0)
        sys_close(in); /* the pipe to a program that was never reached */
    if (count == 1 && pids[0] < 0)
        return; /* one program, which said why it did not start */
    len = k_snprintf(line, sizeof(line), "[exit");
    for (int i = 0; i < count; i++) {
        int status = 0;
        long got = pids[i] < 0 ? -1 : sys_wait((int)pids[i], &status);

        if (pids[i] < 0)
            len += k_snprintf(line + len, sizeof(line) - (usize)len, " -");
        else if (got != pids[i])
            len += k_snprintf(line + len, sizeof(line) - (usize)len, " ?(%ld)", got);
        else
            len += k_snprintf(line + len, sizeof(line) - (usize)len, " %d", status);
    }
    printf("%s]\n", line);
}

/* Runs one finished line; true if it was "exit". */
static bool run_line(char *line)
{
    const char *words[LE_LINE_MAX + 1]; /* every character a word, then NULL */
    const char **cmd[PIPELINE_MAX];
    int n = split(line, words), count;
    bool piped = false;

    for (int i = 0; i < n; i++)
        piped |= words[i] == bar;
    if (n == 0)
        return false;
    if (!piped && is_builtin(words[0])) { /* in a pipeline, cut() refuses them */
        if (n > 1)
            printf("sh: %s takes no arguments\n", words[0]);
        else if (words[0][0] == 'e')
            return true;
        else
            help();
        return false;
    }
    count = cut(words, n, cmd);
    if (count)
        run(cmd, count);
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
