/* SPDX-License-Identifier: MIT */
/*
 * Open files: what a process's fds refer to.
 *
 * A file is one open thing: the console's input, the console's output, or
 * an initrd file with its read position. Every fd that refers to it, in
 * one process or several (spawn() hands a child its parent's), shares it,
 * position included, and counts as one reference; the last to go frees
 * it. The console's two files are static and never freed.
 *
 * A process's fds are its task's fds[]: 0-2 its standard input, output
 * and error, set before it first runs and open until it exits; from
 * FD_FIRST, the files it opened. fd_close_all() closes them all when it
 * exits. Each process is single-threaded and only it changes its own
 * table, so the table needs no lock; a file's count is atomic, as several
 * processes share it.
 */
#ifndef MINIOS_FS_FILE_H
#define MINIOS_FS_FILE_H

#include <compiler.h>
#include <types.h>

struct file;
struct task;

struct file_ops {
    unsigned type; /* STAT_*, for fstat() */
    /*
     * read() and write() on an open fd of this kind, with the caller's
     * arguments, unchecked. NULL: the kind cannot be read (or written),
     * -EBADF.
     */
    s64 (*read)(struct file *f, u64 user_buf, u64 len);
    s64 (*write)(struct file *f, u64 user_buf, u64 len);
    /* The last reference went. NULL for a static file, which never loses its last. */
    void (*release)(struct file *f);
};

struct file {
    const struct file_ops *ops;
    u32 refs; /* atomic */
    /* An initrd file: a byte range of the never-freed initrd, and the read position. */
    const u8 *data;
    u64 size;
    u64 pos;
};

/* The console: fd 0 of the first process, and fds 1 and 2. */
extern struct file console_input, console_output;

/* A new open initrd file on [data, data + size), holding one reference; NULL without memory. */
struct file *file_open_initrd(const u8 *data, u64 size);

/* Another reference to f, which the caller already holds one of; returns f. */
struct file *file_get(struct file *f);

/* Drops a reference; the last frees the file. */
void file_put(struct file *f);

/* Files allocated and not yet freed (the console's static two are not counted). */
u32 file_count(void);

/* The current task's open file at fd, or NULL (not open, or no such fd). */
struct file *fd_get(u64 fd);

/* The lowest fd from `from` not open in the current task, or -EMFILE. */
int fd_free(int from);

/* Puts f (the caller's reference, which it keeps) at a free fd of the current task. */
void fd_install(int fd, struct file *f);

/* close(): an fd from FD_FIRST that is open. -EBADF for 0-2, or one not open. */
int fd_close(u64 fd);

/*
 * Gives a task that has not run yet its fds 0-2: a reference to each of
 * std[0..2], or the console's when std is NULL.
 */
void fd_set_std(struct task *t, struct file *const *std);

/* Closes every fd of t (at exit: t is the current task). */
void fd_close_all(struct task *t);

#endif /* MINIOS_FS_FILE_H */
