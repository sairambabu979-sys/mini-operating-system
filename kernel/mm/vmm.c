// SPDX-License-Identifier: MIT
/*
 * Virtual memory manager. See <mm/vmm.h> and docs/design/memory.md.
 */
#define pr_fmt(fmt) "vmm: " fmt

#include <arch/cpu.h>
#include <arch/cpuid.h>
#include <arch/msr.h>
#include <arch/paging.h>
#include <core/klog.h>
#include <core/panic.h>
#include <core/sections.h>
#include <lib/errno.h>
#include <lib/string.h>
#include <mm/kmalloc.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

/*
 * Page tables needed before the PMM exists come from this pool. It lives in
 * .bss (zeroed by the loader) inside the kernel image, so the boot window
 * reaches it. 32 pages covers the kernel image plus a direct map of tens of
 * GiB; running out is a clear panic, not corruption.
 */
#define EARLY_PT_PAGES 32

static u8 early_pt_pool[EARLY_PT_PAGES][PAGE_SIZE_4K] __aligned(PAGE_SIZE_4K);
static unsigned early_pt_used;

static struct address_space kernel_as = {.lock = SPINLOCK_INIT("kernel page tables")};
static bool active; /* running on our tables: tables reachable via HHDM */
static bool kernel_half_frozen;
static u64 pte_global; /* PTE_GLOBAL if the CPU supports it */

struct address_space *vmm_kernel_space(void)
{
    return &kernel_as;
}

bool vmm_is_active(void)
{
    return active;
}

/*
 * Where page-table frames are written. Early-pool tables are kernel-image
 * frames: their direct-map alias is read-only by design, so they are always
 * written through the (writable) .bss mapping instead.
 */
static u64 *table_virt(u64 phys)
{
    u64 image_start = (uptr)__kernel_start - KERNEL_VMA;
    u64 image_end = (uptr)__kernel_end - KERNEL_VMA;

    if (phys >= image_start && phys < image_end)
        return (u64 *)(uptr)(phys + KERNEL_VMA);
    return active ? phys_to_virt(phys) : phys_to_kimage(phys);
}

/* Returns a zeroed page-table page (physical address), or 0. */
static u64 alloc_table(void)
{
    if (!active) {
        if (early_pt_used == EARLY_PT_PAGES)
            panic("vmm: early page-table pool exhausted (%u pages)", EARLY_PT_PAGES);
        return (u64)(uptr)early_pt_pool[early_pt_used++] - KERNEL_VMA;
    }
    u64 phys = pmm_alloc_page();

    if (phys != PMM_ALLOC_FAILED)
        memset(phys_to_virt(phys), 0, PAGE_SIZE_4K);
    return phys;
}

static u64 leaf_bits(unsigned flags)
{
    u64 bits = PTE_PRESENT;

    if (flags & VM_WRITE)
        bits |= PTE_WRITE;
    if (flags & VM_USER)
        bits |= PTE_USER;
    if (!(flags & VM_EXEC))
        bits |= PTE_NX;
    if (flags & VM_GLOBAL)
        bits |= pte_global;
    if (flags & VM_NOCACHE)
        bits |= PTE_PCD | PTE_PWT;
    else if (flags & VM_WC)
        bits |= cpu_features()->pat ? PTE_PWT : PTE_PCD | PTE_PWT; /* PAT entry 1 */
    if (flags & VM_IOMAP)
        bits |= PTE_SW_IOMAP;
    return bits;
}

/*
 * Returns the entry for `va` at `level` (4 = PML4 ... 1 = PT), creating
 * intermediate tables on the way if `create`. NULL if absent (and not
 * created) or blocked by a huge page above `level`.
 *
 * Intermediate entries are maximally permissive (present, writable, user
 * for the user half); the leaf alone decides access.
 */
static u64 *walk(const struct address_space *as, uptr va, int level, bool create, int *err)
{
    u64 *table = table_virt(as->pml4_phys);
    static const unsigned shifts[] = {0, 12, 21, 30, 39};

    for (int lvl = 4; lvl > level; lvl--) {
        u64 *entry = &table[(va >> shifts[lvl]) & 0x1FF];

        if (!(*entry & PTE_PRESENT)) {
            if (!create) {
                *err = -ENOENT;
                return NULL;
            }
            /* The kernel half is shared by copying PML4 entries: it must not grow. */
            if (lvl == 4 && va >= KERNEL_HALF && kernel_half_frozen)
                panic("vmm: new kernel PML4 entry after freeze (va %#lx)", va);
            u64 phys = alloc_table();

            if (phys == 0) {
                *err = -ENOMEM;
                return NULL;
            }
            *entry = phys | PTE_PRESENT | PTE_WRITE | (va < KERNEL_HALF ? PTE_USER : 0);
        } else if (*entry & PTE_HUGE) {
            *err = -EEXIST;
            return NULL;
        }
        table = table_virt(*entry & PTE_ADDR);
    }
    return &table[(va >> shifts[level]) & 0x1FF];
}

