// SPDX-License-Identifier: MIT
/*
 * The file system at /tmp. See <fs/tmpfs.h>.
 */
#include <fs/tarfs.h>
#include <fs/tmpfs.h>
#include <lib/errno.h>
#include <lib/string.h>

/*
 * Every component of `path` after its leading "/" is a name: not "", "."
 * or "..". One trailing "/" is allowed, and "/" alone is the root.
 */
static bool components_ok(const char *path)
{
    const char *c = path + 1;

    if (*c == '\0')
        return true;
    for (;;) {
        const char *end = c;

        while (*end && *end != '/')
            end++;
        usize len = (usize)(end - c);

        if (len == 0 || (len == 1 && c[0] == '.') || (len == 2 && c[0] == '.' && c[1] == '.'))
            return false;
        if (*end == '\0' || end[1] == '\0') /* the last one, with or without a "/" */
            return true;
        c = end + 1;
    }
}

static struct tmpfs_inode *find(struct tmpfs *fs, const char *name, usize len)
{
    for (u32 i = 0; i < fs->nlinked; i++)
        if (strncmp(fs->dir[i]->name, name, len) == 0 && fs->dir[i]->name[len] == '\0')
            return fs->dir[i];
    return NULL;
}

int tmpfs_resolve(struct tmpfs *fs, const char *path, char name[TMP_NAME_MAX],
                  struct tmpfs_inode **found)
{
    const char *rest = path + 4, *end;
    struct tmpfs_inode *inode;
    usize len;

    *found = NULL;
    name[0] = '\0';
    if (strncmp(path, "/tmp", 4) != 0 || (*rest != '\0' && *rest != '/'))
        return 1;
    if (!components_ok(path))
        return -EINVAL;
    if (*rest == '\0' || rest[1] == '\0')
        return 0; /* "/tmp", "/tmp/" */
    rest++;
    for (end = rest; *end && *end != '/'; end++)
        ;
    len = (usize)(end - rest);
    if (len >= TMP_NAME_MAX)
        return -ENAMETOOLONG;
    inode = find(fs, rest, len);
    if (*end == '/') /* "/tmp/f/" or "/tmp/f/x": there are no directories in /tmp */
        return inode ? -ENOTDIR : -ENOENT;
    memcpy(name, rest, len);
    name[len] = '\0';
    *found = inode;
    return 0;
}

int tmpfs_create(struct tmpfs *fs, const char *name, struct tmpfs_inode **out)
{
    struct tmpfs_inode *inode;

    if (fs->ninodes == TMP_FILES_MAX)
        return -ENOSPC;
    inode = fs->alloc(sizeof(*inode));
    if (!inode)
        return -ENOMEM;
    strlcpy(inode->name, name, sizeof(inode->name));
    inode->linked = true;
    fs->dir[fs->nlinked++] = inode;
    fs->ninodes++;
    *out = inode;
    return 0;
}

void tmpfs_open(struct tmpfs_inode *inode)
{
    inode->opens++;
}

static void free_inode(struct tmpfs *fs, struct tmpfs_inode *inode)
{
    tmpfs_truncate(fs, inode);
    fs->free(inode);
    fs->ninodes--;
}

void tmpfs_close(struct tmpfs *fs, struct tmpfs_inode *inode)
{
    if (--inode->opens == 0 && !inode->linked)
        free_inode(fs, inode);
}

void tmpfs_unlink(struct tmpfs *fs, struct tmpfs_inode *inode)
{
    u32 i = 0;

    while (fs->dir[i] != inode)
        i++;
    for (fs->nlinked--; i < fs->nlinked; i++) /* the rest keep their order */
        fs->dir[i] = fs->dir[i + 1];
    fs->dir[fs->nlinked] = NULL;
    inode->linked = false;
    if (inode->opens == 0)
        free_inode(fs, inode);
}

void tmpfs_truncate(struct tmpfs *fs, struct tmpfs_inode *inode)
{
    for (usize i = 0; i < TMPFS_PAGES_MAX; i++) {
        if (inode->pages[i]) {
            fs->free(inode->pages[i]);
            inode->pages[i] = NULL;
            fs->npages--;
        }
    }
    inode->size = 0;
}

int tmpfs_page(struct tmpfs *fs, struct tmpfs_inode *inode, u64 pos, bool make, u8 **page)
{
    u64 index = pos / TMP_PAGE;

    *page = NULL;
    if (index >= TMPFS_PAGES_MAX)
        return make ? -ENOSPC : 0;
    *page = inode->pages[index];
    if (*page || !make)
        return 0;
    if (fs->npages == TMPFS_PAGES_MAX)
        return -ENOSPC;
    *page = fs->alloc(TMP_PAGE);
    if (!*page)
        return -ENOMEM;
    inode->pages[index] = *page;
    fs->npages++;
    return 0;
}

void tmpfs_drop_page(struct tmpfs *fs, struct tmpfs_inode *inode, u64 pos)
{
    u64 index = pos / TMP_PAGE;

    if (index < TMPFS_PAGES_MAX && inode->pages[index]) {
        fs->free(inode->pages[index]);
        inode->pages[index] = NULL;
        fs->npages--;
    }
}

int tmpfs_readdir(const struct tmpfs *fs, u64 index, struct dirent *out)
{
    const struct tmpfs_inode *inode;

    if (index >= fs->nlinked)
        return 0;
    inode = fs->dir[index];
    memset(out, 0, sizeof(*out));
    out->size = inode->size;
    out->type = DIRENT_FILE;
    strlcpy(out->name, inode->name, sizeof(out->name));
    return 1;
}

int tmpfs_root_readdir(const u8 *archive, usize len, u64 index, struct dirent *out)
{
    if (index == 0) {
        memset(out, 0, sizeof(*out));
        out->type = DIRENT_DIR;
        strlcpy(out->name, "tmp", sizeof(out->name));
        return 1;
    }
    index--;
    for (u64 i = 0; archive; i++) {
        int r = tarfs_readdir(archive, len, "/", i, out);

        if (r <= 0)
            return r;
        if (strcmp(out->name, "tmp") == 0)
            continue; /* hidden by /tmp */
        if (index-- == 0)
            return 1;
    }
    return 0;
}
