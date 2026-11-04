// SPDX-License-Identifier: MIT
/*
 * Checks the file calls from ring 3: open, read, close, readdir and fstat
 * on the initrd, each error in its documented order, and the fds spawn()
 * gives a child: none from FD_FIRST, and 0-2 the parent's own or the
 * ones it names, sharing their files. The children are fstest itself,
 * told by argv[1] what to check. Any wrong answer is printed (to fd 2,
 * which one child has as its only way out) and counted. Exit status is
 * the number of failures.
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
        int n =
            k_snprintf(line, sizeof(line), "fstest: FAIL %s: got %ld, want %ld\n", what, got, want);

        sys_write(2, line, (usize)MIN(n, (int)sizeof(line) - 1));
        failures++;
    }
}

#define KPTR     ((void *)0xFFFF800000001000ull)
#define UNMAPPED ((void *)0x1000)
#define MOTD     "Welcome to minios.\n" /* its first line */

static const char read_only[16] = "rodata";                /* mapped, not writable */
static const struct dirent read_only_dirent = {.size = 1}; /* nonzero: .rodata, not .bss */
static char too_long[PATH_MAX + 1]; /* "/aaa...", PATH_MAX bytes before its NUL */
static char big[64 * 1024];         /* a whole program, across several read chunks */
extern char _end[];                 /* user/linker.ld: the page after it is unmapped */

/* "fstest child": started with the parent's fd FD_FIRST open, it must see none. */
static int child(void)
{
    struct stat st;
    char c;

    expect("child: read(FD_FIRST)", sys_read(FD_FIRST, &c, 1), -EBADF);
    expect("child: close(FD_FIRST)", sys_close(FD_FIRST), -EBADF);
    expect("child: fd 0 is the console's, as its parent's",
           sys_fstat(0, &st) == 0 && st.type == STAT_CONSOLE, 1);
    return failures;
}

/* /etc/motd's size, counted by reading it. */
static long motd_size(void)
{
    char buf[64];
    long fd = sys_open("/etc/motd"), total = 0, n;

    while ((n = sys_read((int)fd, buf, sizeof(buf))) > 0)
        total += n;
    sys_close((int)fd);
    return n < 0 ? n : total;
}

/* "fstest stdin": fd 0 is /etc/motd, 9 bytes in. The position is shared, not copied. */
static int child_stdin(void)
{
    char buf[256];
    struct stat st;
    long n = sys_read(0, buf, sizeof(buf));

    expect("stdin child: fstat(0) is the file",
           sys_fstat(0, &st) == 0 && st.type == STAT_FILE && (long)st.size == motd_size(), 1);
    expect("stdin child: reads on from the parent's position",
           n == motd_size() - 9 && memcmp(buf, &MOTD[9], strlen(MOTD) - 9) == 0, 1);
    expect("stdin child: then the end", sys_read(0, buf, 1), 0);
    expect("stdin child: write(0), a file", sys_write(0, "x", 1), -EBADF);
    expect("stdin child: close(0)", sys_close(0), -EBADF);
    expect("stdin child: still open", sys_fstat(0, &st), 0);
    expect("stdin child: nothing from FD_FIRST", sys_fstat(FD_FIRST, &st), -EBADF);
    return failures;
}

/* "fstest reader": fd 0 is /etc/motd from its start; it reads all of it. */
static int child_reader(void)
{
    char buf[256];
    long n = sys_read(0, buf, sizeof(buf));

    expect("reader: all of the file", n == motd_size() && memcmp(buf, MOTD, strlen(MOTD)) == 0, 1);
    return failures;
}

/*
 * "fstest inherit": fd 0 is /etc/motd, which its parent has already
 * closed. A grandchild spawned without stdio gets this fd 0, and reading
 * it there moves it here too.
 */
static int child_inherit(void)
{
    static const char *const reader_args[] = {"reader", NULL};
    int status = -1;
    char c;
    long pid = sys_spawnv("fstest", reader_args);

    expect("inherit: spawn(reader)", pid > 0, 1);
    expect("inherit: wait(reader)", sys_wait((int)pid, &status), pid);
    expect("inherit: reader's failures", status, 0);
    expect("inherit: the reader read it to its end here too", sys_read(0, &c, 1), 0);
    return failures;
}

/* "fstest swapped": stdio {1, 0, 2}, so fd 0 is the console's output and fd 1 its input. */
static int child_swapped(void)
{
    struct stat st;
    char c;

    expect("swapped: read(0), the console's output", sys_read(0, &c, 1), -EBADF);
    expect("swapped: write(1), the console's input", sys_write(1, "x", 1), -EBADF);
    expect("swapped: write(0) writes", sys_write(0, "", 0), 0);
    expect("swapped: fd 0 is the console", sys_fstat(0, &st) == 0 && st.type == STAT_CONSOLE, 1);
    return failures;
}

