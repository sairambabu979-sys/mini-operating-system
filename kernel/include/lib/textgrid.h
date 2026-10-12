/* SPDX-License-Identifier: MIT */
/*
 * A character grid with a cursor: the terminal logic of the screen
 * consoles, independent of how a cell is drawn.
 *
 * Pure code (host-tested). Writing only updates `cells`, in memory;
 * textgrid_render() then draws exactly the cells that differ from
 * `shown`, what the display last got. A burst of output that scrolls
 * the screen twenty times costs one redraw, not twenty, and nothing ever
 * reads the display back (framebuffer reads are very slow on hardware).
 *
 * Semantics follow a VT100 for the subset a log needs:
 *   - printable ASCII is stored at the cursor; anything else as '?';
 *   - writing the last column sets a pending wrap, resolved by the next
 *     printable character, so a line of exactly `cols` characters
 *     followed by '\n' does not leave a blank line;
 *   - '\n' moves to the start of the next line (the console has no
 *     separate CR/LF translation), '\r' to the start of this one, '\t' to
 *     the next multiple of 8 (or nowhere, keeping a pending wrap), '\b'
 *     one column left;
 *   - moving below the last row scrolls everything up one row.
 */
#ifndef MINIOS_LIB_TEXTGRID_H
#define MINIOS_LIB_TEXTGRID_H

#include <types.h>

typedef void (*textgrid_draw_t)(void *ctx, u32 col, u32 row, u8 ch);

struct textgrid {
    u32 cols, rows;
    u32 col, row; /* the cursor; col < cols and row < rows always */
    bool wrap_pending;
    u8 *cells; /* rows * cols characters, row-major: the screen as it should be */
    u8 *shown; /* the same, as last drawn */
    textgrid_draw_t draw;
    void *ctx;
    u64 scrolls;
};

/*
 * `cells` and `shown` (rows * cols bytes each, cols and rows at least 1)
 * must both hold what the display currently shows: the caller either
 * reads it back once or clears display and arrays alike. The cursor
 * starts at the top left.
 */
void textgrid_init(struct textgrid *g, u8 *cells, u8 *shown, u32 cols, u32 rows,
                   textgrid_draw_t draw, void *ctx);

/* Moves the cursor, clamped to the grid. Clears any pending wrap. */
void textgrid_set_cursor(struct textgrid *g, u32 col, u32 row);

void textgrid_putc(struct textgrid *g, char c);
void textgrid_write(struct textgrid *g, const char *s, usize len);

/* Draws every cell whose character changed since the last render. Returns how many. */
u32 textgrid_render(struct textgrid *g);

/* The character in a cell, as it should be (rendered or not). */
static inline u8 textgrid_cell(const struct textgrid *g, u32 col, u32 row)
{
    return g->cells[(usize)row * g->cols + col];
}

#endif /* MINIOS_LIB_TEXTGRID_H */
