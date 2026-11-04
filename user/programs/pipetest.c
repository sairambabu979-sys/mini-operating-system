// SPDX-License-Identifier: MIT
/*
 * Checks pipe() from ring 3: the fds it gives, each error in its order,
 * reads and writes that block and wake, the end of input once no writer
 * is left, -EPIPE once no reader is, buffers that fail without losing a
 * byte, writes of up to PIPE_ATOMIC bytes that never mix, and cat between
 * two pipes. The children are pipetest itself, told by argv[1] what to do.
 * Any wrong answer is printed (to fd 2: some children's fd 1 is a pipe)
 * and counted. Exit status is the number of failures.
 */
#include "libsys.h"

#include <lib/errno.h>
#include <lib/printf.h>
#include <lib/string.h>

static int failures;

static void expect(const char *what, long got, long want)
{
    char line[160];

    if (got != want) {
        int n = k_snprintf(line, sizeof(line), "pipetest: FAIL %s: got %ld, want %ld\n", what, got,
                           want);

        sys_write(2, line, (usize)MIN(n, (int)sizeof(line) - 1));
        failures++;
    }
}

#define KPTR       ((void *)0xFFFF800000001000ull)
#define UNMAPPED   ((void *)0x1000)
#define STREAM_LEN 10000L /* more than two pipes' worth: the writer must wait */
#define RECORD     200    /* at most PIPE_ATOMIC: written whole */
#define RECORDS    64

static const int read_only[2] = {1, 2}; /* mapped, not writable */
static char big[16 * 1024];
extern char _end[]; /* user/linker.ld: the page after it is unmapped */

/* The stream the writer children send: byte i is i % 251. */
static char stream_byte(long i)
{
    return (char)(i % 251);
}

/* "pipetest writer": fd 1 is a pipe; sends STREAM_LEN bytes in uneven writes. */
static int child_writer(void)
{
    char buf[1000];
    long sent = 0;

    while (sent < STREAM_LEN) {
        long n = MIN(STREAM_LEN - sent, (long)sizeof(buf) - sent % 7);

        for (long i = 0; i < n; i++)
            buf[i] = stream_byte(sent + i);
        if (sys_write(1, buf, (usize)n) != n)
            return 1;
        sent += n;
    }
    return 0;
}

