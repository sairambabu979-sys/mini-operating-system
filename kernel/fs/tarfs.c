// SPDX-License-Identifier: MIT
/*
 * Paths over a ustar archive. See <fs/tarfs.h>.
 */
#include <fs/tarfs.h>
#include <lib/errno.h>
#include <lib/string.h>
#include <lib/tar.h>

/* The archive name for `path` ("" for the root) into name[TAR_NAME_MAX]; 0 or an errno. */
static int archive_name(const char *path, char *name)
{
    usize n;

    if (path[0] != '/')
        return -EINVAL;
    path++;
    n = strnlen(path, TAR_NAME_MAX);
    if (n == TAR_NAME_MAX)
        return -ENAMETOOLONG;
    memcpy(name, path, n + 1);
    if (n > 0 && name[n - 1] == '/') /* "/bin/" is "/bin" */
        name[--n] = '\0';
    if (n == 0)
        return path[0] == '\0' ? 0 : -EINVAL; /* "/" is the root; "//" is not */
    for (const char *c = name;;) {            /* each component: not "", ".", ".." */
        const char *end = c;

        while (*end && *end != '/')
            end++;
        usize clen = (usize)(end - c);

        if (clen == 0 || (clen == 1 && c[0] == '.') || (clen == 2 && c[0] == '.' && c[1] == '.'))
            return -EINVAL;
        if (!*end)
            return 0;
        c = end + 1;
    }
}

/* Looks up an archive name ("" is the root). */
static int find(const u8 *archive, usize len, const char *name, struct tarfs_node *out)
{
    struct tar_entry e;
    usize pos = 0;

    if (name[0] == '\0') {
        *out = (struct tarfs_node){NULL, 0, true};
        return 0;
    }
    while (tar_next(archive, len, &pos, &e) > 0)
        if (strcmp(e.name, name) == 0) {
            *out = e.is_dir ? (struct tarfs_node){NULL, 0, true}
                            : (struct tarfs_node){e.data, e.size, false};
            return 0;
        }
    return -ENOENT;
}

int tarfs_lookup(const u8 *archive, usize len, const char *path, struct tarfs_node *out)
{
    char name[TAR_NAME_MAX];
    int err = archive_name(path, name);

    return err ? err : find(archive, len, name, out);
}

int tarfs_readdir(const u8 *archive, usize len, const char *path, u64 index, struct dirent *out)
{
    char dir[TAR_NAME_MAX];
    struct tarfs_node node;
    struct tar_entry e;
    usize pos = 0, dlen;
    int err = archive_name(path, dir);

    if (!err)
        err = find(archive, len, dir, &node);
    if (err)
        return err;
    if (!node.is_dir)
        return -ENOTDIR;
    dlen = strlen(dir);
    while (tar_next(archive, len, &pos, &e) > 0) {
        const char *child = e.name;

        if (dlen) { /* under "dir/" */
            if (strncmp(e.name, dir, dlen) != 0 || e.name[dlen] != '/')
                continue;
            child = e.name + dlen + 1;
        }
        if (strchr(child, '/')) /* deeper down */
            continue;
        if (index-- > 0)
            continue;
        memset(out, 0, sizeof(*out));
        out->size = e.is_dir ? 0 : e.size;
        out->type = e.is_dir ? DIRENT_DIR : DIRENT_FILE;
        /* a ustar entry's own name is at most 155 characters (a prefix with no "/"): it fits */
        strlcpy(out->name, child, sizeof(out->name));
        return 1;
    }
    return 0;
}
