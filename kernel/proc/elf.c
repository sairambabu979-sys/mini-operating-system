// SPDX-License-Identifier: MIT
/*
 * ELF64 validation. See <proc/elf.h>.
 *
 * Headers are copied out with memcpy rather than cast in place: the input
 * is untrusted and need not be aligned.
 */
#include <lib/errno.h>
#include <lib/string.h>
#include <proc/elf.h>

#define PAGE 4096ull

struct elf64_ehdr {
    u8 ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u64 entry;
    u64 phoff;
    u64 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
};

struct elf64_phdr {
    u32 type;
    u32 flags;
    u64 offset;
    u64 vaddr;
    u64 paddr;
    u64 filesz;
    u64 memsz;
    u64 align;
};

static_assert(sizeof(struct elf64_ehdr) == 64, "Elf64_Ehdr is 64 bytes");
static_assert(sizeof(struct elf64_phdr) == 56, "Elf64_Phdr is 56 bytes");

enum {
    ET_EXEC = 2,
    EM_X86_64 = 62,
    PT_LOAD = 1,
    PT_DYNAMIC = 2,
    PT_INTERP = 3,
    PF_X = 1,
    PF_W = 2,
};

/* a + b, failing instead of wrapping. */
static bool add_ok(u64 a, u64 b, u64 *sum)
{
    return !__builtin_add_overflow(a, b, sum);
}

static int check_header(const struct elf64_ehdr *h, usize len)
{
    static const u8 magic[4] = {0x7F, 'E', 'L', 'F'};
    u64 table_size, table_end;

    if (memcmp(h->ident, magic, 4) != 0)
        return -ENOEXEC;
    if (h->ident[4] != 2 /* ELFCLASS64 */ || h->ident[5] != 1 /* little endian */ ||
        h->ident[6] != 1 /* EV_CURRENT */)
        return -ENOEXEC;
    if (h->type != ET_EXEC || h->machine != EM_X86_64 || h->version != 1)
        return -ENOEXEC;
    if (h->phentsize != sizeof(struct elf64_phdr) || h->phnum == 0 || h->phnum > ELF_MAX_PHDRS)
        return -EINVAL;
    table_size = (u64)h->phnum * sizeof(struct elf64_phdr);
    if (!add_ok(h->phoff, table_size, &table_end) || table_end > len)
        return -EINVAL;
    return 0;
}

static int check_segment(const struct elf64_phdr *ph, usize len, u64 user_min, u64 user_top)
{
    u64 file_end, mem_end;

    if (ph->filesz > ph->memsz)
        return -EINVAL;
    if (!add_ok(ph->offset, ph->filesz, &file_end) || file_end > len)
        return -EINVAL;
    if (ph->vaddr < user_min || !add_ok(ph->vaddr, ph->memsz, &mem_end) || mem_end > user_top)
        return -EINVAL;
    if ((ph->flags & PF_W) && (ph->flags & PF_X))
        return -EINVAL; /* W^X holds in user space too */
    return 0;
}

int elf_parse(const void *data, usize len, u64 user_min, u64 user_top, struct elf_image *out)
{
    struct elf64_ehdr h;
    const u8 *bytes = data;
    int err;

    if (len < sizeof(h))
        return -ENOEXEC;
    memcpy(&h, bytes, sizeof(h));
    err = check_header(&h, len);
    if (err)
        return err;

    out->entry = h.entry;
    out->nsegments = 0;

    for (u16 i = 0; i < h.phnum; i++) {
        struct elf64_phdr ph;

        memcpy(&ph, bytes + h.phoff + (u64)i * sizeof(ph), sizeof(ph));
        if (ph.type == PT_INTERP || ph.type == PT_DYNAMIC)
            return -ENOEXEC; /* static executables only */
        if (ph.type != PT_LOAD || ph.memsz == 0)
            continue;
        err = check_segment(&ph, len, user_min, user_top);
        if (err)
            return err;
        if (out->nsegments == ELF_MAX_SEGMENTS)
            return -EINVAL;
        out->segments[out->nsegments++] = (struct elf_segment){
            .vaddr = ph.vaddr,
            .memsz = ph.memsz,
            .offset = ph.offset,
            .filesz = ph.filesz,
            .writable = ph.flags & PF_W,
            .executable = ph.flags & PF_X,
        };
    }
    if (out->nsegments == 0)
        return -ENOEXEC;

    /* Segments are mapped page by page with their own permissions: no page may be shared. */
    for (u32 i = 0; i < out->nsegments; i++) {
        const struct elf_segment *a = &out->segments[i];
        u64 a_lo = a->vaddr & ~(PAGE - 1), a_hi = (a->vaddr + a->memsz + PAGE - 1) & ~(PAGE - 1);

        for (u32 j = i + 1; j < out->nsegments; j++) {
            const struct elf_segment *b = &out->segments[j];
            u64 b_lo = b->vaddr & ~(PAGE - 1),
                b_hi = (b->vaddr + b->memsz + PAGE - 1) & ~(PAGE - 1);

            if (a_lo < b_hi && b_lo < a_hi)
                return -EINVAL;
        }
    }

    /* The entry point must be inside executable memory. */
    for (u32 i = 0; i < out->nsegments; i++) {
        const struct elf_segment *s = &out->segments[i];

        if (s->executable && out->entry >= s->vaddr && out->entry < s->vaddr + s->memsz)
            return 0;
    }
    return -EINVAL;
}
