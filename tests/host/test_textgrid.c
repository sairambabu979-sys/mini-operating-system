// SPDX-License-Identifier: MIT
/*
 * Terminal logic of the screen consoles, against hand-checked cases and
 * against an independent reference model on random output, rendered at
 * random moments.
 */
#include "test.h"

#include <lib/textgrid.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define COLS 10
#define ROWS 4

/* A fake display the draw callback writes to, plus a draw counter. */
struct screen {
    uint8_t px[ROWS][COLS];
    int draws;
};

static void draw(void *ctx, uint32_t col, uint32_t row, uint8_t ch)
{
    struct screen *s = ctx;

    s->px[row][col] = ch;
    s->draws++;
}

static uint8_t shown_buf[COLS * ROWS];

static void setup(struct textgrid *g, uint8_t *cells, struct screen *s)
{
    memset(cells, ' ', COLS * ROWS);
    memset(shown_buf, ' ', sizeof(shown_buf));
    memset(s->px, ' ', sizeof(s->px));
    s->draws = 0;
    textgrid_init(g, cells, shown_buf, COLS, ROWS, draw, s);
}

static void puts_grid(struct textgrid *g, const char *str)
{
    textgrid_write(g, str, strlen(str));
}

/* Row `row` of the shadow copy as a string, trailing blanks included. */
static const char *row_text(const struct textgrid *g, uint32_t row)
{
    static char buf[COLS + 1];

    for (uint32_t c = 0; c < COLS; c++)
        buf[c] = (char)textgrid_cell(g, c, row);
    buf[COLS] = '\0';
    return buf;
}

TEST(textgrid_prints_at_the_cursor)
{
    uint8_t cells[COLS * ROWS];
    struct screen s;
    struct textgrid g;

    setup(&g, cells, &s);
    puts_grid(&g, "hi\nthere");
    EXPECT_EQ(s.draws, 0); /* nothing reaches the display before a render */
    EXPECT_EQ(textgrid_render(&g), 7);
    EXPECT_STREQ(row_text(&g, 0), "hi        ");
    EXPECT_STREQ(row_text(&g, 1), "there     ");
    EXPECT_EQ(g.col, 5);
    EXPECT_EQ(g.row, 1);
    EXPECT_EQ(s.draws, 7);             /* blanks were already blank: never redrawn */
    EXPECT_EQ(textgrid_render(&g), 0); /* and a second render draws nothing */
}

TEST(textgrid_full_line_then_newline_leaves_no_blank_line)
{
    uint8_t cells[COLS * ROWS];
    struct screen s;
    struct textgrid g;

    setup(&g, cells, &s);
    puts_grid(&g, "0123456789"); /* exactly one row */
    EXPECT_EQ(g.row, 0);
    EXPECT_EQ(g.col, COLS - 1);
    EXPECT_TRUE(g.wrap_pending);
    puts_grid(&g, "\nnext");
    EXPECT_STREQ(row_text(&g, 1), "next      ");
    EXPECT_EQ(g.row, 1);
}

TEST(textgrid_wraps_a_long_line)
{
    uint8_t cells[COLS * ROWS];
    struct screen s;
    struct textgrid g;

    setup(&g, cells, &s);
    puts_grid(&g, "0123456789ab");
    EXPECT_STREQ(row_text(&g, 0), "0123456789");
    EXPECT_STREQ(row_text(&g, 1), "ab        ");
    EXPECT_EQ(g.col, 2);
}

TEST(textgrid_scrolls_and_redraws_only_changed_cells)
{
    uint8_t cells[COLS * ROWS];
    struct screen s;
    struct textgrid g;

    setup(&g, cells, &s);
    puts_grid(&g, "a\nb\nc\nd");
    textgrid_render(&g);
    s.draws = 0;
    puts_grid(&g, "\ne");
    textgrid_render(&g);
    EXPECT_STREQ(row_text(&g, 0), "b         ");
    EXPECT_STREQ(row_text(&g, 2), "d         ");
    EXPECT_STREQ(row_text(&g, 3), "e         ");
    EXPECT_EQ(g.scrolls, 1);
    /* One changed cell per row: the blanking of row 3 and its 'e' collapse. */
    EXPECT_EQ(s.draws, 4);
    EXPECT_EQ(memcmp(s.px, cells, sizeof(cells)), 0);
}

TEST(textgrid_a_burst_of_scrolling_is_drawn_once)
{
    uint8_t cells[COLS * ROWS];
    struct screen s;
    struct textgrid g;

    setup(&g, cells, &s);
    for (int i = 0; i < 50; i++)
        puts_grid(&g, i % 2 ? "odd line\n" : "even\n");
    EXPECT_EQ(g.scrolls, 47);
    /* Only the final screen: "odd line", "even", "odd line"; their spaces were already blank. */
    EXPECT_EQ(textgrid_render(&g), 7 + 4 + 7);
    EXPECT_EQ(memcmp(s.px, cells, sizeof(cells)), 0);
}

