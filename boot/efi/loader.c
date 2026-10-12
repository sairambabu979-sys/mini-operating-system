// SPDX-License-Identifier: MIT
/*
 * UEFI loader: BOOTX64.EFI.
 *
 * Gives the kernel exactly the hand-off the BIOS loader does (struct
 * boot_info at BOOT_INFO_PHYS, kernel at its link address, identity +
 * higher-half page tables, interrupts off), so one kernel binary, and one
 * test suite, covers both firmwares. See docs/design/boot.md.
 *
 *   1. read \minios\kernel.elf (and the optional \minios\cmdline) from the
 *      volume this loader was started from;
 *   2. claim the kernel's physical range and the boot_info page from the
 *      firmware (AllocateAddress), so firmware-owned memory is never
 *      overwritten: if it is taken, fail with a clear message;
 *   3. copy and zero the kernel's segments while boot services still work;
 *   4. build page tables (0-4 GiB identity, 0-1 GiB at -2 GiB);
 *   5. fetch the final memory map, ExitBootServices (retrying if the map
 *      changed), convert the map to E820 form, publish boot_info (magic
 *      last), switch CR3 and jump.
 *
 * Built for x86_64-unknown-windows (PE32+, Microsoft x64 ABI). Note LLP64:
 * `long` is 32 bits here, so every 64-bit value is printed with %ll.
 */
#include "efi.h"

#include <boot/boot_info.h>
#include <lib/printf.h>
#include <lib/string.h>

#define KERNEL_VMA      0xFFFFFFFF80000000ull
#define KERNEL_PHYS_MIN 0x100000ull
#define PAGE            4096ull
#define IDENTITY_LIMIT  (4ull << 30)
#define COM1            0x3F8
#define CR4_LA57        (1ull << 12)

static struct efi_system_table *st;
static struct efi_boot_services *bs;
static bool boot_services_active = true;

