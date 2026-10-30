// SPDX-License-Identifier: MIT
/*
 * The initrd both loaders hand over: there, valid, in place, holding every
 * user program the build made (where the kernel runs them from), and every
 * byte of every file as the build packed it, against the hashes mkinitrd
 * wrote to etc/manifest. That its frames are never handed out is
 * test_pmm.c's drain; the manifest catches anything that wrote over them.
 */
#include <arch/paging.h>
#include <core/boot.h>
#include <core/ktest.h>
#include <fs/initrd.h>
#include <lib/string.h>
#include <lib/tar.h>
#include <proc/elf.h>
#include <proc/process.h>
#include <proc/uaccess.h>
#include <user_programs.h> /* generated: USER_PROGRAM_NAMES */

KTEST(initrd_is_handed_over_and_valid)
{
    const struct boot_info *bi = boot_info_get();
    usize len;
    const u8 *rd = initrd_get(&len);

    KEXPECT(rd != NULL);
    KEXPECT(bi->initrd_size > 0 && len == bi->initrd_size);
    KEXPECT(len % TAR_BLOCK ==
            0); /* the build's archive is whole blocks: a short hand-over shows */
    KEXPECT(IS_ALIGNED(bi->initrd_phys, PAGE_SIZE_4K));
    KEXPECT(rd == phys_to_virt(bi->initrd_phys));
    KEXPECT(rd && tar_validate(rd, len) > 0);
}

/* Every program the build made is bin/NAME, a loadable ELF, and bin/ holds nothing else. */
KTEST(initrd_holds_every_program)
{
    static const char *const names[] = {USER_PROGRAM_NAMES};
    usize len, pos = 0, in_bin = 0;
    const u8 *rd = initrd_get(&len);
    struct tar_entry e;
    struct user_image img;
    struct elf_image elf;

    for (usize i = 0; i < ARRAY_SIZE(names); i++) {
        bool found = user_image_find(names[i], &img);

        KEXPECT(found);
        if (found)
            KEXPECT_EQ(elf_parse(img.data, img.size, USER_MIN, USER_TOP, &elf), 0);
    }
    while (rd && tar_next(rd, len, &pos, &e) > 0)
        if (!e.is_dir && strncmp(e.name, "bin/", 4) == 0)
            in_bin++;
    KEXPECT_EQ(in_bin, ARRAY_SIZE(names));
}

/*
 * A name is looked up under bin/ only. A name too long for a task is
 * refused before it is copied: the long one here, unchecked, would run far
 * past the path buffer and the stack canary.
 */
KTEST(initrd_program_names_stay_in_bin)
{
    static const char *const refused[] = {
        "",
        "bin",
        "bin/hello",
        "../bin/hello",
        "./hello",
        "hello/",
        "no-such-program",
        "hello0123456789a", /* 16 characters: one too many */
        "hello-0123456789-0123456789-0123456789-0123456789-0123456789",
    };
    struct user_image img;
    usize len;
    const u8 *rd = initrd_get(&len);

    KEXPECT(user_image_find("hello", &img));
    KEXPECT(rd && img.data > rd && img.data + img.size <= rd + len); /* read in place */
    for (usize i = 0; i < ARRAY_SIZE(refused); i++)
        KEXPECT(!user_image_find(refused[i], &img));
}

static u64 fnv1a64(const u8 *p, usize n)
{
    u64 h = 0xCBF29CE484222325ull;

    for (usize i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

/* Steps past one space at *p; false if there is none. */
static bool skip_space(const char **p, const char *end)
{
    if (*p == end || **p != ' ')
        return false;
    (*p)++;
    return true;
}

/* One manifest line, "HASH SIZE PATH" without its "\n"; false if malformed. */
static bool parse_manifest_line(const char *p, const char *end, u64 *hash, u64 *size,
                                char path[TAR_NAME_MAX])
{
    *hash = 0;
    *size = 0;
    for (int i = 0; i < 16; i++, p++) {
        if (p == end)
            return false;
        char c = *p;
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;

        if (v < 0)
            return false;
        *hash = *hash << 4 | (u64)v;
    }
    if (!skip_space(&p, end) || p == end || *p < '0' || *p > '9')
        return false;
    while (p < end && *p >= '0' && *p <= '9')
        *size = *size * 10 + (u64)(*p++ - '0');
    if (!skip_space(&p, end) || p == end || (usize)(end - p) >= TAR_NAME_MAX)
        return false;
    memcpy(path, p, (usize)(end - p));
    path[end - p] = '\0';
    return true;
}

/* Every file's size and hash match the manifest mkinitrd wrote, and every file is listed. */
KTEST(initrd_matches_its_manifest)
{
    usize len, pos = 0, listed = 0, files = 0;
    const u8 *rd = initrd_get(&len);
    struct tar_entry m, e;
    bool have = rd && tar_find(rd, len, "etc/manifest", &m);

    KEXPECT(have);
    if (!have)
        return;
    const char *p = (const char *)m.data, *end = p + m.size;

    while (p < end) {
        const char *nl = p;
        u64 hash, size;
        char path[TAR_NAME_MAX];

        while (nl < end && *nl != '\n')
            nl++;
        bool ok = nl < end && parse_manifest_line(p, nl, &hash, &size, path);

        KEXPECT(ok);
        if (!ok)
            return;
        bool found = tar_find(rd, len, path, &e);

        KEXPECT(found);
        if (found) {
            KEXPECT_EQ(e.size, size);
            KEXPECT_EQ(fnv1a64(e.data, e.size), hash);
        }
        listed++;
        p = nl + 1;
    }
    while (tar_next(rd, len, &pos, &e) > 0)
        if (!e.is_dir && strcmp(e.name, "etc/manifest") != 0)
            files++;
    KEXPECT(listed > 0);
    KEXPECT_EQ(listed, files); /* every file is listed */
}
