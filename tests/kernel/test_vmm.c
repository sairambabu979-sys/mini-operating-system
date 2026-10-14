// SPDX-License-Identifier: MIT
/*
 * Virtual memory tests. Protections are verified by attempting the
 * forbidden access and checking the resulting #PF in detail.
 */
#include <arch/cpu.h>
#include <arch/cpuid.h>
#include <arch/extable.h>
#include <arch/irqflags.h>
#include <arch/msr.h>
#include <arch/paging.h>
#include <arch/setjmp.h>
#include <arch/trap.h>
#include <core/boot.h>
#include <core/ktest.h>
#include <core/panic.h>
#include <core/sections.h>
#include <lib/errno.h>
#include <lib/string.h>
#include <mm/pmm.h>
#include <mm/vmalloc.h>
#include <mm/vmm.h>

/* Attempts an 8-byte write at addr. Returns the trap record. */
static struct trap_expectation try_write(uptr addr)
{
    struct trap_expectation e;

    trap_expect(&e, VEC_PAGE_FAULT, NULL);
    __asm__ volatile("1: movq %[v], (%[p])\n2:\n" EXTABLE_ENTRY(1b, 2b)
                     :
                     : [v] "r"(0x5A5Aull), [p] "r"(addr)
                     : "memory");
    trap_expect_clear();
    return e;
}

static struct trap_expectation try_read(uptr addr)
{
    struct trap_expectation e;
    u64 v = 0;

    trap_expect(&e, VEC_PAGE_FAULT, NULL);
    __asm__ volatile("1: movq (%[p]), %[v]\n2:\n" EXTABLE_ENTRY(1b, 2b)
                     : [v] "+r"(v)
                     : [p] "r"(addr)
                     : "memory");
    trap_expect_clear();
    return e;
}

/* Calls code at addr; a fault there (instruction fetch) resumes via longjmp. */
static struct trap_expectation try_execute(uptr addr)
{
    struct kjmp_buf jb;
    struct trap_expectation e;

    trap_expect(&e, VEC_PAGE_FAULT, &jb);
    if (ksetjmp(&jb) == 0)
        ((void (*)(void))addr)();
    trap_expect_clear();
    return e;
}

static int __attribute__((noinline)) answer(void)
{
    return 42;
}

static const u64 rodata_word = 0x1122334455667788ull;
static u64 data_word = 1;
static u8 data_code[16] = {0xC3}; /* ret */

KTEST(cpu_protection_features_are_enabled)
{
    const struct cpu_features *cpu = cpu_features();

    KEXPECT(read_cr0() & CR0_WP);
    KEXPECT(rdmsr(MSR_EFER) & EFER_NXE);
    KEXPECT_EQ(!!(read_cr4() & CR4_SMEP), cpu->smep);
    KEXPECT_EQ(!!(read_cr4() & CR4_SMAP), cpu->smap);
    KEXPECT_EQ(read_cr3(), vmm_kernel_space()->pml4_phys);
}

KTEST(kernel_text_is_read_only)
{
    struct trap_expectation e = try_write((uptr)&answer);

    KEXPECT(e.hit);
    KEXPECT_EQ(e.error_code, PF_PRESENT | PF_WRITE);
    KEXPECT_EQ(e.cr2, (uptr)&answer);
    KEXPECT_EQ(answer(), 42); /* and still intact */
}

KTEST(kernel_rodata_is_read_only)
{
    struct trap_expectation e = try_write((uptr)&rodata_word);

    KEXPECT(e.hit);
    KEXPECT_EQ(e.error_code, PF_PRESENT | PF_WRITE);
    KEXPECT_EQ(rodata_word, 0x1122334455667788ull);
}

KTEST(kernel_data_is_not_executable)
{
    struct trap_expectation e = try_execute((uptr)data_code);

    KEXPECT(e.hit);
    KEXPECT_EQ(e.error_code, PF_PRESENT | PF_INSTR);
    KEXPECT_EQ(e.cr2, (uptr)data_code);
}

KTEST(direct_map_is_not_executable)
{
    uptr alias = (uptr)phys_to_virt((uptr)&answer - KERNEL_VMA);
    struct trap_expectation e = try_execute(alias);

    KEXPECT(e.hit);
    KEXPECT_EQ(e.error_code, PF_PRESENT | PF_INSTR);
}

KTEST(direct_map_alias_of_kernel_image_is_read_only)
{
    /* Writing .data through the kernel image is fine; through the alias it is not. */
    uptr alias = (uptr)phys_to_virt((uptr)&data_word - KERNEL_VMA);
    struct trap_expectation e = try_write(alias);

    KEXPECT(e.hit);
    KEXPECT_EQ(e.error_code, PF_PRESENT | PF_WRITE);
    data_word = 2;
    KEXPECT_EQ(*(volatile u64 *)alias, 2); /* same frame, readable */
}

KTEST(null_and_old_identity_map_are_unmapped)
{
    struct trap_expectation e = try_read(0);

    KEXPECT(e.hit);
    KEXPECT_EQ(e.cr2, 0);
    KEXPECT_EQ(e.error_code, 0); /* not present */
    e = try_read(0x100000);      /* the bootloader's identity map is gone */
    KEXPECT(e.hit);
    KEXPECT_EQ(e.error_code, 0);
}

