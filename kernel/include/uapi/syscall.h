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

#define SYS_EXIT     0  /* exit(int code): does not return */
#define SYS_WRITE    1  /* write(int fd, const void *buf, size_t len) -> len */
#define SYS_YIELD    2  /* yield() -> 0 */
#define SYS_GETPID   3  /* getpid() -> pid */
#define SYS_SLEEP_MS 4  /* sleep_ms(u64 ms) -> 0 */
#define SYS_SPAWN    5  /* spawn(const char *name, const char *const *args) -> child pid */
#define SYS_WAIT     6  /* wait(int pid or -1, int *status) -> pid */
#define SYS_TIME_MS  7  /* time_ms() -> milliseconds since boot */
#define SYS_READ     8  /* read(int fd, void *buf, size_t len) -> bytes; 0 at end of file */
#define SYS_OPEN     9  /* open(const char *path) -> fd, read-only */
#define SYS_CLOSE    10 /* close(int fd) -> 0 */
#define SYS_READDIR                                                                                \
    11 /* readdir(const char *path, u64 index, struct dirent *out) -> 1, or 0 past the end */

/*
 * spawn()'s arguments: `args` is NULL or a NULL-terminated list of at most
 * SPAWN_ARGS_MAX strings, SPAWN_ARGS_SIZE bytes in all with their NULs;
 * more is -E2BIG. The program starts as main(argc, argv), argv[0] its name
 * and argv[argc] NULL: crt0 gets argc in RDI and argv in RSI, and the
 * strings and the vector sit at the top of its stack.
 */
#define SPAWN_ARGS_MAX  8
#define SPAWN_ARGS_SIZE 256

/*
 * Files: the initrd, read-only, by absolute path ("/bin/sh"; "/" is the
 * root, one trailing "/" is allowed, and "", ".", ".." components are
 * -EINVAL). PATH_MAX counts the NUL. fds 0-2 are the console; open()
 * hands out the lowest free fd from FD_FIRST, at most FILES_MAX at once
 * (-EMFILE), and a new process starts with none open.
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

#define SYS_COUNT 12

/* Exit status of a process killed by CPU exception `vector`. */
#define EXIT_STATUS_FAULT(vector) (128 + (vector))

#endif /* MINIOS_UAPI_SYSCALL_H */