TEST(textgrid_control_characters)
{
    uint8_t cells[COLS * ROWS];
    struct screen s;
    struct textgrid g;

    setup(&g, cells, &s);
    puts_grid(&g, "abc\rX");
    EXPECT_STREQ(row_text(&g, 0), "Xbc       ");
    puts_grid(&g, "\b\bY"); /* backspace stops at column 0 */
    EXPECT_STREQ(row_text(&g, 0), "Ybc       ");
    puts_grid(&g, "\tZ");
    EXPECT_EQ(g.col, 9);
    EXPECT_EQ(textgrid_cell(&g, 8, 0), 'Z');
    puts_grid(&g, "\t\t"); /* tabs past the end clamp to the last column */
    EXPECT_EQ(g.col, COLS - 1);
    EXPECT_EQ(g.row, 0);
    puts_grid(&g, "\n\x01\x7f\xc3");
    EXPECT_STREQ(row_text(&g, 1), "???       ");
}

TEST(textgrid_tab_keeps_a_pending_wrap)
{
    uint8_t cells[COLS * ROWS];
    struct screen s;
    struct textgrid g;

    setup(&g, cells, &s);
    puts_grid(&g, "0123456789\tX");              /* a full line, a tab, then a character */
    EXPECT_STREQ(row_text(&g, 0), "0123456789"); /* nothing overwritten */
    EXPECT_STREQ(row_text(&g, 1), "X         ");
}

TEST(textgrid_set_cursor_clamps)
{
    uint8_t cells[COLS * ROWS];
    struct screen s;
    struct textgrid g;

    setup(&g, cells, &s);
    textgrid_set_cursor(&g, 100, 100);
    EXPECT_EQ(g.col, COLS - 1);
    EXPECT_EQ(g.row, ROWS - 1);
    puts_grid(&g, "xy"); /* last cell, then a wrap that scrolls */
    EXPECT_EQ(textgrid_cell(&g, COLS - 1, ROWS - 2), 'x');
    EXPECT_STREQ(row_text(&g, ROWS - 1), "y         ");
}

/*
 * Reference model: the same semantics written the obvious way (whole
 * screen as a 2D array, memmove scrolling, no diffing). The real grid,
 * fed the same random stream, must end with the same cells and cursor,
 * and its incremental draws must have produced exactly those cells.
 */
struct model {
    uint8_t cells[ROWS][COLS];
    uint32_t col, row;
    int pending;
};

static void model_newline(struct model *m)
{
    m->col = 0;
    m->pending = 0;
    if (m->row + 1 < ROWS) {
        m->row++;
    } else {
        memmove(m->cells[0], m->cells[1], (ROWS - 1) * COLS);
        memset(m->cells[ROWS - 1], ' ', COLS);
    }
}

static void model_putc(struct model *m, uint8_t c)
{
    if (c == '\n') {
        model_newline(m);
    } else if (c == '\r') {
        m->col = 0;
        m->pending = 0;
    } else if (c == '\t') {
        if (!m->pending) {
            m->col = (m->col / 8 + 1) * 8;
            if (m->col >= COLS)
                m->col = COLS - 1;
        }
    } else if (c == '\b') {
        if (m->col)
            m->col--;
        m->pending = 0;
    } else {
        if (m->pending)
            model_newline(m);
        m->cells[m->row][m->col] = (c >= 0x20 && c <= 0x7E) ? c : '?';
        if (m->col + 1 < COLS)
            m->col++;
        else
            m->pending = 1;
    }
}

TEST(textgrid_matches_a_reference_model_on_random_output)
{
    static const uint8_t alphabet[] = "abcdefghij \n\n\r\t\b\x01\xff";

    srand(0x7E47);
    for (int iter = 0; iter < 20000; iter++) {
        uint8_t cells[COLS * ROWS];
        struct screen s;
        struct textgrid g;
        struct model m;

        setup(&g, cells, &s);
        memset(m.cells, ' ', sizeof(m.cells));
        m.col = m.row = 0;
        m.pending = 0;

        int n = rand() % 120;

        for (int i = 0; i < n; i++) {
            uint8_t c = alphabet[rand() % (sizeof(alphabet) - 1)];

            textgrid_putc(&g, (char)c);
            model_putc(&m, c);
            if (rand() % 16 == 0)
                textgrid_render(&g);
        }
        textgrid_render(&g);
        EXPECT_EQ(memcmp(cells, m.cells, sizeof(cells)), 0);
        EXPECT_EQ(memcmp(s.px, cells, sizeof(cells)), 0);
        EXPECT_EQ(g.col, m.col);
        EXPECT_EQ(g.row, m.row);
        EXPECT_EQ(g.wrap_pending, m.pending);
        if (memcmp(cells, m.cells, sizeof(cells)) != 0)
            return; /* one counterexample is enough output */
    }
}
