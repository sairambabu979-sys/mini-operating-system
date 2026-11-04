/* SPDX-License-Identifier: MIT */
/*
 * Pipes: a PIPE_SIZE-byte buffer with a read end and a write end, each an
 * open file (<fs/file.h>). The ABI is <uapi/syscall.h>'s.
 */
#ifndef MINIOS_FS_PIPE_H
#define MINIOS_FS_PIPE_H

#include <compiler.h>
#include <types.h>

struct file;

/* A new pipe: its two ends, one reference each, in *rd and *wr. 0, or -ENOMEM with nothing made. */
__must_check int pipe_create(struct file **rd, struct file **wr);

/* Pipes not yet freed (for the tests' leak checks). */
u32 pipe_count(void);

#endif /* MINIOS_FS_PIPE_H */
