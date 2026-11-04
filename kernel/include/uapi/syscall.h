/* SPDX-License-Identifier: MIT */
/*
 * System call ABI, shared by the kernel and user programs.
 *
 *   syscall: RAX = number, args in RDI RSI RDX R10 R8 R9, result in RAX.
 *   Negative results in [-4095, -1] are errno values. RCX and R11 are
 *   clobbered; every other register is preserved.
 */
#ifndef MINIOS_UAPI_SYSCALL_H
#define MINIOS_UAPI_SYSCALL_H

#define SYS_EXIT     0 /* exit(int code): does not return */
#define SYS_WRITE    1 /* write(int fd, const void *buf, size_t len) -> len */
#define SYS_YIELD    2 /* yield() -> 0 */
#define SYS_GETPID   3 /* getpid() -> pid */
#define SYS_SLEEP_MS 4 /* sleep_ms(u64 ms) -> 0 */
#define SYS_SPAWN    5 /* spawn(const char *name, const char *const *args, const int *stdio) -> pid */
#define SYS_WAIT     6  /* wait(int pid or -1, int *status) -> pid */
#define SYS_TIME_MS  7  /* time_ms() -> milliseconds since boot */
#define SYS_READ     8  /* read(int fd, void *buf, size_t len) -> bytes; 0 at end of file */
#define SYS_OPEN     9  /* open(const char *path) -> fd, read-only */
#define SYS_CLOSE    10 /* close(int fd) -> 0 */
#define SYS_READDIR                                                                                \
    11 /* readdir(const char *path, u64 index, struct dirent *out) -> 1, or 0 past the end */
#define SYS_FSTAT 12 /* fstat(int fd, struct stat *out) -> 0 */
#define SYS_PIPE  13 /* pipe(int fds[2]) -> 0: fds[0] the read end, fds[1] the write end */

/*
 * spawn()'s arguments: `args` is NULL or a NULL-terminated list of at most
 * SPAWN_ARGS_MAX strings, SPAWN_ARGS_SIZE bytes in all with their NULs;
 * more is -E2BIG. The program starts as main(argc, argv), argv[0] its name
 * and argv[argc] NULL: crt0 gets argc in RDI and argv in RSI, and the
 * strings and the vector sit at the top of its stack.
 *
 * `stdio` is NULL, or three fds of the caller's: the child's fds 0, 1
 * and 2 are those, the same open files (a file's position is shared,
 * not copied). NULL gives it the caller's own 0, 1 and 2. A bad pointer
 * is -EFAULT and an fd that is not open -EBADF, both checked after the
 * arguments and before anything is created.
 */
#define SPAWN_ARGS_MAX  8
#define SPAWN_ARGS_SIZE 256

/*
 * Files: the initrd, read-only, by absolute path ("/bin/sh"; "/" is the
 * root, one trailing "/" is allowed, and "", ".", ".." components are
 * -EINVAL). PATH_MAX counts the NUL.
 *
 * fds 0-2 are the standard input, output and error: the console's input
 * and output (twice) for the first process, then whatever spawn() gave.
 * They stay open until the process exits (close() is -EBADF). open()
 * hands out the lowest free fd from FD_FIRST, at most FILES_MAX at once
 * (-EMFILE), and a new process starts with none of those open. Reading
 * or writing a file that cannot be read or written (the console's
 * output, an initrd file) is -EBADF. Everything still open closes when
 * the process exits, however it ends.
 */
#define PATH_MAX  256
#define FD_FIRST  3
#define FILES_MAX 8

/*
 * readdir()'s record: entry `index` of a directory, counted from 0 in the
 * archive's order. `name` is the entry's own name, not its path.
 */
#define DIRENT_FILE     1
#define DIRENT_DIR      2
#define DIRENT_NAME_MAX 256 /* with the NUL */

struct dirent {
    unsigned long long size; /* bytes; 0 for a directory */
    unsigned int type;       /* DIRENT_FILE or DIRENT_DIR */
    unsigned int reserved;   /* 0 */
    char name[DIRENT_NAME_MAX];
};

/*
 * fstat()'s record: what an open fd refers to. `type` uses readdir()'s
 * numbers where they overlap: STAT_FILE is an initrd file, `size` its
 * size; STAT_CONSOLE the console's input or output, `size` 0; STAT_PIPE
 * either end of a pipe, `size` the bytes waiting in it. -EBADF for an fd
 * that is not open, then -EFAULT unless `out` is writable.
 */
#define STAT_FILE    DIRENT_FILE
#define STAT_CONSOLE 3
#define STAT_PIPE    4

/*
 * Pipes: pipe() makes one, its two ends at the two lowest free fds from
 * FD_FIRST (-EMFILE with fewer than two free). -EFAULT unless both ints
 * of fds are writable, checked before anything is made; -ENOMEM.
 *
 * It holds PIPE_SIZE bytes. A read blocks until at least one byte is
 * there, then takes what is, up to len; once no write end is open
 * anywhere, it returns what is left and then 0. A write blocks until all
 * of it is in, PIPE_ATOMIC bytes at a time, each piece whole: a write of
 * at most PIPE_ATOMIC bytes is never mixed with another's. Once no read
 * end is open anywhere, a write returns the bytes it put in before that,
 * or -EPIPE if none. Reading the write end or writing the read end is
 * -EBADF. Either call goes PIPE_ATOMIC bytes at a time, and a piece of
 * the buffer that is not readable (write) or writable (read) ends it
 * before any of that piece moves: -EFAULT if it is the first, else the
 * bytes moved before it. So a bad buffer never loses a byte.
 */
#define PIPE_SIZE   4096
#define PIPE_ATOMIC 256

struct stat {
    unsigned long long size;
    unsigned int type;     /* STAT_* */
    unsigned int reserved; /* 0 */
};

#define SYS_COUNT 14

/* Exit status of a process killed by CPU exception `vector`. */
#define EXIT_STATUS_FAULT(vector) (128 + (vector))

#endif /* MINIOS_UAPI_SYSCALL_H */
