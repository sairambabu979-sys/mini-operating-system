/* SPDX-License-Identifier: MIT */
/*
 * The console font: 8x8 pixels per glyph, ASCII only.
 */
#ifndef MINIOS_LIB_FONT8X8_H
#define MINIOS_LIB_FONT8X8_H

#include <types.h>

#define FONT8X8_GLYPHS 128

/* font8x8[c][row]: bit `col` set means pixel (col, row) is ink; row 0 is the top. */
extern const u8 font8x8[FONT8X8_GLYPHS][8];

#endif /* MINIOS_LIB_FONT8X8_H */