/*
 * "fstest crash": dies by a fault with files open, one closed below the
 * last, so closing them all must not stop at the first gap; the kernel
 * test checks they closed.
 */
static int child_crash(void)
{
    for (int i = 0; i < FILES_MAX; i++)
        sys_open("/etc/motd");
    sys_close(FD_FIRST + 2);
    *(volatile int *)0x1000 = 1; /* UNMAPPED */
    return 1;
}

static void bad_paths(void)
{
    too_long[0] = '/';
    for (usize i = 1; i < PATH_MAX; i++)
        too_long[i] = 'a';
    expect("open(kernel pointer)", sys_open(KPTR), -EFAULT);
    expect("open(unmapped)", sys_open(UNMAPPED), -EFAULT);
    expect("open(no NUL in PATH_MAX bytes)", sys_open(too_long), -ENAMETOOLONG);
    expect("open(relative)", sys_open("etc/motd"), -EINVAL);
    expect("open(..)", sys_open("/bin/../etc/motd"), -EINVAL);
    expect("open(missing)", sys_open("/etc/nope"), -ENOENT);
    expect("open(a directory)", sys_open("/bin"), -EISDIR);
    expect("open(the root)", sys_open("/"), -EISDIR);
    expect("open(a file with a trailing /)", sys_open("/etc/motd/"), -ENOTDIR);
    expect("open(below a file)", sys_open("/etc/motd/x"), -ENOTDIR);
}

static void reads(void)
{
    char buf[128]; /* more than all of /etc/motd */
    long fd = sys_open("/etc/motd"), n;

    expect("open(/etc/motd) is the first file fd", fd, FD_FIRST);
    expect("read(len 0)", sys_read((int)fd, buf, 0), 0);
    expect("read(kernel pointer)", sys_read((int)fd, KPTR, 8), -EFAULT);
    expect("read(range wraps)", sys_read((int)fd, buf, ~0ul), -EFAULT);
    expect("read(read-only buffer)", sys_read((int)fd, (void *)read_only, 8), -EFAULT);
    /* none of those moved the position: the file still starts at its first byte */
    n = sys_read((int)fd, buf, 9);
    expect("read 9", n, 9);
    expect("read 9: the first 9 bytes", n == 9 && memcmp(buf, MOTD, 9) == 0, 1);
    n = sys_read((int)fd, buf + 9, sizeof(buf) - 9);
    expect("read the rest: starts where the last read ended",
           n > 10 && memcmp(buf, MOTD, strlen(MOTD)) == 0, 1);
    expect("read at the end", sys_read((int)fd, buf, sizeof(buf)), 0);
    expect("write(a file)", sys_write((int)fd, "x", 1), -EBADF);
    expect("close", sys_close((int)fd), 0);
    expect("close again", sys_close((int)fd), -EBADF);
    expect("read(closed)", sys_read((int)fd, buf, 1), -EBADF);
    expect("close(0)", sys_close(0), -EBADF);
    expect("close(1)", sys_close(1), -EBADF);
    expect("read(1)", sys_read(1, buf, 1), -EBADF);
    expect("close(past the table)", sys_close(FD_FIRST + FILES_MAX), -EBADF);

    struct dirent d;
    long total = 0;

    fd = sys_open("/bin/sh");
    expect("open(/bin/sh)", fd, FD_FIRST);
    while ((n = sys_read((int)fd, big + total, sizeof(big) - (usize)total)) > 0)
        total += n;
    expect("read(/bin/sh) to the end", n, 0);
    bool listed = false;

    for (u64 i = 0; sys_readdir("/bin", i, &d) > 0; i++) {
        if (strcmp(d.name, "sh") == 0) {
            listed = true;
            expect("read(/bin/sh): every byte readdir counts", total, (long)d.size);
        }
    }
    expect("readdir(/bin) lists sh", listed, 1);
    expect("read(/bin/sh): an ELF", total > 4 && big[0] == 0x7f && big[1] == 'E', 1);
    expect("read(/bin/sh): more than one page", total > 4096, 1);
    sys_close((int)fd);

    /*
     * A buffer that runs into an unmapped page: the read stops exactly
     * there, and the next goes on from the first byte not delivered.
     */
    char *edge = (char *)(((u64)_end + 4095) & ~4095ull);

    fd = sys_open("/bin/sh");
    expect("read(200 bytes, 100 before an unmapped page)", sys_read((int)fd, edge - 100, 200), 100);
    expect("read: those are the first 100 bytes", memcmp(edge - 100, big, 100) == 0, 1);
    expect("read: goes on from byte 100", sys_read((int)fd, buf, 8), 8);
    expect("read: bytes 100 to 107", memcmp(buf, big + 100, 8) == 0, 1);
    expect("read(all of it unmapped)", sys_read((int)fd, edge, 8), -EFAULT);
    expect("read: the fault moved nothing", sys_read((int)fd, buf, 8), 8);
    expect("read: bytes 108 to 115", memcmp(buf, big + 108, 8) == 0, 1);
    sys_close((int)fd);
}

