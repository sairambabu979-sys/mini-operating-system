/* SPDX-License-Identifier: MIT */
/*
 * Screen consoles: text on a UEFI framebuffer, or BIOS VGA text mode.
 *
 * Both are a textgrid (lib/textgrid.h) plus a way to draw one cell, and
 * both register with the console (core/printk.h), which replays the boot
 * log onto them. Without either, the console is COM1 only.
 */
#ifndef MINIOS_DRIVERS_SCREEN_H
#define MINIOS_DRIVERS_SCREEN_H

#include <boot/boot_info.h>
#include <lib/fbrender.h>
#include <lib/textgrid.h>

/*
 * Brings up whichever screen the loader left: the framebuffer it
 * reported, else (BIOS boots only) VGA text mode if the BIOS data area
 * says the display is in it. "console=serial" disables both. Needs the
 * memory manager.
 */
void screen_init(const struct boot_info *bi);

/* The grid of the active screen console, or NULL. */
const struct textgrid *screen_grid(void);

/* The framebuffer surface when that is the active screen, else NULL. */
const struct fb_surface *screen_framebuffer(void);

/* Pixel size of one text cell on the framebuffer. */
u32 screen_cell_width(void);
u32 screen_cell_height(void);

/* The VGA text buffer (character | attribute << 8) when that is the active screen, else NULL. */
const volatile u16 *screen_vga_text(void);

/*
 * Renders after console_go_live() (outside a panic) that ran with
 * interrupts off or preemption on. Must stay 0: a slow redraw with
 * interrupts off loses timer ticks, and a preempted one stalls the
 * screen (see core/printk.c). Debug builds panic instead of counting.
 */
u64 screen_render_violations(void);

/* Text colors on the framebuffer, 0xRRGGBB. */
#define SCREEN_FG 0xC0C0C0u
#define SCREEN_BG 0x000000u

#endif /* MINIOS_DRIVERS_SCREEN_H */
