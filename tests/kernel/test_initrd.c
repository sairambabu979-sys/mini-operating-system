// SPDX-License-Identifier: MIT
/*
 * The initrd both loaders hand over: there, valid, in place, and holding
 * every user program the build made, which is where the kernel now runs
 * them from. That its frames are never handed out is test_pmm.c's drain.
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

/* A name is looked up under bin/ only; a name too long for a task is refused, not truncated. */
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
    };
    struct user_image img;

    KEXPECT(user_image_find("hello", &img) && strcmp(img.name, "hello") == 0);
    for (usize i = 0; i < ARRAY_SIZE(refused); i++)
        KEXPECT(!user_image_find(refused[i], &img));
}
