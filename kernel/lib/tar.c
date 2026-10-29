// SPDX-License-Identifier: MIT
/*
 * ustar reader. See <lib/tar.h>.
 */
#include <lib/errno.h>
#include <lib/string.h>
#include <lib/tar.h>

/* Header fields: offset and length within the 512-byte block. */
#define H_NAME     0
#define H_NAME_LEN 100
#define H_SIZE     124
#define H_SIZE_LEN 12
#define H_CHKSUM   148
#define H_CHK_LEN  8
#define H_TYPE     156
#define H_MAGIC    257
#define H_PREFIX   345
#define H_PFX_LEN  155

static bool all_zero(const u8 *b)
{
    for (int i = 0; i < TAR_BLOCK; i++)
        if (b[i])
            return false;
    return true;
}

/*
 * An octal field: optional leading spaces, digits, then a NUL or space
 * (or the field's end). Base-256 (a high bit set) and anything else is
 * rejected. The fields are at most 12 digits, 36 bits, so the value
 * cannot overflow.
 */
static bool parse_octal(const u8 *f, usize len, u64 *out)
{
    usize i = 0;
    u64 v = 0;
    bool any = false;

    while (i < len && f[i] == ' ')
        i++;
    for (; i < len && f[i] >= '0' && f[i] <= '7'; i++) {
        v = v << 3 | (u64)(f[i] - '0');
        any = true;
    }
    for (; i < len; i++)
        if (f[i] != '\0' && f[i] != ' ')
            return false;
    *out = v;
    return any;
}

/* The header checksum: every byte summed, the checksum field counted as spaces. */
static bool checksum_ok(const u8 *h)
{
    u64 want, sum = 0;

    if (!parse_octal(h + H_CHKSUM, H_CHK_LEN, &want))
        return false;
    for (int i = 0; i < TAR_BLOCK; i++)
        sum += (i >= H_CHKSUM && i < H_CHKSUM + H_CHK_LEN) ? ' ' : h[i];
    return sum == want;
}

/* Copies a field that may fill its width without a NUL; returns its length. */
static usize copy_field(char *dst, const u8 *f, usize len)
{
    usize n = strnlen((const char *)f, len);

    memcpy(dst, f, n);
    return n;
}

static int build_name(const u8 *h, char *name)
{
    usize n = copy_field(name, h + H_PREFIX, H_PFX_LEN);

    if (n)
        name[n++] = '/';
    n += copy_field(name + n, h + H_NAME, H_NAME_LEN);
    name[n] = '\0';
    char *start = name;

    while (start[0] == '.' && start[1] == '/') /* "./bin/x" is "bin/x" */
        start += 2;
    n = strlen(start);
    memmove(name, start, n + 1);
    while (n && name[n - 1] == '/') /* "bin/" is "bin" */
        name[--n] = '\0';
    return n ? 0 : -EINVAL;
}

int tar_next(const u8 *archive, usize len, usize *pos, struct tar_entry *e)
{
    for (;;) {
        if (*pos == len)
            return 0;
        if (*pos > len || len - *pos < TAR_BLOCK)
            return -EINVAL;
        const u8 *h = archive + *pos;

        if (all_zero(h))
            return 0;
        if (memcmp(h + H_MAGIC, "ustar", 5) != 0 || !checksum_ok(h))
            return -EINVAL;
        u64 size;

        if (!parse_octal(h + H_SIZE, H_SIZE_LEN, &size))
            return -EINVAL;
        usize body = len - *pos - TAR_BLOCK;

        if (size > body)
            return -EINVAL;
        u64 padded = (size + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK;

        /* the last entry may end without its padding block, at the buffer's end */
        usize next = *pos + TAR_BLOCK + (usize)(padded <= body ? padded : size);
        u8 type = h[H_TYPE];

        if (type != '0' && type != '\0' && type != '5') {
            *pos = next; /* not a file or a directory: skip it */
            continue;
        }
        if (build_name(h, e->name) != 0)
            return -EINVAL;
        e->data = h + TAR_BLOCK;
        e->size = (usize)size;
        e->is_dir = type == '5';
        *pos = next;
        return 1;
    }
}

int tar_validate(const u8 *archive, usize len)
{
    struct tar_entry e;
    usize pos = 0;
    int n = 0, r;

    while ((r = tar_next(archive, len, &pos, &e)) > 0)
        n++;
    return r < 0 ? r : n;
}

bool tar_find(const u8 *archive, usize len, const char *name, struct tar_entry *e)
{
    usize pos = 0;

    while (tar_next(archive, len, &pos, e) > 0)
        if (!e->is_dir && strcmp(e->name, name) == 0)
            return true;
    return false;
}
