// SPDX-License-Identifier: MIT
/*
 * Paths to files: /tmp and the initrd. See <fs/vfs.h>.
 *
 * Every use of /tmp holds tmp_lock, a sleeping mutex, including the
 * copies to and from user memory: they never sleep, but a long one may
 * be preempted, and the lock keeps another process off the file meanwhile.
 * It also orders every read and write of a file's position, which the fds
 * spawn() shares between processes share too.
 */
#include <arch/paging.h>
#include <fs/file.h>
#include <fs/initrd.h>
#include <fs/tarfs.h>
#include <fs/tmpfs.h>
#include <fs/vfs.h>
#include <lib/errno.h>
#include <lib/string.h>
#include <mm/kmalloc.h>
#include <proc/uaccess.h>
#include <sched/mutex.h>

static void *tmp_alloc(usize size)
{
    return kzalloc(size);
}

static struct mutex tmp_lock = MUTEX_INIT(tmp_lock, "tmp");
static struct tmpfs tmp = {.alloc = tmp_alloc, .free = kfree};
static const u8 zeros[TMP_PAGE]; /* what a page never written reads as */

/* The most of `left` that stays inside one tmp page at `pos` and one user page at `user`. */
static usize piece(u64 left, u64 pos, u64 user)
{
    u64 n = MIN(left, TMP_PAGE - pos % TMP_PAGE);

    return (usize)MIN(n, PAGE_SIZE_4K - user % PAGE_SIZE_4K);
}

/*
 * read() of a /tmp file: -EBADF unless opened to read, -EFAULT for the
 * range, 0 for len 0 or at the end. Copied out a piece at a time (piece()):
 * the first piece that is not writable ends the read there (-EFAULT if it
 * was the first), and the position moves by exactly what was copied.
 */
static s64 tmp_read(struct file *f, u64 user_buf, u64 len)
{
    struct tmpfs_inode *in = f->inode;
    u64 want, done = 0;

    if ((f->flags & O_ACCMODE) == O_WRONLY)
        return -EBADF;
    if (!user_range_ok(user_buf, len))
        return -EFAULT;
    mutex_lock(&tmp_lock);
    want = in->size > f->pos ? MIN(len, in->size - f->pos) : 0;
    while (done < want) {
        u64 pos = f->pos + done;
        usize n = piece(want - done, pos, user_buf + done);
        u8 *page;

        (void)tmpfs_page(&tmp, in, pos, false, &page);
        if (copy_to_user((void *)(user_buf + done), (page ? page : zeros) + pos % TMP_PAGE, n))
            break;
        done += n;
    }
    f->pos += done;
    mutex_unlock(&tmp_lock);
    return done || want == 0 ? (s64)done : -EFAULT;
}

/*
 * write() to a /tmp file: -EBADF unless opened to write, -EFAULT for the
 * range, 0 for len 0. With O_APPEND it starts at the end. A piece at a
 * time: a page that cannot be had (-ENOSPC, -ENOMEM) or a piece that is
 * not readable (-EFAULT) ends it, with that error if nothing was written,
 * else the bytes written. The size grows only by what was copied, and a
 * page made for a piece that faulted is given back, so a failed write
 * takes no space.
 */
static s64 tmp_write(struct file *f, u64 user_buf, u64 len)
{
    struct tmpfs_inode *in = f->inode;
    u64 done = 0;
    int err = 0;

    if ((f->flags & O_ACCMODE) == O_RDONLY)
        return -EBADF;
    if (!user_range_ok(user_buf, len))
        return -EFAULT;
    if (len == 0)
        return 0;
    mutex_lock(&tmp_lock);
    if (f->flags & O_APPEND)
        f->pos = in->size;
    while (done < len) {
        u64 pos = f->pos + done;
        usize n = piece(len - done, pos, user_buf + done);
        u8 *page, *had;

        (void)tmpfs_page(&tmp, in, pos, false, &had);
        err = tmpfs_page(&tmp, in, pos, true, &page);
        if (err)
            break;
        if (copy_from_user(page + pos % TMP_PAGE, (const void *)(user_buf + done), n)) {
            if (!had)
                tmpfs_drop_page(&tmp, in, pos);
            err = -EFAULT;
            break;
        }
        done += n;
        in->size = MAX(in->size, pos + n);
    }
    f->pos += done;
    mutex_unlock(&tmp_lock);
    return done ? (s64)done : err;
}

static u64 tmp_size(struct file *f)
{
    u64 size;

    mutex_lock(&tmp_lock);
    size = f->inode->size;
    mutex_unlock(&tmp_lock);
    return size;
}

static void tmp_release(struct file *f)
{
    mutex_lock(&tmp_lock);
    tmpfs_close(&tmp, f->inode);
    mutex_unlock(&tmp_lock);
    kfree(f);
}

static const struct file_ops tmp_ops = {.type = STAT_FILE,
                                        .read = tmp_read,
                                        .write = tmp_write,
                                        .size = tmp_size,
                                        .release = tmp_release};