static void flush(const struct address_space *as, uptr va)
{
    /* Kernel-half mappings are global and visible in every address space. */
    if (active && (va >= KERNEL_HALF || read_cr3() == as->pml4_phys))
        invlpg(va);
}

/*
 * Maps one range, using 1 GiB / 2 MiB pages where alignment allows when
 * `allow_huge` (direct map only). On -EEXIST the range may be partially
 * mapped; callers treat that as a bug.
 */
static int map_range(struct address_space *as, uptr va, u64 pa, usize size, unsigned flags,
                     bool allow_huge)
{
    u64 bits = leaf_bits(flags);
    int err = 0;

    while (size > 0) {
        u64 step = PAGE_SIZE_4K;
        int level = 1;

        if (allow_huge && cpu_features()->page_1g && IS_ALIGNED(va | pa, PAGE_SIZE_1G) &&
            size >= PAGE_SIZE_1G) {
            step = PAGE_SIZE_1G;
            level = 3;
        } else if (allow_huge && IS_ALIGNED(va | pa, PAGE_SIZE_2M) && size >= PAGE_SIZE_2M) {
            step = PAGE_SIZE_2M;
            level = 2;
        }

        u64 *entry = walk(as, va, level, true, &err);

        if (!entry)
            return err;
        if (*entry & PTE_PRESENT)
            return -EEXIST;
        *entry = pa | bits | (level > 1 ? PTE_HUGE : 0);
        flush(as, va);
        va += step;
        pa += step;
        size -= step;
    }
    return 0;
}

int vmm_map(struct address_space *as, uptr va, u64 pa, usize size, unsigned flags)
{
    if (!IS_ALIGNED(va | pa | size, PAGE_SIZE_4K) || !va_is_canonical(va))
        return -EINVAL;
    if ((flags & VM_WRITE) && (flags & VM_EXEC))
        return -EINVAL; /* W^X */

    irqflags_t irq = spin_lock_irqsave(&as->lock);
    int err = map_range(as, va, pa, size, flags, false);

    spin_unlock_irqrestore(&as->lock, irq);
    return err;
}

int vmm_unmap_page(struct address_space *as, uptr va, u64 *pa)
{
    int err = 0;
    irqflags_t irq = spin_lock_irqsave(&as->lock);
    u64 *entry = walk(as, va, 1, false, &err);

    if (entry && !(*entry & PTE_PRESENT))
        err = -ENOENT;
    if (entry && err == 0) {
        *pa = *entry & PTE_ADDR;
        *entry = 0;
        flush(as, va);
    }
    spin_unlock_irqrestore(&as->lock, irq);
    return err;
}

/*
 * Lock-free: entries are read with single aligned loads, and kernel-half
 * tables are never freed. A caller racing an unmap of the same page must
 * synchronize itself.
 */
bool vmm_translate(const struct address_space *as, uptr va, u64 *pa, u64 *pte)
{
    u64 *table = table_virt(as->pml4_phys);
    static const unsigned shifts[] = {0, 12, 21, 30, 39};

    for (int lvl = 4; lvl >= 1; lvl--) {
        u64 entry = table[(va >> shifts[lvl]) & 0x1FF];

        if (!(entry & PTE_PRESENT))
            return false;
        if (lvl == 1 || (entry & PTE_HUGE)) {
            u64 page_mask = (1ull << shifts[lvl]) - 1;

            *pa = (entry & PTE_ADDR & ~page_mask) | (va & page_mask);
            *pte = entry;
            return true;
        }
        table = table_virt(entry & PTE_ADDR);
    }
    return false;
}

struct address_space *vmm_create_user_space(void)
{
    struct address_space *as = kzalloc(sizeof(*as));

    if (!as)
        return NULL;
    as->pml4_phys = alloc_table();
    if (as->pml4_phys == 0) {
        kfree(as);
        return NULL;
    }
    as->lock = (struct spinlock)SPINLOCK_INIT("user page tables");

    const u64 *kernel_pml4 = table_virt(kernel_as.pml4_phys);
    u64 *pml4 = table_virt(as->pml4_phys);

    BUG_ON(!kernel_half_frozen);
    for (unsigned i = PT_ENTRIES / 2; i < PT_ENTRIES; i++)
        pml4[i] = kernel_pml4[i];
    return as;
}

/* Frees a user-half table at `level` (3 = PDPT ... 1 = PT) and everything below it. */
static void free_user_table(u64 phys, int level)
{
    u64 *table = phys_to_virt(phys);

    for (unsigned i = 0; i < PT_ENTRIES; i++) {
        u64 e = table[i];

        if (!(e & PTE_PRESENT))
            continue;
        BUG_ON(e & PTE_HUGE); /* user mappings are always 4 KiB */
        if (level == 1)
            pmm_free_page(e & PTE_ADDR); /* a user frame, owned by this space */
        else
            free_user_table(e & PTE_ADDR, level - 1);
    }
    pmm_free_page(phys);
}