static const struct efi_guid loaded_image_guid = {
    0x5B1B31A1, 0x9562, 0x11D2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
static const struct efi_guid simple_fs_guid = {
    0x964E5B22, 0x6459, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
static const struct efi_guid acpi20_guid = {
    0x8868E871, 0xE4F1, 0x11D3, {0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81}};
static const struct efi_guid acpi10_guid = {
    0xEB9D2D30, 0x2D88, 0x11D3, {0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}};

/* --- output ----------------------------------------------------------------- */

static inline void outb(u16 port, u8 v)
{
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline u8 inb(u16 port)
{
    u8 v;

    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* After ExitBootServices there is no console protocol: poll COM1 directly. */
static void serial_putc(char c)
{
    if (c == '\n')
        serial_putc('\r');
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++)
        ;
    outb(COM1, (u8)c);
}

/* Firmware console (OVMF mirrors it to the serial port), UTF-16, CRLF. */
static void con_write(const char *s)
{
    char16 buf[128];
    usize n = 0;

    for (; *s; s++) {
        if (*s == '\n')
            buf[n++] = '\r';
        buf[n++] = (char16)(u8)*s;
        if (n >= ARRAY_SIZE(buf) - 3) {
            buf[n] = 0;
            st->con_out->output_string(st->con_out, buf);
            n = 0;
        }
    }
    buf[n] = 0;
    st->con_out->output_string(st->con_out, buf);
}

static void __printf(1, 2) efi_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    k_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (boot_services_active) {
        con_write("minios-efi: ");
        con_write(buf);
    } else {
        for (const char *p = "minios-efi: "; *p; p++)
            serial_putc(*p);
        for (const char *p = buf; *p; p++)
            serial_putc(*p);
    }
}

static __noreturn void halt(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}

#define fail(...)                                                                                  \
    do {                                                                                           \
        efi_log("FATAL: " __VA_ARGS__);                                                            \
        halt();                                                                                    \
    } while (0)

/* --- firmware services ------------------------------------------------------ */

static bool claim_pages_at(u64 address, u64 pages)
{
    u64 addr = address;

    return bs->allocate_pages(EFI_ALLOCATE_ADDRESS, EFI_LOADER_DATA, pages, &addr) == EFI_SUCCESS;
}

/* Zeroed pages below `limit`, which the loader's own page tables identity-map. */
static void *alloc_pages_below(u64 pages, u64 limit)
{
    u64 addr = limit - 1;

    if (bs->allocate_pages(EFI_ALLOCATE_MAX_ADDRESS, EFI_LOADER_DATA, pages, &addr) != EFI_SUCCESS)
        fail("out of memory below %#llx (%llu pages)\n", (unsigned long long)limit,
             (unsigned long long)pages);
    memset((void *)(uptr)addr, 0, pages * PAGE);
    return (void *)(uptr)addr;
}

static struct efi_file *open_boot_volume(efi_handle image)
{
    struct efi_loaded_image *li;
    struct efi_simple_file_system *fs;
    struct efi_file *root;

    if (bs->handle_protocol(image, &loaded_image_guid, (void **)&li) != EFI_SUCCESS)
        fail("no LoadedImage protocol\n");
    /* Everything after the CR3 switch runs identity-mapped: it must sit below 4 GiB. */
    if ((uptr)li->image_base + li->image_size > IDENTITY_LIMIT)
        fail("loader image above 4 GiB\n");
    if (bs->handle_protocol(li->device_handle, &simple_fs_guid, (void **)&fs) != EFI_SUCCESS)
        fail("boot device has no file system\n");
    if (fs->open_volume(fs, &root) != EFI_SUCCESS)
        fail("cannot open the boot volume\n");
    return root;
}

/* Reads a whole file into pool memory. NULL (or fatal, if required) when missing. */
static u8 *read_file(struct efi_file *root, const char16 *path, const char *name, u64 *size,
                     bool required)
{
    struct efi_file *f;
    u8 *buf;

    if (root->open(root, &f, path, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS) {
        if (required)
            fail("cannot open %s\n", name);
        return NULL;
    }
    if (f->set_position(f, ~0ull) != EFI_SUCCESS || f->get_position(f, size) != EFI_SUCCESS ||
        f->set_position(f, 0) != EFI_SUCCESS)
        fail("cannot size %s\n", name);
    if (bs->allocate_pool(EFI_LOADER_DATA, *size + 1, (void **)&buf) != EFI_SUCCESS)
        fail("out of memory reading %s\n", name);

    u64 done = 0;

    while (done < *size) {
        u64 chunk = *size - done;

        if (f->read(f, &chunk, buf + done) != EFI_SUCCESS || chunk == 0)
            fail("read error in %s\n", name);
        done += chunk;
    }
    buf[*size] = 0;
    f->close(f);
    return buf;
}

/* --- kernel ELF --------------------------------------------------------------- */

struct elf64_ehdr {
    u8 ident[16];
    u16 type, machine;
    u32 version;
    u64 entry, phoff, shoff;
    u32 flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf64_phdr {
    u32 type, flags;
    u64 offset, vaddr, paddr, filesz, memsz, align;
};

/*
 * Validates the kernel ELF (the same checks as stage 2), claims its
 * physical range from the firmware, copies its segments and zeroes .bss.
 * Returns the entry point.
 */
static u64 load_kernel(const u8 *file, u64 size, struct boot_info *bi)
{
    struct elf64_ehdr eh;
    u64 lo = ~0ull, hi = 0;

    if (size < sizeof(eh))
        fail("kernel.elf is truncated\n");
    memcpy(&eh, file, sizeof(eh));
    if (memcmp(eh.ident,
               "\x7f"
               "ELF",
               4) != 0 ||
        eh.ident[4] != 2 || eh.ident[5] != 1 || eh.type != 2 || eh.machine != 62 ||
        eh.phentsize != sizeof(struct elf64_phdr) || eh.entry < KERNEL_VMA)
        fail("kernel.elf is not an x86_64 kernel executable\n");
    if (eh.phoff > size || (u64)eh.phnum * sizeof(struct elf64_phdr) > size - eh.phoff)
        fail("kernel.elf program headers out of bounds\n");

    for (int pass = 0; pass < 2; pass++) {
        for (u16 i = 0; i < eh.phnum; i++) {
            struct elf64_phdr ph;

            memcpy(&ph, file + eh.phoff + (u64)i * sizeof(ph), sizeof(ph));
            if (ph.type != 1 /* PT_LOAD */ || ph.memsz == 0)
                continue;
            if (pass == 0) {
                if (ph.filesz > ph.memsz || ph.offset > size || ph.filesz > size - ph.offset ||
                    ph.paddr < KERNEL_PHYS_MIN || ph.paddr + ph.memsz < ph.paddr ||
                    ph.paddr + ph.memsz > IDENTITY_LIMIT)
                    fail("kernel.elf segment %u is invalid\n", i);
                lo = MIN(lo, ph.paddr);
                hi = MAX(hi, ph.paddr + ph.memsz);
            } else {
                memcpy((void *)(uptr)ph.paddr, file + ph.offset, ph.filesz);
                memset((void *)(uptr)(ph.paddr + ph.filesz), 0, ph.memsz - ph.filesz);
            }
        }
        if (pass == 0) {
            if (hi == 0)
                fail("kernel.elf has nothing to load\n");
            u64 base = lo & ~(PAGE - 1), pages = (hi - base + PAGE - 1) / PAGE;

            /* The kernel is linked to run at this physical range; the firmware must yield it. */
            if (!claim_pages_at(base, pages))
                fail("physical %#llx-%#llx (kernel) is in use by the firmware\n",
                     (unsigned long long)base, (unsigned long long)(base + pages * PAGE));
        }
    }
    bi->kernel_phys_start = lo;
    bi->kernel_phys_end = hi;
    return eh.entry;
}

/* --- page tables and hand-off --------------------------------------------------- */

/* The RSDP from the configuration table: ACPI 2.0+ preferred, 1.0 accepted. */
static u64 find_acpi_rsdp(void)
{
    u64 v1 = 0;

    for (u64 i = 0; i < st->number_of_table_entries; i++) {
        const struct efi_configuration_table *t = &st->configuration_table[i];

        if (memcmp(&t->vendor_guid, &acpi20_guid, sizeof(struct efi_guid)) == 0)
            return (u64)(uptr)t->vendor_table;
        if (memcmp(&t->vendor_guid, &acpi10_guid, sizeof(struct efi_guid)) == 0)
            v1 = (u64)(uptr)t->vendor_table;
    }
    return v1;
}

/* 0-4 GiB identity (this loader keeps running after the switch) and 0-1 GiB at
 * KERNEL_VMA, exactly the windows stage 2 provides. 2 MiB pages. */
static u64 build_page_tables(void)
{
    u64 *pml4 = alloc_pages_below(1, IDENTITY_LIMIT);
    u64 *pdpt_lo = alloc_pages_below(1, IDENTITY_LIMIT);
    u64 *pdpt_hi = alloc_pages_below(1, IDENTITY_LIMIT);
    u64 *pd = alloc_pages_below(4, IDENTITY_LIMIT);

    for (u64 i = 0; i < 4ull * 512; i++)
        pd[i] = (i << 21) | 0x83; /* present, writable, 2 MiB */
    for (u64 i = 0; i < 4; i++)
        pdpt_lo[i] = ((uptr)pd + i * PAGE) | 0x3;
    pdpt_hi[510] = (uptr)pd | 0x3; /* -2 GiB -> physical 0 */
    pml4[0] = (uptr)pdpt_lo | 0x3;
    pml4[511] = (uptr)pdpt_hi | 0x3;
    return (uptr)pml4;
}

static u32 e820_type_of(u32 efi_type)
{
    switch (efi_type) {
    case EFI_LOADER_CODE: /* this loader and its data are dead once the kernel runs */
    case EFI_LOADER_DATA:
    case EFI_BOOT_SERVICES_CODE: /* reclaimable after ExitBootServices */
    case EFI_BOOT_SERVICES_DATA:
    case EFI_CONVENTIONAL_MEMORY:
        return E820_USABLE;
    case EFI_ACPI_RECLAIM_MEMORY:
        return E820_ACPI_RECLAIMABLE;
    case EFI_ACPI_MEMORY_NVS:
        return E820_ACPI_NVS;
    case EFI_UNUSABLE_MEMORY:
        return E820_BAD;
    default: /* runtime services, MMIO, reserved, persistent...: hands off */
        return E820_RESERVED;
    }
}

/* EFI map -> E820 entries, merging contiguous runs of the same resulting type. */
static void convert_memory_map(const u8 *map, u64 map_size, u64 desc_size, struct boot_info *bi)
{
    u32 n = 0;

    for (u64 off = 0; off + desc_size <= map_size; off += desc_size) {
        struct efi_memory_descriptor d;

        memcpy(&d, map + off, sizeof(d));
        u32 type = e820_type_of(d.type);
        u64 base = d.physical_start, len = d.number_of_pages * PAGE;

        if (len == 0)
            continue;
        if (n > 0 && bi->mmap[n - 1].type == type &&
            bi->mmap[n - 1].base + bi->mmap[n - 1].length == base) {
            bi->mmap[n - 1].length += len;
            continue;
        }
        if (n == BOOT_MMAP_MAX) {
            efi_log("memory map truncated at %u entries\n", n); /* loses memory, never invents it */
            break;
        }
        bi->mmap[n++] =
            (struct e820_entry){.base = base, .length = len, .type = type, .acpi_attrs = 1};
    }
    bi->mmap_count = n;
}

static __noreturn void jump_to_kernel(u64 pml4, u64 entry, struct boot_info *bi)
{
    __asm__ volatile("cli\n\t"
                     "mov $0xFF, %%al\n\t"
                     "out %%al, $0xA1\n\t" /* mask both 8259s, as stage 2 does */
                     "out %%al, $0x21\n\t"
                     "mov %0, %%cr3\n\t"
                     "mov %2, %%rdi\n\t"
                     "jmp *%1"
                     :
                     : "r"(pml4), "r"(entry), "r"((u64)(uptr)bi)
                     : "rax", "rdi", "memory");
    __builtin_unreachable();
}

efi_status efi_main(efi_handle image, struct efi_system_table *system_table);

efi_status efi_main(efi_handle image, struct efi_system_table *system_table)
{
    u64 rsp, cr4;

    st = system_table;
    bs = st->boot_services;
    efi_log("starting\n");

    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (rsp > IDENTITY_LIMIT)
        fail("firmware stack above 4 GiB\n");
    if (cr4 & CR4_LA57)
        fail("firmware runs 5-level paging; this kernel uses 4-level\n");

    /* boot_info lives at a fixed address the kernel checks. */
    if (!claim_pages_at(BOOT_INFO_PHYS, 1))
        fail("physical %#x (boot_info) is in use by the firmware\n", BOOT_INFO_PHYS);
    struct boot_info *bi = (struct boot_info *)(uptr)BOOT_INFO_PHYS;

    memset(bi, 0, sizeof(*bi));

    struct efi_file *root = open_boot_volume(image);
    u64 kernel_size, cmdline_size;
    const u8 *kernel = read_file(root, u"\\minios\\kernel.elf", "kernel.elf", &kernel_size, true);
    const u8 *cmdline = read_file(root, u"\\minios\\cmdline", "cmdline", &cmdline_size, false);

    if (cmdline) {
        usize n = 0;

        while (n < BOOT_CMDLINE_MAX - 1 && n < cmdline_size && cmdline[n] != '\n' &&
               cmdline[n] != '\r')
            n++;
        memcpy(bi->cmdline, cmdline, n);
    }

    u64 entry = load_kernel(kernel, kernel_size, bi);
    u64 pml4 = build_page_tables();

    bi->acpi_rsdp = find_acpi_rsdp();
    if (!bi->acpi_rsdp)
        efi_log("no ACPI RSDP in the configuration table\n");

    efi_log("kernel %#llx-%#llx, entry %#llx, cmdline \"%s\"\n",
            (unsigned long long)bi->kernel_phys_start, (unsigned long long)bi->kernel_phys_end,
            (unsigned long long)entry, bi->cmdline);

    /* The final memory map. Nothing may allocate (or print) between getting it and
     * ExitBootServices, and the map may still change once: retry on a stale key. */
    u64 capacity = 0, key = 0, desc_size = 0;
    u32 desc_version;
    u8 *map = NULL;

    bs->get_memory_map(&capacity, NULL, &key, &desc_size, &desc_version);
    capacity += 16 * desc_size; /* room for the pool allocation below, and drift */
    if (bs->allocate_pool(EFI_LOADER_DATA, capacity, (void **)&map) != EFI_SUCCESS)
        fail("out of memory for the memory map\n");

    u64 map_size = 0;
    efi_status status = EFI_INVALID_PARAMETER;

    for (int attempt = 0; attempt < 4 && status != EFI_SUCCESS; attempt++) {
        map_size = capacity;
        if (bs->get_memory_map(&map_size, (struct efi_memory_descriptor *)map, &key, &desc_size,
                               &desc_version) != EFI_SUCCESS)
            fail("GetMemoryMap failed\n");
        status = bs->exit_boot_services(image, key);
    }
    if (status != EFI_SUCCESS)
        fail("ExitBootServices failed\n");
    boot_services_active = false;

    convert_memory_map(map, map_size, desc_size, bi);
    bi->boot_drive = 0;
    bi->loader = BOOT_LOADER_UEFI;
    bi->version = BOOT_INFO_VERSION;
    __atomic_store_n(&bi->magic, BOOT_INFO_MAGIC, __ATOMIC_RELEASE); /* written last */

    efi_log("boot services exited, %u memory regions; jumping to the kernel\n", bi->mmap_count);
    jump_to_kernel(pml4, entry, bi);
}
