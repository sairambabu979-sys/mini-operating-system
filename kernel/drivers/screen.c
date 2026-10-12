// SPDX-License-Identifier: MIT
/*
 * Screen consoles. See <drivers/screen.h>.
 */
#define pr_fmt(fmt) "screen: " fmt

#include <arch/io.h>
#include <arch/irqflags.h>
#include <arch/paging.h>
#include <core/klog.h>
#include <core/panic.h>
#include <core/printk.h>
#include <drivers/screen.h>
#include <lib/cmdline.h>
#include <lib/string.h>
#include <mm/kmalloc.h>
#include <mm/vmalloc.h>
#include <mm/vmm.h>

/* BIOS data area fields (IBM PC/AT BIOS technical reference). */
#define BDA_PHYS       0x400u
#define BDA_VIDEO_MODE 0x49u /* u8 */
#define BDA_COLUMNS    0x4Au /* u16 */
#define BDA_CURSOR     0x50u /* u8 column, u8 row (page 0) */
#define BDA_CRTC_PORT  0x63u /* u16: 0x3D4 colour, 0x3B4 mono */
#define BDA_LAST_ROW   0x84u /* u8: rows - 1 (EGA and later) */
#define VGA_COLOR_TEXT 0xB8000u
#define VGA_MONO_TEXT  0xB0000u
#define VGA_ATTR       0x0700u /* light grey on black */
#define CRTC_CURSOR_HI 0x0Eu
#define CRTC_CURSOR_LO 0x0Fu

static struct textgrid grid;
static bool active;
static u64 renders_irqs_off; /* once live, outside a panic: always 0 */

/* Both arrays hold `n` cells; NULL if either allocation fails. */
static u8 *alloc_grid(usize n, u8 **shown)
{
    u8 *cells = kmalloc(n);

    *shown = kmalloc(n);
    if (!cells || !*shown) {
        kfree(cells);
        kfree(*shown);
        return NULL;
    }
    return cells;
}

static void putc_grid(char c)
{
    textgrid_putc(&grid, c);
}

static void render(void)
{
    if (console_is_live() && !arch_irqs_enabled() && !panic_in_progress())
        renders_irqs_off++;
    textgrid_render(&grid);
}

/* --- framebuffer ------------------------------------------------------------- */

static struct fb_surface fb;
static u32 scale_x, scale_y;
static u32 fg, bg;

static void fb_draw_cell(void *ctx, u32 col, u32 row, u8 ch)
{
    (void)ctx;
    fb_draw_glyph(&fb, col * 8 * scale_x, row * 8 * scale_y, ch, scale_x, scale_y, fg, bg);
}

static const struct console_screen fb_screen = {
    .name = "framebuffer", .putc = putc_grid, .flush = render};

static bool shift_ok(u8 shift)
{
    return shift <= 24 && shift % 8 == 0;
}

static bool fb_init(const struct boot_framebuffer *b)
{
    if (b->bpp != 32 || b->width < 64 || b->height < 64 || b->pitch / 4 < b->width ||
        b->pitch % 4 != 0 || !IS_ALIGNED(b->phys, 4) || !shift_ok(b->red_shift) ||
        !shift_ok(b->green_shift) || !shift_ok(b->blue_shift)) {
        pr_warn("framebuffer %ux%u, %u bpp, pitch %u: unsupported\n", b->width, b->height, b->bpp,
                b->pitch);
        return false;
    }
    void *base = iomap(b->phys, (usize)b->pitch * b->height, VM_WRITE | VM_WC);

    if (!base) {
        pr_warn("cannot map the framebuffer at %#llx\n", (unsigned long long)b->phys);
        return false;
    }
    fb = (struct fb_surface){.base = base,
                             .width = b->width,
                             .height = b->height,
                             .pitch = b->pitch,
                             .red_shift = b->red_shift,
                             .green_shift = b->green_shift,
                             .blue_shift = b->blue_shift};
    /* 8x16 cells; doubled on screens wide enough to make those tiny. */
    scale_x = b->width >= 1920 ? 2 : 1;
    scale_y = 2 * scale_x;

    u32 cols = b->width / (8 * scale_x), rows = b->height / (8 * scale_y);
    u8 *shown;
    u8 *cells = alloc_grid((usize)cols * rows, &shown);

    if (!cells)
        return false;
    memset(cells, ' ', (usize)cols * rows);
    memset(shown, ' ', (usize)cols * rows);
    fg = fb_color(&fb, SCREEN_FG);
    bg = fb_color(&fb, SCREEN_BG);
    fb_fill(&fb, 0, 0, fb.width, fb.height, bg);
    textgrid_init(&grid, cells, shown, cols, rows, fb_draw_cell, NULL);
    active = true;
    console_register_screen(&fb_screen);
    pr_info("framebuffer %ux%u at %#llx: %ux%u text\n", b->width, b->height,
            (unsigned long long)b->phys, cols, rows);
    return true;
}

