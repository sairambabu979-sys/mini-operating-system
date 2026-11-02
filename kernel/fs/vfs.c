// SPDX-License-Identifier: MIT
#include <fs/vfs.h>
#include <lib/errno.h>
#include <lib/string.h>
#include <lib/tar.h>

#define VFS_MAX_NODES 256
struct node {
    char path[FS_PATH_MAX];
    const u8 *data;
    usize size;
    int parent;
    bool directory;
};
static struct node nodes[VFS_MAX_NODES];
static usize count;

static bool canonical(const char *path)
{
    if (path[0] != '/' || strnlen(path, FS_PATH_MAX) == FS_PATH_MAX)
        return false;
    if (!path[1])
        return true;
    for (const char *p = path + 1; *p;) {
        const char *start = p;

        while (*p && *p != '/')
            p++;
        usize n = (usize)(p - start);

        if (!n || (n == 1 && start[0] == '.') ||
            (n == 2 && start[0] == '.' && start[1] == '.'))
            return false;
        if (*p && !*++p)
            return false;
    }
    return true;
}

static int find(const char *path, usize n)
{
    for (usize i = 0; i < n; i++)
        if (strcmp(nodes[i].path, path) == 0)
            return (int)i;
    return -ENOENT;
}

int vfs_mount(const void *archive, usize len)
{
    struct tar_entry entry;
    usize pos = 0, n = 1;
    int r;

    count = 0;
    nodes[0] = (struct node){.path = "/", .directory = true, .parent = -1};
    if (tar_validate(archive, len) < 0)
        return -EINVAL;
    while ((r = tar_next(archive, len, &pos, &entry)) > 0) {
        usize length = strlen(entry.name);

        if (length + 2 > FS_PATH_MAX)
            return -ENAMETOOLONG;
        if (n == VFS_MAX_NODES)
            return -ENOSPC;
        struct node *node = &nodes[n];

        node->path[0] = '/';
        memcpy(node->path + 1, entry.name, length + 1);
        if (!canonical(node->path) || find(node->path, n) >= 0)
            return -EINVAL;
        node->data = entry.data;
        node->size = entry.size;
        node->directory = entry.is_dir;
        n++;
    }
    if (r < 0)
        return r;
    /* Archive order is immaterial: resolve parents only after reading every entry. */
    for (usize i = 1; i < n; i++) {
        char parent[FS_PATH_MAX];

        strlcpy(parent, nodes[i].path, sizeof(parent));
        usize slash = strlen(parent);

        while (slash && parent[slash] != '/')
            slash--;
        parent[slash ? slash : 1] = '\0';
        int p = find(parent, n);

        if (p < 0 || !nodes[p].directory)
            return -EINVAL;
        nodes[i].parent = p;
    }
    count = n;
    return 0;
}

int vfs_lookup(const char *path)
{
    return canonical(path) ? find(path, count) : -EINVAL;
}

int vfs_stat(int node, struct fs_stat *out)
{
    if (node < 0 || (usize)node >= count)
        return -EBADF;
    *out = (struct fs_stat){.size = nodes[node].directory ? 0 : nodes[node].size,
                            .type = nodes[node].directory ? FS_DIRECTORY : FS_REGULAR};
    return 0;
}

int vfs_data(int node, const u8 **data, usize *size)
{
    if (node < 0 || (usize)node >= count)
        return -EBADF;
    if (nodes[node].directory)
        return -EISDIR;
    *data = nodes[node].data;
    *size = nodes[node].size;
    return 0;
}

int vfs_readdir(int node, usize index, struct fs_dirent *out)
{
    if (node < 0 || (usize)node >= count)
        return -EBADF;
    if (!nodes[node].directory)
        return -ENOTDIR;
    for (usize i = 1; i < count; i++) {
        if (nodes[i].parent != node)
            continue;
        if (index) {
            index--;
            continue;
        }
        memset(out, 0, sizeof(*out));
        const char *name = nodes[i].path + (node ? strlen(nodes[node].path) + 1 : 1);

        strlcpy(out->name, name, sizeof(out->name));
        (void)vfs_stat((int)i, &out->stat);
        return 1;
    }
    return 0;
}
