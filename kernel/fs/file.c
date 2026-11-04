// SPDX-License-Identifier: MIT
/*
 * Open files and fd tables. See <fs/file.h>; the system calls that use
 * them are in kernel/proc/syscalls.c.
 */
#include <arch/paging.h>
#include <core/input.h>
#include <core/panic.h>
#include <core/preempt.h>
#include <core/printk.h>
#include <fs/file.h>
#include <lib/errno.h>
#include <mm/kmalloc.h>
#include <proc/uaccess.h>
#include <sched/sched.h>
#include <uapi/syscall.h>

#define WRITE_CHUNK 256

static u32 nfiles;

/* --- the console ------------------------------------------------------------ */

/*
 * read() of the console's input: raw bytes, at least one. The order of
 * the checks is part of the ABI: -EFAULT for the range, then 0 for an
 * empty read, then -EFAULT for a buffer that is not mapped and writable,
 * then -ENODEV with no input device. Every failure comes before anything
 * is taken off the queue, and the buffer is probed, its contents kept,
 * rather than merely range-checked (the sys_wait precedent): a bad buffer
 * must not swallow a keystroke. The process is single-threaded, so
 * nothing can unmap the buffer between the probe and the copy out.
 */
static s64 console_read(struct file *f, u64 user_buf, u64 len)
{
    u8 buf[INPUT_READ_MAX];
    usize n;

    (void)f;
    if (!user_range_ok(user_buf, len))
        return -EFAULT;
    n = (usize)MIN(len, (u64)INPUT_READ_MAX);
    if (n == 0)
        return 0;
    if (copy_from_user(buf, (const void *)user_buf, n) != 0 ||
        copy_to_user((void *)user_buf, buf, n) != 0)
        return -EFAULT;
    if (!input_present())
        return -ENODEV;
    n = input_read(buf, n);
    if (copy_to_user((void *)user_buf, buf, n) != 0)
        return -EFAULT; /* cannot happen: probed above */
    return (s64)n;
}

/* write() to the console: copied in 256-byte chunks, each one console message. */
static s64 console_write_user(struct file *f, u64 user_buf, u64 len)
{
    char chunk[WRITE_CHUNK];

    (void)f;
    if (!user_range_ok(user_buf, len))
        return -EFAULT;
    for (u64 done = 0; done < len;) {
        usize n = (usize)MIN(len - done, (u64)WRITE_CHUNK);

        if (copy_from_user(chunk, (const void *)(user_buf + done), n) != 0)
            return done ? (s64)done : -EFAULT;
        console_write(chunk, n);
        done += n;
    }
    return (s64)len;
}

static const struct file_ops console_input_ops = {.type = STAT_CONSOLE, .read = console_read};
static const struct file_ops console_output_ops = {.type = STAT_CONSOLE,
                                                   .write = console_write_user};

/* One reference each from the start, never dropped: neither is ever freed. */
struct file console_input = {.ops = &console_input_ops, .refs = 1};
struct file console_output = {.ops = &console_output_ops, .refs = 1};

/* --- initrd files ------------------------------------------------------------ */

/*
 * read() of an initrd file: from its position, as much as fits, 0 at its
 * end. -EFAULT for the range, then 0 for len 0. It copies out one user
 * page at a time: copy_to_user is all or nothing, so a fault inside a
 * page fails that page's whole piece. The first page that is not writable
 * ends the read there (-EFAULT if it was the first), and the position
 * moves by exactly the bytes before it, so no byte is lost.
 *
 * Processes that share the file share the position, so a read must be
 * one step for them: preemption stays off from reading the position to
 * moving it (one CPU; the copies never sleep). Two readers switched
 * between those points would both read from the same place, and both
 * move it, past the end.
 */
static s64 initrd_read(struct file *f, u64 user_buf, u64 len)
{
    u64 want, done = 0;

    if (!user_range_ok(user_buf, len))
        return -EFAULT;
    preempt_disable();
    want = MIN(len, f->size - f->pos);
    while (done < want) {
        u64 to_page_end = PAGE_SIZE_4K - ((user_buf + done) & (PAGE_SIZE_4K - 1));
        usize n = (usize)MIN(want - done, to_page_end);

        if (copy_to_user((void *)(user_buf + done), f->data + f->pos + done, n) != 0)
            break;
        done += n;
    }
    f->pos += done;
    preempt_enable();
    return done || want == 0 ? (s64)done : -EFAULT;
}

static void initrd_release(struct file *f)
{
    kfree(f);
}

static const struct file_ops initrd_ops = {
    .type = STAT_FILE, .read = initrd_read, .release = initrd_release};

struct file *file_open_initrd(const u8 *data, u64 size)
{
    struct file *f = kzalloc(sizeof(*f));

    if (!f)
        return NULL;
    *f = (struct file){.ops = &initrd_ops, .refs = 1, .data = data, .size = size};
    __atomic_fetch_add(&nfiles, 1, __ATOMIC_RELAXED);
    return f;
}

/* --- references ---------------------------------------------------------------- */

struct file *file_get(struct file *f)
{
    BUG_ON(__atomic_fetch_add(&f->refs, 1, __ATOMIC_RELAXED) == 0);
    return f;
}

void file_put(struct file *f)
{
    u32 was = __atomic_fetch_sub(&f->refs, 1, __ATOMIC_ACQ_REL);

    BUG_ON(was == 0);
    if (was > 1)
        return;
    BUG_ON(!f->ops->release); /* a static file lost its permanent reference */
    f->ops->release(f);
    __atomic_fetch_sub(&nfiles, 1, __ATOMIC_RELAXED);
}

u32 file_count(void)
{
    return __atomic_load_n(&nfiles, __ATOMIC_RELAXED);
}

/* --- fd tables ----------------------------------------------------------------- */

struct file *fd_get(u64 fd)
{
    struct task *self = task_current();

    return fd < ARRAY_SIZE(self->fds) ? self->fds[fd] : NULL;
}

int fd_free(int from)
{
    struct task *self = task_current();

    for (int fd = from; fd < (int)ARRAY_SIZE(self->fds); fd++)
        if (!self->fds[fd])
            return fd;
    return -EMFILE;
}

void fd_install(int fd, struct file *f)
{
    struct task *self = task_current();

    BUG_ON(fd < FD_FIRST || fd >= (int)ARRAY_SIZE(self->fds) || self->fds[fd]);
    self->fds[fd] = f;
}

int fd_close(u64 fd)
{
    struct task *self = task_current();
    struct file *f;

    if (fd < FD_FIRST || fd >= ARRAY_SIZE(self->fds) || !self->fds[fd])
        return -EBADF; /* 0-2 included: they stay open */
    f = self->fds[fd];
    self->fds[fd] = NULL;
    file_put(f);
    return 0;
}

void fd_set_std(struct task *t, struct file *const *std)
{
    static struct file *const console[FD_FIRST] = {&console_input, &console_output,
                                                   &console_output};

    if (!std)
        std = console;
    for (int fd = 0; fd < FD_FIRST; fd++) {
        BUG_ON(t->fds[fd]);
        t->fds[fd] = file_get(std[fd]);
    }
}

void fd_close_all(struct task *t)
{
    for (usize fd = 0; fd < ARRAY_SIZE(t->fds); fd++) {
        struct file *f = t->fds[fd];

        if (f) {
            t->fds[fd] = NULL;
            file_put(f);
        }
    }
}
