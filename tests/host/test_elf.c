// SPDX-License-Identifier: MIT
/*
 * ELF validation against hand-built good, malformed and hostile images.
 */
#include "test.h"

#include <lib/errno.h>
#include <proc/elf.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define USER_MIN 0x10000ull
#define USER_TOP 0x800000000000ull

struct ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

struct image {
    struct ehdr eh;
    struct phdr ph[4];
    uint8_t payload[256];
};

enum { PF_X = 1, PF_W = 2, PF_R = 4 };

/* A valid two-segment executable: text R-X at 0x400000, data RW- at 0x401000. */
static struct image good(void)
{
    struct image im;

    memset(&im, 0, sizeof(im));
    memcpy(im.eh.ident,
           "\x7f"
           "ELF\x02\x01\x01",
           7);
    im.eh.type = 2;
    im.eh.machine = 62;
    im.eh.version = 1;
    im.eh.entry = 0x400010;
    im.eh.phoff = offsetof(struct image, ph);
    im.eh.ehsize = sizeof(struct ehdr);
    im.eh.phentsize = sizeof(struct phdr);
    im.eh.phnum = 2;
    im.ph[0] =
        (struct phdr){1, PF_R | PF_X, offsetof(struct image, payload), 0x400000, 0, 64, 64, 0x1000};
    im.ph[1] = (struct phdr){
        1, PF_R | PF_W, offsetof(struct image, payload) + 64, 0x401000, 0, 32, 0x2000, 0x1000};
    return im;
}

static int parse(const struct image *im, size_t len)
{
    struct elf_image out;

    return elf_parse(im, len, USER_MIN, USER_TOP, &out);
}

TEST(elf_accepts_a_valid_executable)
{
    struct image im = good();
    struct elf_image out;

    EXPECT_EQ(elf_parse(&im, sizeof(im), USER_MIN, USER_TOP, &out), 0);
    EXPECT_EQ(out.nsegments, 2);
    EXPECT_EQ(out.entry, 0x400010);
    EXPECT_TRUE(out.segments[0].executable && !out.segments[0].writable);
    EXPECT_TRUE(out.segments[1].writable && !out.segments[1].executable);
    EXPECT_EQ(out.segments[1].memsz, 0x2000); /* .bss beyond filesz */
}

TEST(elf_rejects_wrong_identity)
{
    struct image im;

    im = good(), im.eh.ident[0] = 0;
    EXPECT_EQ(parse(&im, sizeof(im)), -ENOEXEC);
    im = good(), im.eh.ident[4] = 1; /* 32-bit */
    EXPECT_EQ(parse(&im, sizeof(im)), -ENOEXEC);
    im = good(), im.eh.machine = 3; /* i386 */
    EXPECT_EQ(parse(&im, sizeof(im)), -ENOEXEC);
    im = good(), im.eh.type = 3; /* ET_DYN */
    EXPECT_EQ(parse(&im, sizeof(im)), -ENOEXEC);
    im = good(), im.ph[1].type = 3; /* PT_INTERP: dynamically linked */
    EXPECT_EQ(parse(&im, sizeof(im)), -ENOEXEC);
    EXPECT_EQ(parse(&im, 10), -ENOEXEC); /* truncated header */
}

TEST(elf_rejects_out_of_bounds_tables_and_data)
{
    struct image im;

    im = good(), im.eh.phoff = sizeof(im) - 8; /* table runs off the end */
    EXPECT_EQ(parse(&im, sizeof(im)), -EINVAL);
    im = good(), im.eh.phoff = UINT64_MAX - 10; /* offset arithmetic would wrap */
    EXPECT_EQ(parse(&im, sizeof(im)), -EINVAL);
    im = good(), im.eh.phentsize = 40;
    EXPECT_EQ(parse(&im, sizeof(im)), -EINVAL);
    im = good(), im.ph[0].filesz = 10000; /* data past end of file */
    im.ph[0].memsz = 10000;
    EXPECT_EQ(parse(&im, sizeof(im)), -EINVAL);
    im = good(), im.ph[0].offset = UINT64_MAX;
    EXPECT_EQ(parse(&im, sizeof(im)), -EINVAL);
    im = good(), im.ph[0].filesz = 128; /* filesz > memsz */
    EXPECT_EQ(parse(&im, sizeof(im)), -EINVAL);
}

TEST(elf_rejects_segments_outside_user_space)
{
    struct image im;

    im = good(), im.ph[0].vaddr = 0; /* the NULL page */
    EXPECT_EQ(parse(&im, sizeof(im)), -EINVAL);
    im = good(), im.ph[1].vaddr = 0xFFFFFFFF80000000ull; /* kernel half */
    EXPECT_EQ(parse(&im, sizeof(im)), -EINVAL);
    im = good(), im.ph[1].vaddr = USER_TOP - 0x1000; /* crosses USER_TOP */
    EXPECT_EQ(parse(&im, sizeof(im)), -EINVAL);
    im = good(), im.ph[1].vaddr = UINT64_MAX - 16; /* vaddr + memsz wraps */
    EXPECT_EQ(parse(&im, sizeof(im)), -EINVAL);
}

TEST(elf_rejects_unsafe_layouts)
{
    struct image im;

    im = good(), im.ph[0].flags = PF_R | PF_W | PF_X; /* W+X */
    EXPECT_EQ(parse(&im, sizeof(im)), -EINVAL);
    im = good(), im.ph[1].vaddr = 0x400800; /* shares a page with text */
    EXPECT_EQ(parse(&im, sizeof(im)), -EINVAL);
    im = good(), im.eh.entry = 0x401000; /* entry in data */
    EXPECT_EQ(parse(&im, sizeof(im)), -EINVAL);
    im = good(), im.eh.entry = 0x500000; /* entry in nothing */
    EXPECT_EQ(parse(&im, sizeof(im)), -EINVAL);
}

TEST(elf_survives_random_corruption)
{
    unsigned seed = 77;

    /* Never crash or read out of bounds (ASan watches); any verdict is fine. */
    for (int iter = 0; iter < 20000; iter++) {
        struct image im = good();
        uint8_t *raw = (uint8_t *)&im;
        int flips = 1 + rand_r(&seed) % 8;

        for (int f = 0; f < flips; f++)
            raw[rand_r(&seed) % offsetof(struct image, payload)] ^=
                (uint8_t)(1u << (rand_r(&seed) % 8));
        size_t len = (size_t)(rand_r(&seed) % (int)sizeof(im)) + 1;
        uint8_t *copy = malloc(len); /* exact-size buffer: ASan flags any overread */

        memcpy(copy, &im, len);
        struct elf_image out;
        int r = elf_parse(copy, len, USER_MIN, USER_TOP, &out);

        EXPECT_TRUE(r == 0 || r == -EINVAL || r == -ENOEXEC);
        free(copy);
    }
}
