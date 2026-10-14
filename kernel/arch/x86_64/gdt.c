// SPDX-License-Identifier: MIT
/*
 * Global Descriptor Table and Task State Segment.
 *
 * In long mode segmentation is mostly vestigial: base and limit are ignored
 * for CS/DS/ES/SS. What still matters is the privilege level, the L bit on
 * code segments, and the TSS, which supplies the ring-0 stack and the IST
 * stacks for exceptions that must never run on a possibly-broken stack.
 */
#include <arch/segments.h>
#include <lib/string.h>

/* Access byte bits. */
#define DESC_PRESENT   (1u << 7)
#define DESC_DPL(n)    ((u32)(n) << 5)
#define DESC_CODE_DATA (1u << 4) /* S: code/data rather than system */
#define DESC_EXEC      (1u << 3)
#define DESC_RW        (1u << 1) /* readable code / writable data */
#define DESC_TSS_AVAIL 0x9u      /* system type: 64-bit TSS (available) */

/* Flags nibble. */
#define FLAG_GRAN_4K (1u << 3)
#define FLAG_DB32    (1u << 2)
#define FLAG_LONG    (1u << 1)

static u64 encode_desc(u32 base, u32 limit, u32 access, u32 flags)
{
    return ((u64)(limit & 0xFFFF)) | ((u64)(base & 0xFFFFFF) << 16) | ((u64)(access & 0xFF) << 40) |
           ((u64)((limit >> 16) & 0xF) << 48) | ((u64)(flags & 0xF) << 52) |
           ((u64)((base >> 24) & 0xFF) << 56);
}

static u64 gdt[GDT_ENTRIES] __aligned(16);
static struct tss tss __aligned(16);
static u8 ist_stacks[IST_COUNT][IST_STACK_SIZE] __aligned(16);

struct descriptor_ptr {
    u16 limit;
    u64 base;
} __packed;

/* Reloads CS with a far return, then the data segment registers. */
static void load_segments(u16 code, u16 data)
{
    __asm__ volatile("pushq %q0\n\t"
                     "leaq 1f(%%rip), %%rax\n\t"
                     "pushq %%rax\n\t"
                     "lretq\n"
                     "1:\n\t"
                     "movw %w1, %%ds\n\t"
                     "movw %w1, %%es\n\t"
                     "movw %w1, %%ss\n\t"
                     "xorl %%eax, %%eax\n\t"
                     "movw %%ax, %%fs\n\t"
                     "movw %%ax, %%gs\n\t"
                     :
                     : "r"((u64)code), "r"((u32)data)
                     : "rax", "memory");
}

void gdt_init(void)
{
    const u32 code = DESC_PRESENT | DESC_CODE_DATA | DESC_EXEC | DESC_RW;
    const u32 data = DESC_PRESENT | DESC_CODE_DATA | DESC_RW;

    gdt[GDT_NULL / 8] = 0;
    gdt[GDT_KERNEL_CODE / 8] =
        encode_desc(0, 0xFFFFF, code | DESC_DPL(0), FLAG_GRAN_4K | FLAG_LONG);
    gdt[GDT_KERNEL_DATA / 8] =
        encode_desc(0, 0xFFFFF, data | DESC_DPL(0), FLAG_GRAN_4K | FLAG_DB32);
    gdt[GDT_USER_CODE32 / 8] =
        encode_desc(0, 0xFFFFF, code | DESC_DPL(3), FLAG_GRAN_4K | FLAG_DB32);
    gdt[GDT_USER_DATA / 8] = encode_desc(0, 0xFFFFF, data | DESC_DPL(3), FLAG_GRAN_4K | FLAG_DB32);
    gdt[GDT_USER_CODE / 8] = encode_desc(0, 0xFFFFF, code | DESC_DPL(3), FLAG_GRAN_4K | FLAG_LONG);

    memset(&tss, 0, sizeof(tss));
    for (int i = 0; i < IST_COUNT; i++)
        tss.ist[i] = (u64)(uptr)&ist_stacks[i][IST_STACK_SIZE];
    tss.iomap_base = sizeof(tss);

    /* A 64-bit TSS descriptor spans two GDT slots; the second holds base[63:32]. */
    uptr tss_base = (uptr)&tss;

    gdt[GDT_TSS / 8] =
        encode_desc((u32)tss_base, sizeof(tss) - 1, DESC_PRESENT | DESC_TSS_AVAIL, 0);
    gdt[GDT_TSS / 8 + 1] = tss_base >> 32;

    struct descriptor_ptr gdtr = {.limit = sizeof(gdt) - 1, .base = (u64)(uptr)gdt};

    __asm__ volatile("lgdt %0" : : "m"(gdtr) : "memory");
    load_segments(GDT_KERNEL_CODE, GDT_KERNEL_DATA);
    __asm__ volatile("ltr %w0" : : "r"((u16)GDT_TSS) : "memory");
}

void tss_set_kernel_stack(u64 rsp0)
{
    tss.rsp[0] = rsp0;
}

uptr ist_stack_top(enum ist_slot slot)
{
    if (slot < 1 || slot > IST_COUNT)
        return 0;
    return (uptr)&ist_stacks[slot - 1][IST_STACK_SIZE];
}
