// SPDX-License-Identifier: MIT
/*
 * The initrd both loaders hand over: there, valid, in place, and holding
 * every user program byte for byte as the build packed it. The kernel
 * also embeds those programs, so the copies can be compared exactly:
 * any frame of the initrd handed out and written since boot shows here.
 */
#include <arch/paging.h>
#include <core/boot.h>
#include <core/ktest.h>
#include <fs/initrd.h>
#include <lib/string.h>
#include <lib/tar.h>
#include <proc/process.h>

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

KTEST(initrd_holds_every_program_byte_for_byte)
{
    usize len, pos = 0, programs = 0;
    const u8 *rd = initrd_get(&len);
    struct tar_entry e;
    struct user_image img;

    KEXPECT(rd != NULL);
    if (!rd)
        return;
    while (tar_next(rd, len, &pos, &e) > 0) {
        if (e.is_dir || strncmp(e.name, "bin/", 4) != 0)
            continue;
        bool found = user_image_find(e.name + 4, &img);

        KEXPECT(found);
        if (found)
            KEXPECT(img.size == e.size && memcmp(img.data, e.data, e.size) == 0);
        programs++;
    }
    KEXPECT(programs > 10);
    KEXPECT(tar_find(rd, len, "bin/sh", &e) && user_image_find("sh", &img));
}
