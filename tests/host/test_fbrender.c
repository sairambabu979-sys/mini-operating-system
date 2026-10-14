// SPDX-License-Identifier: MIT
/*
 * Framebuffer drawing into an ordinary buffer: exact pixels, the pixel
 * format, row padding left alone, and clipping (ASan catches any write
 * outside an exactly-sized buffer).
 */
#include "test.h"

#include <lib/fbrender.h>
#include <lib/font8x8.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PAD_BYTE 0xA5

static struct fb_surface make_surface(uint32_t w, uint32_t h, uint32_t pad_px)
{
    uint32_t pitch = (w + pad_px) * 4;
    void *mem = malloc((size_t)pitch * h);

    memset(mem, PAD_BYTE, (size_t)pitch * h);
    return (struct fb_surface){.base = mem,
                               .width = w,
                               .height = h,
                               .pitch = pitch,
                               .red_shift = 16,
                               .green_shift = 8,
                               .blue_shift = 0};
}

static uint32_t pixel(const struct fb_surface *s, uint32_t x, uint32_t y)
{
    uint32_t v;

    memcpy(&v, (uint8_t *)s->base + (size_t)y * s->pitch + (size_t)x * 4, 4);
    return v;
}

TEST(fbrender_color_follows_the_pixel_format)
{
    struct fb_surface bgr = {.red_shift = 16, .green_shift = 8, .blue_shift = 0};
    struct fb_surface rgb = {.red_shift = 0, .green_shift = 8, .blue_shift = 16};

    EXPECT_EQ(fb_color(&bgr, 0x123456), 0x123456);
    EXPECT_EQ(fb_color(&rgb, 0x123456), 0x563412);
}

TEST(fbrender_glyph_pixels_are_exact)
{
    struct fb_surface s = make_surface(32, 32, 3);

    fb_draw_glyph(&s, 4, 2, 'A', 1, 2, 0xFFFFFF, 0x000001);
    for (uint32_t y = 0; y < 32; y++) {
        for (uint32_t x = 0; x < 32; x++) {
            uint32_t got = pixel(&s, x, y);

            if (x >= 4 && x < 12 && y >= 2 && y < 18) {
                int ink = (font8x8['A'][(y - 2) / 2] >> (x - 4)) & 1;

                EXPECT_EQ(got, ink ? 0xFFFFFFu : 0x000001u);
            } else {
                EXPECT_EQ(got, 0xA5A5A5A5u); /* outside the cell: untouched */
            }
        }
    }
    /* The padding between rows is never written. */
    for (uint32_t y = 0; y < 32; y++)
        for (uint32_t i = 32 * 4; i < s.pitch; i++)
            EXPECT_EQ(((uint8_t *)s.base)[(size_t)y * s.pitch + i], PAD_BYTE);
    free(s.base);
}

TEST(fbrender_scaled_glyph_covers_its_whole_cell)
{
    struct fb_surface s = make_surface(16, 32, 0);

    fb_draw_glyph(&s, 0, 0, '#', 2, 4, 0xFFFFFF, 0);
    for (uint32_t y = 0; y < 32; y++)
        for (uint32_t x = 0; x < 16; x++)
            EXPECT_EQ(pixel(&s, x, y), (font8x8['#'][y / 4] >> (x / 2)) & 1 ? 0xFFFFFFu : 0u);
    free(s.base);
}

TEST(fbrender_clips_at_the_edges)
{
    struct fb_surface s = make_surface(13, 11, 0); /* not a multiple of any cell */

    fb_draw_glyph(&s, 8, 0, 'W', 1, 2, 0xFFFFFF, 0); /* right edge: 5 of 8 columns */
    for (uint32_t y = 0; y < 11; y++)
        for (uint32_t x = 8; x < 13; x++)
            EXPECT_EQ(pixel(&s, x, y), (font8x8['W'][y / 2] >> (x - 8)) & 1 ? 0xFFFFFFu : 0u);
    fb_draw_glyph(&s, 0, 8, 'M', 1, 2, 0xFFFFFF, 0); /* bottom edge: 3 of 16 rows */
    for (uint32_t y = 8; y < 11; y++)
        for (uint32_t x = 0; x < 8; x++)
            EXPECT_EQ(pixel(&s, x, y), (font8x8['M'][(y - 8) / 2] >> x) & 1 ? 0xFFFFFFu : 0u);
    fb_draw_glyph(&s, 12, 10, 'W', 1, 2, 0xFFFFFF, 0); /* one pixel visible */
    fb_draw_glyph(&s, 13, 0, 'W', 1, 2, 0xFFFFFF, 0);  /* fully outside */
    fb_draw_glyph(&s, 0, 0, 'W', 0, 2, 0xFFFFFF, 0);   /* degenerate scale */
    fb_fill(&s, 10, 9, 100, 100, 0x00FF00);
    EXPECT_EQ(pixel(&s, 12, 10), 0x00FF00u);
    fb_fill(&s, 20, 20, 5, 5, 0x00FF00); /* outside: nothing */
    free(s.base);
}

TEST(fbrender_unknown_characters_draw_as_question_marks)
{
    struct fb_surface a = make_surface(8, 8, 0), b = make_surface(8, 8, 0);

    fb_draw_glyph(&a, 0, 0, 0xC3, 1, 1, 0xFFFFFF, 0);
    fb_draw_glyph(&b, 0, 0, '?', 1, 1, 0xFFFFFF, 0);
    EXPECT_EQ(memcmp(a.base, b.base, 8 * 8 * 4), 0);
    free(a.base);
    free(b.base);
}
