/* SPDX-License-Identifier: MIT */
/*
 * Error codes. Kernel functions return 0 or a negative errno; values match
 * Linux so they are familiar in logs and, later, at the syscall boundary.
 */
#ifndef MINIOS_LIB_ERRNO_H
#define MINIOS_LIB_ERRNO_H

#define EPERM        1
#define ENOENT       2
#define ESRCH        3
#define EINTR        4
#define EIO          5
#define E2BIG        7
#define ENOEXEC      8
#define EBADF        9
#define ECHILD       10
#define EAGAIN       11
#define ENOMEM       12
#define EFAULT       14
#define EBUSY        16
#define EEXIST       17
#define ENODEV       19
#define ENOTDIR      20
#define EISDIR       21
#define EINVAL       22
#define EMFILE       24
#define ENOSPC       28
#define EROFS        30
#define ENAMETOOLONG 36
#define ENOSYS       38

#endif /* MINIOS_LIB_ERRNO_H */