void vmm_destroy_user_space(struct address_space *as)
{
    u64 *pml4 = phys_to_virt(as->pml4_phys);

    BUG_ON(read_cr3() == as->pml4_phys);
    for (unsigned i = 0; i < PT_ENTRIES / 2; i++) {
        if (pml4[i] & PTE_PRESENT)
            free_user_table(pml4[i] & PTE_ADDR, 3);
    }
    pmm_free_page(as->pml4_phys);
    kfree(as);
}

static void must(int err, const char *what)
{
    if (err != 0)
        panic("vmm: %s failed (%d)", what, err);
}

/* Kernel image: one mapping per section class, matching the PT_LOAD segments. */
static void map_kernel_image(void)
{
    struct {
        const char *name;
        char *start, *end;
        unsigned flags;
    } sections[] = {
        {".text", __text_start, __text_end, VM_EXEC},
        {".rodata", __rodata_start, __rodata_end, 0},
        {".data/.bss", __data_start, __data_end, VM_WRITE},
    };

    for (usize i = 0; i < ARRAY_SIZE(sections); i++) {
        uptr va = (uptr)sections[i].start;
        usize size = (usize)(sections[i].end - sections[i].start);

        must(map_range(&kernel_as, va, va - KERNEL_VMA, size, sections[i].flags | VM_GLOBAL, false),
             sections[i].name);
    }
}

/*
 * Direct map of every firmware-described RAM region (usable and ACPI).
 * Reserved ranges and holes stay unmapped: write-back mappings over MMIO
 * invite speculative accesses to devices. The kernel image's own frames
 * are mapped read-only so the alias cannot bypass W^X.
 */
static void map_direct(const struct mem_region *regions, usize n, const struct boot_info *bi)
{
    u64 kstart = ALIGN_DOWN(bi->kernel_phys_start, PAGE_SIZE_4K);
    u64 kend = ALIGN_UP(bi->kernel_phys_end, PAGE_SIZE_4K);
    u64 mapped_end = 0;
    u64 total = 0;

    for (usize i = 0; i < n; i++) {
        u32 t = regions[i].type;

        if (t != E820_USABLE && t != E820_ACPI_RECLAIMABLE && t != E820_ACPI_NVS)
            continue;
        u64 base = MAX(ALIGN_DOWN(regions[i].base, PAGE_SIZE_4K), mapped_end);
        u64 end = MIN(ALIGN_UP(regions[i].end, PAGE_SIZE_4K), HHDM_SIZE);

        if (base >= end)
            continue;

        /* Split around the kernel image: [base, kstart) RW, image R, (kend, end) RW. */
        u64 cuts[] = {base, MAX(base, MIN(kstart, end)), MAX(base, MIN(kend, end)), end};

        for (int c = 0; c < 3; c++) {
            if (cuts[c] >= cuts[c + 1])
                continue;
            unsigned flags = VM_GLOBAL | (c == 1 ? 0 : VM_WRITE);

            must(map_range(&kernel_as, HHDM_BASE + cuts[c], cuts[c], cuts[c + 1] - cuts[c], flags,
                           true),
                 "direct map");
        }
        mapped_end = end;
        total += end - base;
    }
    pr_info("direct map: %llu MiB at %#llx\n", (unsigned long long)(total >> 20),
            (unsigned long long)HHDM_BASE);
}

void vmm_init(const struct mem_region *regions, usize nregions, const struct boot_info *bi)
{
    const struct cpu_features *cpu = cpu_features();

    if (!cpu->nx)
        panic("vmm: CPU lacks NX; W^X cannot be enforced");
    pte_global = cpu->pge ? PTE_GLOBAL : 0;

    kernel_as.pml4_phys = alloc_table();
    map_kernel_image();
    map_direct(regions, nregions, bi);

    /* NXE before loading tables that use bit 63; WP so ring 0 honors R/O. */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);
    /*
     * Only entry 1 changes (WT -> WC), and no mapping uses it yet: the
     * loader's tables and these new ones never set PWT alone, so no stale
     * entry or cache line can have the old type.
     */
    if (cpu->pat)
        wrmsr(MSR_PAT, PAT_KERNEL);
    write_cr0(read_cr0() | CR0_WP);

    u64 cr4 = read_cr4();

    if (cpu->pge)
        cr4 |= CR4_PGE;
    if (cpu->smep)
        cr4 |= CR4_SMEP;
    if (cpu->smap)
        cr4 |= CR4_SMAP;
    write_cr4(cr4);

    write_cr3(kernel_as.pml4_phys);
    active = true;
    pr_info("kernel page tables active (%u early table pages)\n", early_pt_used);
}

void vmm_init_late(void)
{
    u64 *pml4 = table_virt(kernel_as.pml4_phys);
    unsigned added = 0;

    for (unsigned i = PT_ENTRIES / 2; i < PT_ENTRIES; i++) {
        if (pml4[i] & PTE_PRESENT)
            continue;
        u64 phys = alloc_table();

        if (phys == 0)
            panic("vmm: out of memory pre-allocating kernel PDPTs");
        pml4[i] = phys | PTE_PRESENT | PTE_WRITE;
        added++;
    }
    kernel_half_frozen = true;
    pr_debug("kernel half frozen (%u PDPTs pre-allocated)\n", added);
}