KTEST(translate_reports_section_permissions)
{
    u64 pa, pte;
    struct address_space *as = vmm_kernel_space();

    KEXPECT(vmm_translate(as, (uptr)&answer, &pa, &pte));
    KEXPECT_EQ(pa, (uptr)&answer - KERNEL_VMA);
    KEXPECT(!(pte & PTE_WRITE) && !(pte & PTE_NX));
    KEXPECT(vmm_translate(as, (uptr)&rodata_word, &pa, &pte));
    KEXPECT(!(pte & PTE_WRITE) && (pte & PTE_NX));
    KEXPECT(vmm_translate(as, (uptr)&data_word, &pa, &pte));
    KEXPECT((pte & PTE_WRITE) && (pte & PTE_NX));
    KEXPECT(!vmm_translate(as, 0, &pa, &pte));
}

KTEST(kernel_half_is_fully_preallocated)
{
    const u64 *pml4 = phys_to_virt(read_cr3());

    for (unsigned i = PT_ENTRIES / 2; i < PT_ENTRIES; i++)
        KEXPECT(pml4[i] & PTE_PRESENT);
}

KTEST(vmm_map_unmap_roundtrip)
{
    struct address_space *as = vmm_kernel_space();
    uptr va = VMALLOC_BASE + (256ull << 30); /* far from vmalloc's bump pointer */
    u64 frame = pmm_alloc_page(), pa;

    KEXPECT(frame != PMM_ALLOC_FAILED);
    KEXPECT_EQ(vmm_map(as, va, frame, PAGE_SIZE_4K, VM_WRITE | VM_EXEC), -EINVAL); /* W^X */
    KEXPECT_EQ(vmm_map(as, va + 1, frame, PAGE_SIZE_4K, VM_WRITE), -EINVAL);
    KEXPECT_EQ(vmm_map(as, va, frame, PAGE_SIZE_4K, VM_WRITE), 0);
    KEXPECT_EQ(vmm_map(as, va, frame, PAGE_SIZE_4K, VM_WRITE), -EEXIST);

    *(volatile u64 *)va = 0xFEEDFACE;
    KEXPECT_EQ(*(volatile u64 *)phys_to_virt(frame), 0xFEEDFACE); /* same frame */

    KEXPECT_EQ(vmm_unmap_page(as, va, &pa), 0);
    KEXPECT_EQ(pa, frame);
    KEXPECT(try_read(va).hit); /* TLB was flushed */
    KEXPECT_EQ(vmm_unmap_page(as, va, &pa), -ENOENT);
    pmm_free_page(frame);
}

KTEST(vmalloc_areas_have_guard_pages)
{
    u8 *p = vmalloc(4 * PAGE_SIZE_4K);

    KEXPECT(p != NULL);
    for (usize i = 0; i < 4 * PAGE_SIZE_4K; i++)
        KEXPECT_EQ(p[i], 0); /* zeroed */
    p[0] = 1;
    p[4 * PAGE_SIZE_4K - 1] = 1;

    struct trap_expectation e = try_read((uptr)p - 8);

    KEXPECT(e.hit);
    KEXPECT_EQ(e.cr2, (uptr)p - 8);
    vfree(p);
    KEXPECT(try_read((uptr)p).hit); /* freed VA is quarantined, not reused at once: it faults */
}

/* Frees `n` fresh one-page areas: n newer entries in vmalloc's reuse ring. */
static void free_n_pages(int n)
{
    for (int i = 0; i < n; i++)
        vfree(vmalloc(PAGE_SIZE_4K));
}

/*
 * A freed range is reused, by the same size only, once VMALLOC_QUARANTINE
 * newer ranges were freed: until then a stale pointer into it faults
 * rather than aliasing a new owner. Sizes no other code allocates keep
 * this deterministic.
 */
KTEST(vmalloc_reuses_a_range_only_after_the_quarantine)
{
    u8 *a = vmalloc(7 * PAGE_SIZE_4K);

    KEXPECT(a != NULL);
    vfree(a);
    free_n_pages(VMALLOC_QUARANTINE - 1);

    u8 *b = vmalloc(7 * PAGE_SIZE_4K); /* `a` has one newer free too few */

    KEXPECT(b != NULL && b != a);
    vfree(b); /* the sixteenth */

    u8 *c = vmalloc(7 * PAGE_SIZE_4K);

    KEXPECT(c == a);
    vfree(c);
}

/*
 * Reuse can take an entry from the middle of the ring (the oldest of its
 * size, not the oldest overall). It must leave the ring then: a range
 * still listed would go to a second owner while the first uses it.
 */