static void fd_table(void)
{
    long fds[FILES_MAX];

    for (int i = 0; i < FILES_MAX; i++) {
        fds[i] = sys_open("/etc/motd");
        expect("open: the lowest free fd", fds[i], FD_FIRST + i);
    }
    expect("open(one too many)", sys_open("/etc/motd"), -EMFILE);
    expect("close(a middle one)", sys_close((int)fds[2]), 0);
    expect("open: reuses it", sys_open("/etc/motd"), fds[2]);
    for (int i = 0; i < FILES_MAX; i++)
        expect("close all", sys_close((int)fds[i]), 0);
}

static void stats(void)
{
    struct stat st = {.reserved = 7};
    long fd = sys_open("/etc/motd");

    expect("fstat(0)", sys_fstat(0, &st), 0);
    expect("fstat(0): the console, size 0", st.type == STAT_CONSOLE && st.size == 0, 1);
    expect("fstat(0): reserved is 0", st.reserved, 0);
    expect("fstat(1): the console", sys_fstat(1, &st) == 0 && st.type == STAT_CONSOLE, 1);
    expect("fstat(2): the console", sys_fstat(2, &st) == 0 && st.type == STAT_CONSOLE, 1);
    expect("fstat(a file)", sys_fstat((int)fd, &st), 0);
    expect("fstat(a file): a file, its size",
           st.type == STAT_FILE && (long)st.size == motd_size() && st.reserved == 0, 1);
    expect("fstat(a file): more than its first line", st.size > strlen(MOTD), 1);
    expect("fstat(not open)", sys_fstat((int)fd + 1, &st), -EBADF);
    expect("fstat(past the table)", sys_fstat(FD_FIRST + FILES_MAX, &st), -EBADF);
    expect("fstat(-1)", sys_fstat(-1, &st), -EBADF);
    expect("fstat(out at a kernel pointer)", sys_fstat((int)fd, KPTR), -EFAULT);
    expect("fstat(out unmapped)", sys_fstat((int)fd, UNMAPPED), -EFAULT);
    expect("fstat(out read-only)", sys_fstat((int)fd, (struct stat *)&read_only_dirent), -EFAULT);
    expect("fstat(not open, out bad): the fd first", sys_fstat(99, KPTR), -EBADF);
    sys_close((int)fd);
}

/* spawn()'s stdio: each child above, then each error, in order, with nothing created. */
static void spawn_stdio(void)
{
    static const char *const
        stdin_args[] = {"stdin", NULL},
        *const inherit_args[] = {"inherit", NULL}, *const swapped_args[] = {"swapped", NULL},
        *const crash_args[] = {"crash", NULL}, *const nine_args[] = {"1", "2", "3", "4", "5",
                                                                     "6", "7", "8", "9", NULL};
    static const int swapped[3] = {1, 0, 2};
    char buf[16];
    int status = -1;
    long fd = sys_open("/etc/motd"), pid;
    int std[3] = {(int)fd, 1, 2};

    expect("read 9 before spawn", sys_read((int)fd, buf, 9), 9);
    pid = sys_spawn_stdio("fstest", stdin_args, std);
    expect("spawn(stdin: a file)", pid > 0, 1);
    expect("wait(stdin child)", sys_wait((int)pid, &status), pid);
    expect("stdin child's failures", status, 0);
    expect("the child's reads moved this fd's position", sys_read((int)fd, buf, 1), 0);
    sys_close((int)fd);

    fd = sys_open("/etc/motd");
    std[0] = (int)fd;
    pid = sys_spawn_stdio("fstest", inherit_args, std);
    expect("spawn(inherit)", pid > 0, 1);
    expect("close(its stdin) at once", sys_close((int)fd), 0);
    expect("wait(inherit)", sys_wait((int)pid, &status), pid);
    expect("inherit's failures", status, 0);

    pid = sys_spawn_stdio("fstest", swapped_args, swapped);
    expect("wait(swapped)", sys_wait((int)pid, &status), pid);
    expect("swapped's failures", status, 0);

    fd = sys_open("/etc/motd");
    std[0] = (int)fd;
    pid = sys_spawn_stdio("fstest", crash_args, std);
    expect("wait(crash)", sys_wait((int)pid, &status), pid);
    expect("crash: killed by #PF", status, EXIT_STATUS_FAULT(14));
    sys_close((int)fd);

    char *edge = (char *)(((u64)_end + 4095) & ~4095ull);
    int not_open[3] = {0, FD_FIRST + 1, 2}, negative[3] = {-1, 1, 2},
        past[3] = {0, 1, FD_FIRST + FILES_MAX};

    expect("spawn(stdio: an fd not open)", sys_spawn_stdio("fstest", NULL, not_open), -EBADF);
    expect("spawn(stdio: -1)", sys_spawn_stdio("fstest", NULL, negative), -EBADF);
    expect("spawn(stdio: past the table)", sys_spawn_stdio("fstest", NULL, past), -EBADF);
    expect("spawn(stdio at a kernel pointer)", sys_spawn_stdio("fstest", NULL, KPTR), -EFAULT);
    expect("spawn(stdio unmapped)", sys_spawn_stdio("fstest", NULL, UNMAPPED), -EFAULT);
    memcpy(edge - 4, &negative[1], 4); /* the first of the three mapped, the others not */
    expect("spawn(stdio running into an unmapped page)",
           sys_spawn_stdio("fstest", NULL, (const int *)(edge - 4)), -EFAULT);
    expect("spawn(bad name, bad stdio): the name first", sys_spawn_stdio(KPTR, NULL, KPTR),
           -EFAULT);
    expect("spawn(9 arguments, bad stdio): the arguments first",
           sys_spawn_stdio("fstest", nine_args, not_open), -E2BIG);
    expect("spawn(no such program, bad stdio): stdio first",
           sys_spawn_stdio("no-such-program", NULL, not_open), -EBADF);
    expect("spawn(no such program, good stdio)", sys_spawn_stdio("no-such-program", NULL, swapped),
           -ENOENT);
    expect("none of those made a child", sys_wait(-1, &status), -ECHILD);
}