static bool flags_ok(u32 flags)
{
    u32 mode = flags & O_ACCMODE;

    if (flags & ~(u32)(O_ACCMODE | O_CREAT | O_TRUNC | O_APPEND) || mode == O_ACCMODE)
        return false;
    return mode != O_RDONLY || !(flags & (O_TRUNC | O_APPEND));
}

static int initrd_lookup(const char *path, struct tarfs_node *node)
{
    usize len;
    const u8 *rd = initrd_get(&len);

    return rd ? tarfs_lookup(rd, len, path, node) : -ENOENT;
}

/*
 * -EROFS if `path`, missing from the initrd, names a file the initrd
 * could have held: its parent is a directory there. Otherwise -ENOENT.
 */
static int missing_in_initrd(const char *path)
{
    char parent[PATH_MAX];
    struct tarfs_node node;
    usize len = strlen(path), cut = 0;

    if (path[len - 1] == '/') /* names a directory: nothing to create */
        return -ENOENT;
    memcpy(parent, path, len + 1);
    for (usize i = 0; i < len; i++) /* the last "/" */
        if (parent[i] == '/')
            cut = i;
    parent[cut == 0 ? 1 : cut] = '\0'; /* "/x" -> "/", "/a/x" -> "/a" */
    return initrd_lookup(parent, &node) == 0 && node.is_dir ? -EROFS : -ENOENT;
}

static int open_initrd(const char *path, u32 flags, bool have_fd, struct file **out)
{
    struct tarfs_node node = {0};
    int err = initrd_lookup(path, &node);

    if (err == -ENOENT && (flags & O_CREAT))
        err = missing_in_initrd(path);
    if (err)
        return err;
    if (node.is_dir)
        return -EISDIR;
    if ((flags & O_ACCMODE) != O_RDONLY)
        return -EROFS;
    if (!have_fd)
        return -EMFILE;
    *out = file_open_initrd(node.data, node.size);
    return *out ? 0 : -ENOMEM;
}

int vfs_open(const char *path, u32 flags, bool have_fd, struct file **out)
{
    char name[TMP_NAME_MAX];
    struct tmpfs_inode *in;
    struct file *f;
    bool made = false;
    int r;

    if (!flags_ok(flags))
        return -EINVAL;
    mutex_lock(&tmp_lock);
    r = tmpfs_resolve(&tmp, path, name, &in);
    if (r == 1) {
        mutex_unlock(&tmp_lock);
        return open_initrd(path, flags, have_fd, out);
    }
    if (r == 0 && name[0] == '\0')
        r = -EISDIR;
    else if (r == 0 && !in && !(flags & O_CREAT))
        r = -ENOENT;
    else if (r == 0 && !have_fd)
        r = -EMFILE;
    if (r == 0 && !in) {
        r = tmpfs_create(&tmp, name, &in);
        made = r == 0;
    }
    f = r == 0 ? file_alloc(&tmp_ops) : NULL;
    if (r == 0 && !f) {
        if (made)
            tmpfs_unlink(&tmp, in); /* not open: freed */
        r = -ENOMEM;
    }
    if (r == 0) {
        if (!made && (flags & O_TRUNC))
            tmpfs_truncate(&tmp, in);
        tmpfs_open(in);
        f->inode = in;
        f->flags = flags;
        *out = f;
    }
    mutex_unlock(&tmp_lock);
    return r;
}

int vfs_unlink(const char *path)
{
    char name[TMP_NAME_MAX];
    struct tmpfs_inode *in;
    struct tarfs_node node;
    int r;

    mutex_lock(&tmp_lock);
    r = tmpfs_resolve(&tmp, path, name, &in);
    if (r == 0 && name[0] == '\0')
        r = -EISDIR;
    else if (r == 0 && !in)
        r = -ENOENT;
    else if (r == 0)
        tmpfs_unlink(&tmp, in);
    mutex_unlock(&tmp_lock);
    if (r != 1)
        return r;
    r = initrd_lookup(path, &node);
    if (r)
        return r;
    return node.is_dir ? -EISDIR : -EROFS;
}

int vfs_readdir(const char *path, u64 index, struct dirent *out)
{
    char name[TMP_NAME_MAX];
    struct tmpfs_inode *in;
    usize len;
    const u8 *rd = initrd_get(&len);
    int r;

    if (strcmp(path, "/") == 0)
        return tmpfs_root_readdir(rd, len, index, out);
    mutex_lock(&tmp_lock);
    r = tmpfs_resolve(&tmp, path, name, &in);
    bool in_tmp = r != 1;

    if (r == 0 && name[0] == '\0')
        r = tmpfs_readdir(&tmp, index, out);
    else if (r == 0)
        r = in ? -ENOTDIR : -ENOENT;
    mutex_unlock(&tmp_lock);
    if (in_tmp)
        return r;
    return rd ? tarfs_readdir(rd, len, path, index, out) : -ENOENT;
}

u32 vfs_tmp_files(void)
{
    return __atomic_load_n(&tmp.ninodes, __ATOMIC_RELAXED);
}

u32 vfs_tmp_pages(void)
{
    return __atomic_load_n(&tmp.npages, __ATOMIC_RELAXED);
}