KTEST(vmalloc_reuse_takes_a_range_out_of_the_ring)
{
    u8 *x = vmalloc(5 * PAGE_SIZE_4K), *y = vmalloc(6 * PAGE_SIZE_4K);

    KEXPECT(x != NULL && y != NULL);
    vfree(x);
    vfree(y);
    free_n_pages(VMALLOC_QUARANTINE);

    u8 *y2 = vmalloc(6 * PAGE_SIZE_4K); /* behind `x` in the ring */
    u8 *x2 = vmalloc(5 * PAGE_SIZE_4K);
    u8 *y3 = vmalloc(6 * PAGE_SIZE_4K);

    KEXPECT(y2 == y);
    KEXPECT(x2 == x);
    KEXPECT(y3 != NULL && y3 != y); /* not handed out twice */
    vfree(y3);
    vfree(x2);
    vfree(y2);
}

KTEST(vfree_rejects_foreign_pointers)
{
    struct kjmp_buf jb;
    struct panic_expectation e;
    u8 *p = vmalloc(PAGE_SIZE_4K * 2);

    panic_expect(&e, &jb);
    if (ksetjmp(&jb) == 0)
        vfree(p + PAGE_SIZE_4K); /* interior page */
    panic_expect_clear();
    KEXPECT(e.hit);
    vfree(p);

    /* Shaped like a vmalloc area (guard page, then mappings), but device memory. */
    void *dev = iomap(0xFEC00000ull, PAGE_SIZE_4K, VM_WRITE | VM_NOCACHE);

    KEXPECT(dev != NULL);
    panic_expect(&e, &jb);
    if (ksetjmp(&jb) == 0)
        vfree(dev);
    panic_expect_clear();
    KEXPECT(e.hit);
}

/*
 * Recurses until the stack runs out. volatile keeps every frame real; the
 * never-true exit (the limit is far beyond any stack) keeps the compiler
 * from proving the recursion infinite.
 */
static volatile uptr recursion_limit = ~(uptr)0;

static void __attribute__((noinline)) recurse_forever(void *depth)
{
    volatile u8 frame[256];

    frame[0] = (u8)(uptr)depth;
    if ((uptr)depth == recursion_limit)
        return;
    recurse_forever((void *)((uptr)depth + 1));
    frame[1] = frame[0]; /* not a tail call */
}

/*
 * The scenario guard pages exist for: a kernel stack overflow. The push
 * into the guard faults (#PF); delivering that #PF needs the same broken
 * stack and faults again, so the CPU raises #DF, which runs on its IST
 * stack. Without guard + IST this is a silent corruption or a triple fault.
 */
KTEST(iomap_maps_device_registers_uncached)
{
    u64 phys = 0xFEC00010ull; /* inside the I/O APIC page, a hole in every QEMU memory map */
    volatile u32 *p = iomap(phys, 8, VM_WRITE | VM_NOCACHE);
    u64 pa, pte;

    KEXPECT(p != NULL);
    KEXPECT_EQ((uptr)p & (PAGE_SIZE_4K - 1), 0x10); /* the offset survives */
    KEXPECT(vmm_translate(vmm_kernel_space(), (uptr)p, &pa, &pte));
    KEXPECT_EQ(pa, phys);
    KEXPECT((pte & PTE_PCD) && (pte & PTE_PWT)); /* strong uncacheable */
    KEXPECT((pte & PTE_NX) && (pte & PTE_WRITE) && (pte & PTE_GLOBAL) && !(pte & PTE_USER));
    KEXPECT(try_read(ALIGN_DOWN((uptr)p, PAGE_SIZE_4K) - 8).hit); /* guard page */
}

KTEST(iomap_refuses_bad_requests_and_uncached_ram)
{
    u64 kernel_phys = boot_info_get()->kernel_phys_start;

    /* RAM is cached in the direct map; an uncached alias would be undefined. */
    KEXPECT(iomap(kernel_phys, 16, VM_NOCACHE) == NULL);
    KEXPECT(iomap(kernel_phys, 16, VM_EXEC) == NULL);
    KEXPECT(iomap(kernel_phys, 0, 0) == NULL);
    KEXPECT(iomap(1ull << 52, 16, 0) == NULL); /* beyond any physical address */
    KEXPECT(iomap(~0ull - 8, 16, 0) == NULL);  /* wraps */

    /* A cached, read-only view of the same frame is fine: same memory type. */
    const u32 *view = iomap(kernel_phys, 16, 0);

    KEXPECT(view != NULL);
    u32 expected;

    memcpy(&expected, __kernel_start, sizeof(expected));
    KEXPECT_EQ(*view, expected);
    KEXPECT(try_write((uptr)view).hit);
}

KTEST(kernel_stack_overflow_hits_guard_page)
{
    enum { STACK = 4 * PAGE_SIZE_4K };
    u8 *stack = vmalloc(STACK);
    struct kjmp_buf jb;
    struct trap_expectation e;

    KEXPECT(stack != NULL);
    irqflags_t flags = arch_irq_save();

    trap_expect(&e, VEC_DOUBLE_FAULT, &jb);
    if (ksetjmp(&jb) == 0)
        call_on_stack(recurse_forever, NULL, stack + STACK);
    trap_expect_clear();
    arch_irq_restore(flags);
    KEXPECT(e.hit);
    KEXPECT(e.cr2 >= (uptr)stack - PAGE_SIZE_4K && e.cr2 < (uptr)stack); /* in the guard */
    vfree(stack);
}
