// SPDX-License-Identifier: MIT
/*
 * Pipes. See <fs/pipe.h>, and <uapi/syscall.h> for what programs see.
 *
 * A pipe is a ring of PIPE_SIZE bytes and two open files, its ends. Every
 * fd on an end shares that end's file, so an end closes when the last fd
 * on it does (the file's release), and the pipe goes when both have.
 * pipe->lock guards the ring and which ends are open.
 *
 * Copies to and from user memory happen outside the lock, PIPE_ATOMIC
 * bytes at a time through a buffer on the kernel stack:
 *   - read: probe that piece of the user buffer (read it and write it
 *     back, as the console's read does), then take bytes from the ring,
 *     then copy them out. A bad buffer fails before a byte is taken, and
 *     the copy out cannot fail: the process is single-threaded, so nothing
 *     can unmap the buffer in between.
 *   - write: copy a piece in, wait for room for all of it, put it in
 *     whole. So a write of at most PIPE_ATOMIC bytes is never mixed with
 *     another's.
 *
 * Waiting: a reader sleeps on `readable` until a byte is there or no
 * writer is left, a writer on `writable` until its piece fits or no reader
 * is left. The checks run inside wait_event(), after the task is on the
 * queue and marked BLOCKED, so a wakeup between a check and the sleep is
 * not lost (sched.c's argument; console input waits the same way).
 * Whoever changes what the other side waits for wakes its queue.
 *
 * Nothing can free a pipe under a task blocked in it: that task's fd
 * holds its end, and the process, single-threaded and in the call, cannot
 * close it. Closing an end wakes the other side while still holding the
 * lock, so the other end's close, which may free the pipe, cannot come in
 * between.
 */
#include <core/spinlock.h>
#include <fs/file.h>
#include <fs/pipe.h>
#include <lib/errno.h>
#include <mm/kmalloc.h>
#include <proc/uaccess.h>
#include <sched/sched.h>
#include <uapi/syscall.h>

static_assert(PIPE_ATOMIC <= PIPE_SIZE, "a piece must fit in an empty pipe");

struct pipe {
    struct spinlock lock;
    u8 *ring;
    u32 head;            /* the oldest byte */
    u32 len;             /* bytes in the ring */
    bool reader, writer; /* each end still open */
    struct wait_queue readable, writable;
};

static u32 npipes;

/*
 * A reader's wait_event() check: takes up to max bytes. True once it took
 * some, or once no writer is left (*taken 0: the end).
 */
static bool take(struct pipe *p, u8 *buf, usize max, usize *taken)
{
    irqflags_t flags = spin_lock_irqsave(&p->lock);
    usize n = MIN(max, (usize)p->len);
    bool done;

    for (usize i = 0; i < n; i++)
        buf[i] = p->ring[(p->head + i) % PIPE_SIZE];
    p->head = (u32)((p->head + n) % PIPE_SIZE);
    p->len -= (u32)n;
    done = n || !p->writer;
    spin_unlock_irqrestore(&p->lock, flags);
    *taken = n;
    return done;
}

/*
 * A writer's wait_event() check: puts all n bytes in once they fit. True
 * once it did (*in true), or once no reader is left (*in false).
 */
static bool put(struct pipe *p, const u8 *buf, usize n, bool *in)
{
    irqflags_t flags = spin_lock_irqsave(&p->lock);
    bool done;

    *in = p->reader && PIPE_SIZE - p->len >= n;
    if (*in) {
        for (usize i = 0; i < n; i++)
            p->ring[(p->head + p->len + i) % PIPE_SIZE] = buf[i];
        p->len += (u32)n;
    }
    done = *in || !p->reader;
    spin_unlock_irqrestore(&p->lock, flags);
    return done;
}

/* Whether n bytes at user_buf are mapped and writable; their contents are kept. */
static bool user_writable(u64 user_buf, u8 *tmp, usize n)
{
    return copy_from_user(tmp, (const void *)user_buf, n) == 0 &&
           copy_to_user((void *)user_buf, tmp, n) == 0;
}

/*
 * read() of a read end: -EFAULT for the range, 0 for len 0. Waits for the
 * first byte (or the end), then takes what is there, a piece at a time,
 * without waiting again.
 */
