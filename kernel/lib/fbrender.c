// SPDX-License-Identifier: MIT
/*
 * Framebuffer drawing. See <lib/fbrender.h>.
 */
#include <lib/fbrender.h>
#include <lib/font8x8.h>

static volatile u32 *row_ptr(const struct fb_surface *s, u32 y)
{
    return (volatile u32 *)((uptr)s->base + (usize)y * s->pitch); /* pitch % 4 == 0 */
}

u32 fb_color(const struct fb_surface *s, u32 rgb)
{
    return ((rgb >> 16) & 0xFF) << s->red_shift | ((rgb >> 8) & 0xFF) << s->green_shift |
           (rgb & 0xFF) << s->blue_shift;
}

void fb_fill(const struct fb_surface *s, u32 x, u32 y, u32 w, u32 h, u32 pixel)
{
    if (x >= s->width || y >= s->height)
        return;
    u32 x_end = w > s->width - x ? s->width : x + w;
    u32 y_end = h > s->height - y ? s->height : y + h;

    for (u32 py = y; py < y_end; py++) {
        volatile u32 *row = row_ptr(s, py);

        for (u32 px = x; px < x_end; px++)
            row[px] = pixel;
    }
}

void fb_draw_glyph(const struct fb_surface *s, u32 x, u32 y, u8 ch, u32 sx, u32 sy, u32 fg, u32 bg)
{
    const u8 *glyph = font8x8[ch < FONT8X8_GLYPHS ? ch : '?'];

    if (x >= s->width || y >= s->height || sx == 0 || sy == 0)
        return;
    u32 cell_w = 8 * sx, cell_h = 8 * sy;
    u32 w = cell_w > s->width - x ? s->width - x : cell_w;
    u32 h = cell_h > s->height - y ? s->height - y : cell_h;

    for (u32 dy = 0; dy < h; dy++) {
        u8 bits = glyph[dy / sy];
        volatile u32 *row = row_ptr(s, y + dy) + x;

        for (u32 dx = 0; dx < w; dx++)
            row[dx] = (bits >> (dx / sx)) & 1 ? fg : bg;
    }
}