static void listings(void)
{
    struct dirent d;
    bool bin = false, etc = false;
    long r;

    for (u64 i = 0; (r = sys_readdir("/", i, &d)) > 0; i++) {
        bin |= strcmp(d.name, "bin") == 0 && d.type == DIRENT_DIR && d.size == 0;
        etc |= strcmp(d.name, "etc") == 0 && d.type == DIRENT_DIR;
    }
    expect("readdir(/) ends with 0", r, 0);
    expect("readdir(/): bin/ and etc/", bin && etc, 1);
    expect("readdir(/etc, 0) is a file", sys_readdir("/etc", 0, &d) == 1 && d.type == DIRENT_FILE,
           1);
    expect("readdir(far past the end)", sys_readdir("/bin", 1000000, &d), 0);
    expect("readdir(a file)", sys_readdir("/etc/motd", 0, &d), -ENOTDIR);
    expect("readdir(missing)", sys_readdir("/nope", 0, &d), -ENOENT);
    expect("readdir(relative)", sys_readdir("bin", 0, &d), -EINVAL);
    expect("readdir(path unmapped)", sys_readdir(UNMAPPED, 0, &d), -EFAULT);
    expect("readdir(no NUL in PATH_MAX bytes)", sys_readdir(too_long, 0, &d), -ENAMETOOLONG);
    expect("readdir(a file with a trailing /)", sys_readdir("/etc/motd/", 0, &d), -ENOTDIR);
    expect("readdir(path at a kernel pointer)", sys_readdir(KPTR, 0, &d), -EFAULT);
    expect("readdir(out at a kernel pointer)", sys_readdir("/", 0, KPTR), -EFAULT);
    expect("readdir(out at a kernel pointer, path missing): the pointer first",
           sys_readdir("/nope", 0, KPTR), -EFAULT);
    expect("readdir(out unmapped)", sys_readdir("/", 0, UNMAPPED), -EFAULT);
    expect("readdir(out read-only)", sys_readdir("/", 0, (struct dirent *)&read_only_dirent),
           -EFAULT);
}

int main(int argc, char **argv)
{
    static const char *const child_args[] = {"child", NULL};
    int status = -1;

    static const struct {
        const char *name;
        int (*fn)(void);
    } children[] = {
        {"child", child},           {"stdin", child_stdin},     {"reader", child_reader},
        {"inherit", child_inherit}, {"swapped", child_swapped}, {"crash", child_crash},
    };

    for (usize i = 0; argc == 2 && i < ARRAY_SIZE(children); i++)
        if (strcmp(argv[1], children[i].name) == 0)
            return children[i].fn();
    bad_paths();
    reads();
    fd_table();
    listings();
    stats();
    spawn_stdio();

    long fd = sys_open("/etc/motd"), pid;

    expect("open before spawn", fd, FD_FIRST);
    pid = sys_spawnv("fstest", child_args);
    expect("child pid", pid > 0, 1);
    expect("wait(child)", sys_wait((int)pid, &status), pid);
    expect("child: no files inherited", status, 0);
    sys_close((int)fd);
    return failures;
}
