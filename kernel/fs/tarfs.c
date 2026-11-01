// SPDX-License-Identifier: MIT
/*
 * Paths over a ustar archive. See <fs/tarfs.h>.
 */
#include <fs/tarfs.h>
#include <lib/errno.h>
#include <lib/string.h>
#include <lib/tar.h>

/*
 * The archive name for `path` ("" for the root) into name[TAR_NAME_MAX],
 * and whether it ended in "/"; 0 or an errno.
 */
static int archive_name(const char *path, char *name, bool *slash)
{
    usize n;

    if (path[0] != '/')
        return -EINVAL;
    path++;
    n = strnlen(path, TAR_NAME_MAX);
    if (n == TAR_NAME_MAX)
        return -ENAMETOOLONG;
    memcpy(name, path, n + 1);
    *slash = n > 0 && name[n - 1] == '/';
    if (*slash) /* "/bin/" is "/bin" */
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

/* The first entry named exactly `name` (not the root), or -ENOENT. */
static int find_entry(const u8 *archive, usize len, const char *name, struct tarfs_node *out)
{
    struct tar_entry e;
    usize pos = 0;

    while (tar_next(archive, len, &pos, &e) > 0)
        if (strcmp(e.name, name) == 0) {
            *out = e.is_dir ? (struct tarfs_node){NULL, 0, true}
                            : (struct tarfs_node){e.data, e.size, false};
            return 0;
        }
    return -ENOENT;
}

/*
 * Resolves an archive name ("" is the root): each of its parents must be
 * a directory (-ENOENT if one is missing, -ENOTDIR if one is a file), so
 * a lookup finds only what a listing from the root would reach.
 */
static int resolve(const u8 *archive, usize len, char *name, struct tarfs_node *out)
{
    struct tarfs_node parent;

    if (name[0] == '\0') {
        *out = (struct tarfs_node){NULL, 0, true};
        return 0;
    }
    for (char *slash = strchr(name, '/'); slash; slash = strchr(slash + 1, '/')) {
        int err;

        *slash = '\0';
        err = find_entry(archive, len, name, &parent);
        *slash = '/';
        if (err)
            return err;
        if (!parent.is_dir)
            return -ENOTDIR;
    }
    return find_entry(archive, len, name, out);
}

int tarfs_lookup(const u8 *archive, usize len, const char *path, struct tarfs_node *out)
{
    char name[TAR_NAME_MAX];
    bool slash;
    int err = archive_name(path, name, &slash);

    if (!err)
        err = resolve(archive, len, name, out);
    if (!err && slash && !out->is_dir)
        err = -ENOTDIR; /* "/etc/motd/" */
    return err;
}

/* True if an entry before `end` is also named `name`: only the first of a name is listed. */
static bool seen_before(const u8 *archive, usize len, usize end, const char *name)
{
    struct tar_entry e;
    usize pos = 0;

    while (pos < end && tar_next(archive, len, &pos, &e) > 0)
        if (pos <= end && strcmp(e.name, name) == 0)
            return true;
    return false;
}

int tarfs_readdir(const u8 *archive, usize len, const char *path, u64 index, struct dirent *out)
{
    char dir[TAR_NAME_MAX];
    struct tarfs_node node;
    struct tar_entry e;
    usize pos = 0, start, dlen;
    bool slash;
    int err = archive_name(path, dir, &slash);

    if (!err)
        err = resolve(archive, len, dir, &node);
    if (err)
        return err;
    if (!node.is_dir)
        return -ENOTDIR;
    dlen = strlen(dir);
    for (start = pos; tar_next(archive, len, &pos, &e) > 0; start = pos) {
        const char *child = e.name;

        if (dlen) { /* under "dir/" */
            if (strncmp(e.name, dir, dlen) != 0 || e.name[dlen] != '/')
                continue;
            child = e.name + dlen + 1;
        }
        if (strchr(child, '/') || seen_before(archive, len, start, e.name))
            continue; /* deeper down, or a name listed already */
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
