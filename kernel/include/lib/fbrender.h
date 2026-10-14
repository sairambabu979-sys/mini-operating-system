/* SPDX-License-Identifier: MIT */
/*
 * Drawing into a linear 32-bit-per-pixel framebuffer.
 *
 * Pure code (host-tested against an ordinary buffer). Write-only: nothing
 * here reads a pixel back, since framebuffer reads are uncached even when
 * writes are combined.
 */
#ifndef MINIOS_LIB_FBRENDER_H
#define MINIOS_LIB_FBRENDER_H

#include <types.h>

struct fb_surface {
    void *base; /* pixel (0, 0), 4-byte aligned */
    u32 width, height;
    u32 pitch; /* bytes from one row to the next: >= width * 4, a multiple of 4 */
    u8 red_shift, green_shift, blue_shift; /* bit position of each 8-bit channel */
};

/* 0xRRGGBB in the surface's pixel format. */
u32 fb_color(const struct fb_surface *s, u32 rgb);

/* Fills a rectangle, clipped to the surface. */
void fb_fill(const struct fb_surface *s, u32 x, u32 y, u32 w, u32 h, u32 pixel);

/*
 * Draws glyph `ch` of the 8x8 font, each font pixel as an sx-by-sy block,
 * into the (8 * sx)-by-(8 * sy) cell at (x, y), clipped to the surface.
 * Every pixel of the cell is written: ink in `fg`, the rest in `bg`.
 */
void fb_draw_glyph(const struct fb_surface *s, u32 x, u32 y, u8 ch, u32 sx, u32 sy, u32 fg, u32 bg);

#endif /* MINIOS_LIB_FBRENDER_H */
