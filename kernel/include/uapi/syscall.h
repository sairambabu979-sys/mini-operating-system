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
#define SYS_SPAWN    5 /* spawn(const char *name, const char *const *args) -> child pid */
#define SYS_WAIT     6 /* wait(int pid or -1, int *status) -> pid */
#define SYS_TIME_MS  7 /* time_ms() -> milliseconds since boot */
#define SYS_READ     8 /* read(int fd, void *buf, size_t len) -> bytes; fd 0 only, blocks for >= 1 */

/*
 * spawn()'s arguments: `args` is NULL or a NULL-terminated list of at most
 * SPAWN_ARGS_MAX strings, SPAWN_ARGS_SIZE bytes in all with their NULs;
 * more is -E2BIG. The program starts as main(argc, argv), argv[0] its name
 * and argv[argc] NULL: crt0 gets argc in RDI and argv in RSI, and the
 * strings and the vector sit at the top of its stack.
 */
#define SPAWN_ARGS_MAX  8
#define SPAWN_ARGS_SIZE 256

#define SYS_COUNT 9

/* Exit status of a process killed by CPU exception `vector`. */
#define EXIT_STATUS_FAULT(vector) (128 + (vector))

#endif /* MINIOS_UAPI_SYSCALL_H */