static s64 pipe_read(struct file *f, u64 user_buf, u64 len)
{
    struct pipe *p = f->pipe;
    u8 piece[PIPE_ATOMIC];
    u64 done = 0;

    if (!user_range_ok(user_buf, len))
        return -EFAULT;
    while (done < len) {
        usize want = (usize)MIN(len - done, (u64)PIPE_ATOMIC), n;

        if (!user_writable(user_buf + done, piece, want))
            return done ? (s64)done : -EFAULT;
        if (done == 0)
            wait_event(&p->readable, take(p, piece, want, &n));
        else
            (void)take(p, piece, want, &n); /* only what is there already */
        if (n == 0)
            break;
        wake_up_all(&p->writable);
        if (copy_to_user((void *)(user_buf + done), piece, n) != 0)
            return -EFAULT; /* cannot happen: probed above */
        done += n;
        if (n < want)
            break;
    }
    return (s64)done;
}

/*
 * write() to a write end: -EFAULT for the range, 0 for len 0. Each piece
 * goes in whole, waiting for room; -EPIPE (or the bytes before) once no
 * reader is left.
 */
static s64 pipe_write(struct file *f, u64 user_buf, u64 len)
{
    struct pipe *p = f->pipe;
    u8 piece[PIPE_ATOMIC];
    u64 done = 0;

    if (!user_range_ok(user_buf, len))
        return -EFAULT;
    while (done < len) {
        usize n = (usize)MIN(len - done, (u64)PIPE_ATOMIC);
        bool in;

        if (copy_from_user(piece, (const void *)(user_buf + done), n) != 0)
            return done ? (s64)done : -EFAULT;
        wait_event(&p->writable, put(p, piece, n, &in));
        if (!in)
            return done ? (s64)done : -EPIPE;
        wake_up_all(&p->readable);
        done += n;
    }
    return (s64)done;
}

static u64 pipe_size(struct file *f)
{
    struct pipe *p = f->pipe;
    irqflags_t flags = spin_lock_irqsave(&p->lock);
    u64 n = p->len;

    spin_unlock_irqrestore(&p->lock, flags);
    return n;
}

/* An end's last fd closed: the other side stops waiting, and the second end to go frees the pipe. */
static void close_end(struct file *f, bool reader)
{
    struct pipe *p = f->pipe;
    irqflags_t flags;
    bool last;

    kfree(f);
    if (!p)
        return; /* pipe_create() failed before the pipe was made */
    flags = spin_lock_irqsave(&p->lock);
    if (reader) {
        p->reader = false;
        wake_up_all(&p->writable);
    } else {
        p->writer = false;
        wake_up_all(&p->readable);
    }
    last = !p->reader && !p->writer;
    spin_unlock_irqrestore(&p->lock, flags);
    if (last) {
        kfree(p->ring);
        kfree(p);
        __atomic_fetch_sub(&npipes, 1, __ATOMIC_RELAXED);
    }
}

static void release_read_end(struct file *f)
{
    close_end(f, true);
}

static void release_write_end(struct file *f)
{
    close_end(f, false);
}

static const struct file_ops read_end_ops = {
    .type = STAT_PIPE, .read = pipe_read, .size = pipe_size, .release = release_read_end};
static const struct file_ops write_end_ops = {
    .type = STAT_PIPE, .write = pipe_write, .size = pipe_size, .release = release_write_end};

int pipe_create(struct file **rd, struct file **wr)
{
    struct pipe *p = kzalloc(sizeof(*p));
    struct file *r = file_alloc(&read_end_ops), *w = file_alloc(&write_end_ops);

    if (p)
        p->ring = kmalloc(PIPE_SIZE);
    if (!p || !p->ring || !r || !w) {
        if (p)
            kfree(p->ring);
        kfree(p);
        if (r)
            file_put(r); /* no pipe yet: its release frees only the file */
        if (w)
            file_put(w);
        return -ENOMEM;
    }
    p->lock = (struct spinlock)SPINLOCK_INIT("pipe");
    wait_queue_init(&p->readable);
    wait_queue_init(&p->writable);
    p->reader = p->writer = true;
    r->pipe = w->pipe = p;
    __atomic_fetch_add(&npipes, 1, __ATOMIC_RELAXED);
    *rd = r;
    *wr = w;
    return 0;
}

u32 pipe_count(void)
{
    return __atomic_load_n(&npipes, __ATOMIC_RELAXED);
}
