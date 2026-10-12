// SPDX-License-Identifier: MIT
/*
 * Terminal logic for the screen consoles. See <lib/textgrid.h>.
 */
#include <lib/string.h>
#include <lib/textgrid.h>

#define TAB_WIDTH 8

/* The grid keeps both pointers and writes through them later, which the check cannot see. */
// NOLINTNEXTLINE(readability-non-const-parameter)
void textgrid_init(struct textgrid *g, u8 *cells, u8 *shown, u32 cols, u32 rows,
                   textgrid_draw_t draw, void *ctx)
{
    *g = (struct textgrid){
        .cols = cols, .rows = rows, .cells = cells, .shown = shown, .draw = draw, .ctx = ctx};
}

void textgrid_set_cursor(struct textgrid *g, u32 col, u32 row)
{
    g->col = col < g->cols ? col : g->cols - 1;
    g->row = row < g->rows ? row : g->rows - 1;
    g->wrap_pending = false;
}

static void scroll_up(struct textgrid *g)
{
    usize row_bytes = g->cols;

    memmove(g->cells, g->cells + row_bytes, (usize)(g->rows - 1) * row_bytes);
    memset(g->cells + (usize)(g->rows - 1) * row_bytes, ' ', row_bytes);
    g->scrolls++;
}

static void newline(struct textgrid *g)
{
    g->col = 0;
    g->wrap_pending = false;
    if (g->row + 1 < g->rows)
        g->row++;
    else
        scroll_up(g);
}

void textgrid_putc(struct textgrid *g, char c)
{
    u8 ch = (u8)c;

    switch (ch) {
    case '\n':
        newline(g);
        return;
    case '\r':
        g->col = 0;
        g->wrap_pending = false;
        return;
    case '\t': {
        u32 next = (g->col / TAB_WIDTH + 1) * TAB_WIDTH;

        g->col = next < g->cols ? next : g->cols - 1;
        g->wrap_pending = false;
        return;
    }
    case '\b':
        if (g->col > 0)
            g->col--;
        g->wrap_pending = false;
        return;
    default:
        break;
    }

    if (ch < 0x20 || ch > 0x7E)
        ch = '?';
    if (g->wrap_pending)
        newline(g);
    g->cells[(usize)g->row * g->cols + g->col] = ch;
    if (g->col + 1 < g->cols)
        g->col++;
    else
        g->wrap_pending = true;
}

void textgrid_write(struct textgrid *g, const char *s, usize len)
{
    for (usize i = 0; i < len; i++)
        textgrid_putc(g, s[i]);
}

u32 textgrid_render(struct textgrid *g)
{
    u32 drawn = 0;

    for (u32 row = 0; row < g->rows; row++) {
        usize base = (usize)row * g->cols;

        /* Unchanged rows (most of them, between scrolls) cost one compare. */
        if (memcmp(g->cells + base, g->shown + base, g->cols) == 0)
            continue;
        for (u32 col = 0; col < g->cols; col++) {
            if (g->cells[base + col] != g->shown[base + col]) {
                g->shown[base + col] = g->cells[base + col];
                g->draw(g->ctx, col, row, g->cells[base + col]);
                drawn++;
            }
        }
    }
    return drawn;
}
