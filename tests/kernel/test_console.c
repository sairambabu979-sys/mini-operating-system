// SPDX-License-Identifier: MIT
/*
 * Console tests: the log ring, the screen consoles end to end (down to the
 * pixels or VGA cells actually in video memory), and the write-combining
 * memory type the framebuffer is mapped with.
 */
#include <arch/cpuid.h>
#include <arch/irqflags.h>
#include <arch/msr.h>
#include <arch/paging.h>
#include <core/boot.h>
#include <core/ktest.h>
#include <core/printk.h>
#include <drivers/screen.h>
#include <lib/cmdline.h>
#include <lib/font8x8.h>
#include <lib/string.h>
#include <mm/vmalloc.h>
#include <mm/vmm.h>

static bool log_tail_contains(const char *needle)
{
    static char tail[512];
    usize n = console_log_read(tail, sizeof(tail) - 1);

    tail[n] = '\0';
    for (usize i = 0; i + strlen(needle) <= n; i++)
        if (memcmp(tail + i, needle, strlen(needle)) == 0)
            return true;
    return false;
}

KTEST(console_log_keeps_recent_output)
{
    kprintf("console-log-probe-%d\n", 4242);
    KEXPECT(log_tail_contains("console-log-probe-4242\n"));
    KEXPECT(!log_tail_contains("console-log-probe-4243"));
}

KTEST(screen_console_matches_the_loader)
{
    const struct boot_info *bi = boot_info_get();
    char value[16];

    if (cmdline_get(bi->cmdline, "console", value, sizeof(value))) {
        KEXPECT(screen_grid() == NULL);
        return;
    }
    /* QEMU's OVMF always reports a GOP framebuffer; SeaBIOS leaves text mode 3. */
    if (bi->loader == BOOT_LOADER_UEFI) {
        KEXPECT(bi->framebuffer.phys != 0);
        KEXPECT(screen_framebuffer() != NULL);
    } else {
        KEXPECT(screen_vga_text() != NULL);
    }
    KEXPECT(screen_grid() != NULL);
    KEXPECT(console_screen() != NULL);
}

/* Prints a unique line and returns the grid row it landed on. */
static u32 print_probe(const char *text)
{
    kprintf("%s\n", text);
    return screen_grid()->row - 1; /* the newline moved the cursor down one */
}

KTEST(screen_shows_printed_text)
{
    static const char probe[] = "screen-probe: the quick brown fox";
    const struct textgrid *g = screen_grid();

    if (!g)
        KTEST_SKIP("no screen console");
    if (g->row == 0)
        kprintf("\n"); /* make sure the probe does not start on the top row */
    u32 row = print_probe(probe);

    console_drain(); /* drawn even if another context was mid-feed */

    for (u32 i = 0; i < sizeof(probe) - 1; i++)
        KEXPECT_EQ(textgrid_cell(g, i, row), (u8)probe[i]);

    const volatile u16 *vga = screen_vga_text();

    if (vga) {
        for (u32 i = 0; i < sizeof(probe) - 1; i++)
            KEXPECT_EQ(vga[row * g->cols + i], 0x0700u | (u8)probe[i]);
        return;
    }

    /* Framebuffer: read back every pixel of the first two cells of the probe. */
    const struct fb_surface *fb = screen_framebuffer();
    u32 cw = screen_cell_width(), ch = screen_cell_height();
    u32 fg = fb_color(fb, SCREEN_FG), bg = fb_color(fb, SCREEN_BG);

    for (u32 cell = 0; cell < 2; cell++) {
        const u8 *glyph = font8x8[(u8)probe[cell]];

        for (u32 y = 0; y < ch; y++) {
            const volatile u32 *line =
                (const volatile u32 *)((uptr)fb->base + (usize)(row * ch + y) * fb->pitch);

            for (u32 x = 0; x < cw; x++) {
                bool ink = (glyph[y * 8 / ch] >> (x * 8 / cw)) & 1;

                KEXPECT_EQ(line[cell * cw + x], ink ? fg : bg);
            }
        }
    }
}

KTEST(screen_scrolls_without_losing_lines)
{
    const struct textgrid *g = screen_grid();

    if (!g)
        KTEST_SKIP("no screen console");
    u64 scrolls = g->scrolls;

    for (u32 i = 0; i < g->rows + 2; i++)
        kprintf("scroll-probe line %u\n", i);
    KEXPECT(g->scrolls >= scrolls + 2);
    KEXPECT_EQ(g->row, g->rows - 1); /* parked on the last row */

    /* The last line printed sits just above the cursor, intact. */
    static const char last[] = "scroll-probe line ";

    for (u32 i = 0; i < sizeof(last) - 1; i++)
        KEXPECT_EQ(textgrid_cell(g, i, g->rows - 2), (u8)last[i]);
}

/*
 * Drawing can take milliseconds, and a timer tick missed with interrupts
 * off is never counted. Output printed with them off is logged at once
 * but drawn only once they are back on.
 */
KTEST(screen_drawing_waits_for_interrupts)
{
    const struct textgrid *g = screen_grid();
    static const char probe[] = "deferred-probe";

    if (!g)
        KTEST_SKIP("no screen console");
    kprintf("\n"); /* start the probe on a fresh line */
    u32 row = g->row, col = g->col;
    u64 scrolls = g->scrolls; /* on the bottom row, a drawn line scrolls instead of moving */
    irqflags_t flags = arch_irq_save();

    kprintf("%s\n", probe);
    KEXPECT(log_tail_contains("deferred-probe\n")); /* logged (and on COM1) */
    KEXPECT_EQ(g->row, row);                        /* but not drawn */
    KEXPECT_EQ(g->col, col);
    KEXPECT_EQ(g->scrolls, scrolls);
    arch_irq_restore(flags);
    console_drain(); /* what the idle loop does */

    u32 at = g->row - 1;

    for (u32 i = 0; i < sizeof(probe) - 1; i++)
        KEXPECT_EQ(textgrid_cell(g, i, at), (u8)probe[i]);
    KEXPECT_EQ(screen_render_violations(), 0); /* over the whole boot so far */
}

KTEST(pat_entry_one_is_write_combining)
{
    if (!cpu_features()->pat)
        KTEST_SKIP("no PAT");
    KEXPECT_EQ(rdmsr(MSR_PAT), PAT_KERNEL);
    KEXPECT_EQ((rdmsr(MSR_PAT) >> 8) & 0xFF, PAT_WC);

    void *p = iomap(0xFEC00000ull, PAGE_SIZE_4K, VM_WRITE | VM_WC);
    u64 pa, pte;

    KEXPECT(p != NULL);
    KEXPECT(vmm_translate(vmm_kernel_space(), (uptr)p, &pa, &pte));
    KEXPECT((pte & PTE_PWT) && !(pte & PTE_PCD)); /* PAT index 1 */
}