/* --- VGA text mode ------------------------------------------------------------- */

static volatile u16 *vga;
static u16 crtc_port;

static void vga_draw_cell(void *ctx, u32 col, u32 row, u8 ch)
{
    (void)ctx;
    vga[row * grid.cols + col] = VGA_ATTR | ch;
}

static void vga_flush(void)
{
    u32 pos = grid.row * grid.cols + grid.col;

    render();

    outb(crtc_port, CRTC_CURSOR_LO);
    outb(crtc_port + 1, pos & 0xFF);
    outb(crtc_port, CRTC_CURSOR_HI);
    outb(crtc_port + 1, (pos >> 8) & 0xFF);
}

static const struct console_screen vga_screen = {
    .name = "VGA text", .putc = putc_grid, .flush = vga_flush};

static u16 bda_u16(const volatile u8 *bda, u32 off)
{
    return (u16)(bda[off] | bda[off + 1] << 8);
}

/*
 * Adopts the screen the BIOS left, contents and cursor included, so the
 * loader's progress line stays visible above the kernel's log.
 */
static bool vga_init(void)
{
    const volatile u8 *bda = iomap(BDA_PHYS, 0x100, 0);

    if (!bda)
        return false;
    u8 mode = bda[BDA_VIDEO_MODE];
    u32 cols = bda_u16(bda, BDA_COLUMNS);
    u32 rows = bda[BDA_LAST_ROW] ? bda[BDA_LAST_ROW] + 1u : 25u;
    u16 port = bda_u16(bda, BDA_CRTC_PORT);

    if (mode != 2 && mode != 3 && mode != 7) {
        pr_info("display in BIOS mode %#x, not text: no screen console\n", mode);
        return false;
    }
    if (cols < 40 || cols > 132 || rows > 60 || (port != 0x3D4 && port != 0x3B4)) {
        pr_warn("implausible BIOS text mode (%ux%u, CRTC %#x)\n", cols, rows, port);
        return false;
    }
    u64 phys = mode == 7 ? VGA_MONO_TEXT : VGA_COLOR_TEXT;

    vga = iomap(phys, (usize)cols * rows * 2, VM_WRITE | VM_NOCACHE);

    u8 *shown;
    u8 *cells = vga ? alloc_grid((usize)cols * rows, &shown) : NULL;

    if (!cells)
        return false;
    for (u32 i = 0; i < cols * rows; i++) {
        u8 ch = vga[i] & 0xFF;

        /* A non-ASCII cell is "shown" as itself, so the first render replaces it. */
        shown[i] = ch;
        cells[i] = ch >= 0x20 && ch <= 0x7E ? ch : ' ';
    }
    crtc_port = port;
    textgrid_init(&grid, cells, shown, cols, rows, vga_draw_cell, NULL);
    textgrid_set_cursor(&grid, bda[BDA_CURSOR], bda[BDA_CURSOR + 1]);
    if (grid.col != 0) /* the loader's progress line ends mid-line on screen */
        textgrid_putc(&grid, '\n');
    active = true;
    console_register_screen(&vga_screen);
    pr_info("VGA text mode %u, %ux%u at %#llx\n", mode, cols, rows, (unsigned long long)phys);
    return true;
}

void screen_init(const struct boot_info *bi)
{
    char value[16];

    if (cmdline_get(bi->cmdline, "console", value, sizeof(value)) && strcmp(value, "serial") == 0) {
        pr_info("console=serial: no screen console\n");
        return;
    }
    if (bi->framebuffer.phys)
        fb_init(&bi->framebuffer);
    else if (bi->loader == BOOT_LOADER_BIOS)
        vga_init();
}

const struct textgrid *screen_grid(void)
{
    return active ? &grid : NULL;
}

const struct fb_surface *screen_framebuffer(void)
{
    return active && fb.base ? &fb : NULL;
}

u32 screen_cell_width(void)
{
    return 8 * scale_x;
}

u32 screen_cell_height(void)
{
    return 8 * scale_y;
}

const volatile u16 *screen_vga_text(void)
{
    return active && vga ? vga : NULL;
}

u64 screen_renders_with_irqs_off(void)
{
    return renders_irqs_off;
}