/* "pipetest reader": fd 0 is a pipe; takes the stream to its end and checks every byte. */
static int child_reader(void)
{
    char buf[700];
    long got = 0, n;

    while ((n = sys_read(0, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < n; i++)
            if (buf[i] != stream_byte(got + i))
                return 2;
        got += n;
    }
    return n == 0 && got == STREAM_LEN ? 0 : 1;
}

/*
 * "pipetest blocked": fd 1 is a pipe nobody reads. Its 2 * PIPE_SIZE
 * write fills the pipe and waits; when the reader closes, the write
 * returns what went in, and the next is -EPIPE.
 */
static int child_blocked(void)
{
    long first = sys_write(1, big, 2ul * PIPE_SIZE);

    if (first != PIPE_SIZE)
        return 1;
    return sys_write(1, "x", 1) == -EPIPE ? 0 : 2;
}

/* "pipetest records X": fd 1 is a pipe; RECORDS writes of RECORD bytes, all X but a final \n. */
static int child_records(char x)
{
    char rec[RECORD];

    memset(rec, x, sizeof(rec) - 1);
    rec[RECORD - 1] = '\n';
    for (int i = 0; i < RECORDS; i++)
        if (sys_write(1, rec, sizeof(rec)) != RECORD)
            return 1;
    return 0;
}

static void basics(void)
{
    int fds[2] = {-1, -1};
    struct stat st = {0};
    char buf[64];

    expect("pipe", sys_pipe(fds), 0);
    expect("pipe: the read end at the lowest free fd", fds[0], FD_FIRST);
    expect("pipe: the write end at the next", fds[1], FD_FIRST + 1);
    expect("fstat(read end)", sys_fstat(fds[0], &st), 0);
    expect("fstat(read end): a pipe, empty", st.type == STAT_PIPE && st.size == 0, 1);
    expect("write 5", sys_write(fds[1], "hello", 5), 5);
    expect("fstat(write end): 5 waiting",
           sys_fstat(fds[1], &st) == 0 && st.type == STAT_PIPE && st.size == 5, 1);
    expect("read: all that is there", sys_read(fds[0], buf, sizeof(buf)), 5);
    expect("read: those bytes", memcmp(buf, "hello", 5), 0);
    expect("read(len 0): no wait", sys_read(fds[0], buf, 0), 0);
    expect("write(len 0)", sys_write(fds[1], buf, 0), 0);
    expect("read(the write end)", sys_read(fds[1], buf, 1), -EBADF);
    expect("write(the read end)", sys_write(fds[0], "x", 1), -EBADF);
    expect("read(range wraps)", sys_read(fds[0], buf, ~0ul), -EFAULT);

    /* exactly PIPE_SIZE fits without a wait, and comes back in one read */
    for (usize i = 0; i < PIPE_SIZE; i++)
        big[i] = (char)i;
    expect("write PIPE_SIZE", sys_write(fds[1], big, PIPE_SIZE), PIPE_SIZE);
    expect("fstat: full", sys_fstat(fds[0], &st) == 0 && st.size == PIPE_SIZE, 1);
    memset(big, 0, PIPE_SIZE);
    expect("read it all at once", sys_read(fds[0], big, sizeof(big)), PIPE_SIZE);
    expect("read: every byte, in order",
           big[0] == 0 && big[255] == (char)255 && big[PIPE_SIZE - 1] == (char)(PIPE_SIZE - 1), 1);

    /* the end: what is left, then 0, every time */
    expect("write 2", sys_write(fds[1], "xy", 2), 2);
    expect("close(write end)", sys_close(fds[1]), 0);
    expect("read after the writer went: what is left", sys_read(fds[0], buf, sizeof(buf)), 2);
    expect("then 0", sys_read(fds[0], buf, sizeof(buf)), 0);
    expect("and 0 again", sys_read(fds[0], buf, sizeof(buf)), 0);
    expect("close(read end)", sys_close(fds[0]), 0);

    /* no reader: -EPIPE */
    expect("pipe again: the same fds", sys_pipe(fds) == 0 && fds[0] == FD_FIRST, 1);
    expect("close(read end)", sys_close(fds[0]), 0);
    expect("write with no reader", sys_write(fds[1], "x", 1), -EPIPE);
    expect("write(len 0) with no reader", sys_write(fds[1], "x", 0), 0);
    expect("close(write end)", sys_close(fds[1]), 0);
}

static void errors(void)
{
    char *edge = (char *)(((u64)_end + 4095) & ~4095ull);
    int fds[2] = {77, 88};
    long opened[FILES_MAX];

    expect("pipe(kernel pointer)", sys_pipe(KPTR), -EFAULT);
    expect("pipe(unmapped)", sys_pipe(UNMAPPED), -EFAULT);
    expect("pipe(read-only)", sys_pipe((int *)read_only), -EFAULT);
    expect("pipe(running into an unmapped page)", sys_pipe((int *)(edge - 4)), -EFAULT);
    expect("none of those took an fd", sys_open("/etc/motd"), FD_FIRST);
    sys_close(FD_FIRST);

    for (int i = 0; i < FILES_MAX - 1; i++)
        opened[i] = sys_open("/etc/motd");
    expect("pipe with one fd free", sys_pipe(fds), -EMFILE);
    expect("pipe: -EMFILE left fds as it was", fds[0] == 77 && fds[1] == 88, 1);
    expect("pipe: and took no fd", sys_open("/etc/motd"), FD_FIRST + FILES_MAX - 1);
    expect("pipe with none free", sys_pipe(fds), -EMFILE);
    sys_close(FD_FIRST + FILES_MAX - 1);
    sys_close((int)opened[1]);
    sys_close((int)opened[4]);
    expect("pipe with two free, apart", sys_pipe(fds), 0);
    expect("pipe: in the two lowest", fds[0] == opened[1] && fds[1] == opened[4], 1);
    sys_close(fds[0]);
    sys_close(fds[1]);
    for (int i = 0; i < FILES_MAX - 1; i++)
        if (i != 1 && i != 4)
            sys_close((int)opened[i]);
}

/*
 * Bad buffers fail before a byte moves, and a read that runs into one
 * keeps what is past it. The 256 bytes before `edge` are the end of .bss:
 * big's tail and libsys's statics, which pipetest uses for nothing else.
 */
static void bad_buffers(void)
{
    char *edge = (char *)(((u64)_end + 4095) & ~4095ull);
    int fds[2];
    char buf[64];

    expect("pipe", sys_pipe(fds), 0);
    expect("write 3", sys_write(fds[1], "abc", 3), 3);
    expect("read(kernel pointer)", sys_read(fds[0], KPTR, 3), -EFAULT);
    expect("read(read-only)", sys_read(fds[0], (void *)read_only, 3), -EFAULT);
    expect("read(unmapped)", sys_read(fds[0], UNMAPPED, 3), -EFAULT);
    expect("read: nothing was lost", sys_read(fds[0], buf, sizeof(buf)), 3);
    expect("read: abc", memcmp(buf, "abc", 3), 0);
    expect("write(kernel pointer)", sys_write(fds[1], KPTR, 3), -EFAULT);
    expect("write(unmapped)", sys_write(fds[1], UNMAPPED, 3), -EFAULT);

    struct stat st = {0};

    expect("those wrote nothing", sys_fstat(fds[0], &st) == 0 && st.size == 0, 1);

    /* 300 bytes into a buffer whose first 256 are mapped: the first piece moves, the rest stays */
    for (int i = 0; i < 300; i++)
        big[i] = (char)i;
    expect("write 300", sys_write(fds[1], big, 300), 300);
    expect("read into a buffer that runs out after 256", sys_read(fds[0], edge - 256, 300),
           PIPE_ATOMIC);
    expect("read: the first 256", memcmp(edge - 256, big, 256), 0);
    expect("read: the other 44 are still there", sys_read(fds[0], buf, sizeof(buf)), 44);
    expect("read: bytes 256 to 299", memcmp(buf, big + 256, 44), 0);

    /* a write whose second piece is unmapped: the first goes in, and the count says so */
    memcpy(edge - 256, big, 256);
    expect("write running into an unmapped page", sys_write(fds[1], edge - 256, 300), PIPE_ATOMIC);
    expect("fstat: the first piece is in", sys_fstat(fds[0], &st) == 0 && st.size == 256, 1);
    sys_close(fds[0]);
    sys_close(fds[1]);
}

/* Runs `pipetest NAME` with the given stdio and returns its pid. */
static long spawn_child(const char *name, const int stdio[3])
{
    const char *args[] = {name, NULL};

    return sys_spawn_stdio("pipetest", args, stdio);
}

static int wait_for(long pid)
{
    int status = -1;

    if (sys_wait((int)pid, &status) != pid)
        return -1;
    return status;
}

static void children(void)
{
    int fds[2], status;
    long got = 0, n, pid, pid2;

    /* a writer that must wait for the reader: every byte arrives, in order, then the end */
    expect("pipe", sys_pipe(fds), 0);
    pid = spawn_child("writer", (int[3]){0, fds[1], 2});
    expect("spawn(writer)", pid > 0, 1);
    sys_close(fds[1]); /* the child's is now the only write end */
    bool in_order = true;

    while ((n = sys_read(fds[0], big, 3000)) > 0) {
        for (long i = 0; i < n; i++)
            in_order &= big[i] == stream_byte(got + i);
        got += n;
    }
    expect("writer: the end", n, 0);
    expect("writer: every byte", got, STREAM_LEN);
    expect("writer: in order", in_order, 1);
    expect("writer: status", wait_for(pid), 0);
    sys_close(fds[0]);

    /* a reader that must wait for the writer, and sees the end when the last writer closes */
    expect("pipe", sys_pipe(fds), 0);
    pid = spawn_child("reader", (int[3]){fds[0], 1, 2});
    sys_close(fds[0]);
    for (long sent = 0; sent < STREAM_LEN; sent += n) {
        n = MIN(STREAM_LEN - sent, 1500L);
        for (long i = 0; i < n; i++)
            big[i] = stream_byte(sent + i);
        expect("write to the reader", sys_write(fds[1], big, (usize)n), n);
        sys_yield();
    }
    sys_close(fds[1]);
    expect("reader: status (0: all of it, in order, then the end)", wait_for(pid), 0);

    /* a writer waiting on a full pipe when its reader goes */
    expect("pipe", sys_pipe(fds), 0);
    pid = spawn_child("blocked", (int[3]){0, fds[1], 2});
    sys_close(fds[1]);
    struct stat st = {0};

    for (int i = 0; i < 1000 && (sys_fstat(fds[0], &st) != 0 || st.size < PIPE_SIZE); i++)
        sys_sleep_ms(1);
    expect("blocked: the pipe filled", st.size == PIPE_SIZE, 1);
    sys_close(fds[0]);
    expect("blocked: status (0: PIPE_SIZE, then -EPIPE)", wait_for(pid), 0);

    /* two writers of whole records into one pipe: no record is split */
    expect("pipe", sys_pipe(fds), 0);
    pid = spawn_child("A", (int[3]){0, fds[1], 2});
    pid2 = spawn_child("B", (int[3]){0, fds[1], 2});
    sys_close(fds[1]);
    int records[2] = {0, 0}, broken = 0;
    long have = 0;

    while ((n = sys_read(fds[0], big + have, (usize)(RECORD - have))) > 0) {
        have += n;
        if (have < RECORD)
            continue;
        char x = big[0];

        for (int i = 1; i < RECORD - 1; i++)
            broken += big[i] != x;
        broken += big[RECORD - 1] != '\n' || (x != 'A' && x != 'B');
        records[x == 'B']++;
        have = 0;
    }
    expect("records: none split", broken, 0);
    expect("records: all of A's", records[0], RECORDS);
    expect("records: all of B's", records[1], RECORDS);
    expect("records: nothing left over", have, 0);
    expect("A: status", wait_for(pid), 0);
    expect("B: status", wait_for(pid2), 0);
    sys_close(fds[0]);

    /* cat between two pipes: what goes in comes out */
    int in[2], out[2];

    expect("pipes for cat", sys_pipe(in) == 0 && sys_pipe(out) == 0, 1);
    pid = sys_spawn_stdio("cat", NULL, (int[3]){in[0], out[1], 2});
    expect("spawn(cat)", pid > 0, 1);
    sys_close(in[0]);
    sys_close(out[1]);
    for (long i = 0; i < 5000; i++)
        big[i] = stream_byte(i);
    expect("write 5000 to cat", sys_write(in[1], big, 5000), 5000);
    sys_close(in[1]);
    memset(big, 0, 5000);
    got = 0;
    while ((n = sys_read(out[0], big + got, sizeof(big) - (usize)got)) > 0)
        got += n;
    expect("cat: as many bytes out", got, 5000);
    expect("cat: the same bytes",
           big[0] == 0 && big[250] == (char)250 && big[251] == 0 && big[4999] == stream_byte(4999),
           1);
    expect("cat: status", wait_for(pid), 0);
    sys_close(out[0]);
    expect("every child reaped", sys_wait(-1, &status), -ECHILD);
}

int main(int argc, char **argv)
{
    if (argc == 2) {
        if (strcmp(argv[1], "writer") == 0)
            return child_writer();
        if (strcmp(argv[1], "reader") == 0)
            return child_reader();
        if (strcmp(argv[1], "blocked") == 0)
            return child_blocked();
        if (strcmp(argv[1], "A") == 0 || strcmp(argv[1], "B") == 0)
            return child_records(argv[1][0]);
    }
    basics();
    errors();
    bad_buffers();
    children();
    return failures;
}
