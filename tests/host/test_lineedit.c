// SPDX-License-Identifier: MIT
/*
 * The shell's line editor: hand-checked cases, then a seeded fuzz whose
 * every echo goes through the real text grid of the screen console, held
 * against a reference model of the editor written separately.
 */
#include "test.h"

#include "../../user/lib/lineedit.h"

#include <lib/textgrid.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Feeds a string; collects the echo, the last event, and every finished line ("a|b|"). */
struct run {
    char echo[4096];
    size_t echo_len;
    char lines[512];
    enum le_event last;
    int done, cancels, eofs;
};

static void feed(struct lineedit *le, struct run *r, const char *s, size_t n)
{
    char echo[LE_ECHO_MAX];
    usize len;

    for (size_t i = 0; i < n; i++) {
        r->last = le_feed(le, (u8)s[i], echo, &len);
        memcpy(r->echo + r->echo_len, echo, len);
        r->echo_len += len;
        r->echo[r->echo_len] = '\0';
        if (r->last == LE_DONE) {
            r->done++;
            strcat(r->lines, le->buf);
            strcat(r->lines, "|");
        }
        r->cancels += r->last == LE_CANCEL;
        r->eofs += r->last == LE_EOF;
    }
}

#define FEED(le, r, lit) feed((le), (r), (lit), sizeof(lit) - 1)

TEST(lineedit_types_and_erases)
{
    struct lineedit le;
    struct run r = {0};

    le_reset(&le);
    FEED(&le, &r, "hellp\x7fo\n");
    EXPECT_STREQ(r.lines, "hello|");
    EXPECT_STREQ(r.echo, "hellp\b \bo\n");

    r = (struct run){0};
    FEED(&le, &r,
         "\b\x7f"
         "ab\b\b\bc\n"); /* erasing past the start does nothing */
    EXPECT_STREQ(r.lines, "c|");
    EXPECT_STREQ(r.echo, "ab\b \b\b \bc\n");

    r = (struct run){0};
    FEED(&le, &r,
         "xyz\x15"
         "abc\n");
    EXPECT_STREQ(r.lines, "abc|");
    EXPECT_STREQ(r.echo, "xyz\b \b\b \b\b \babc\n");
}

TEST(lineedit_cancel_and_eof)
{
    struct lineedit le;
    struct run r = {0};

    le_reset(&le);
    FEED(&le, &r, "abc\x03");
    EXPECT_EQ(r.last, LE_CANCEL);
    EXPECT_STREQ(r.echo, "abc^C\n");
    FEED(&le, &r, "d\n");
    EXPECT_STREQ(r.lines, "d|"); /* the cancelled line is gone */

    r = (struct run){0};
    FEED(&le, &r, "ab\x04\n"); /* Ctrl-D inside a line is ignored */
    EXPECT_STREQ(r.lines, "ab|");
    EXPECT_EQ(r.eofs, 0);
    FEED(&le, &r, "\x04");
    EXPECT_EQ(r.last, LE_EOF);
    EXPECT_STREQ(r.echo, "ab\n");
}

TEST(lineedit_cr_lf_and_crlf_each_end_one_line)
{
    struct lineedit le;
    struct run r = {0};

    le_reset(&le);
    FEED(&le, &r, "a\rb\nc\r\nd\n");
    EXPECT_STREQ(r.lines, "a|b|c|d|");
    EXPECT_STREQ(r.echo, "a\nb\nc\nd\n");

    r = (struct run){0};
    FEED(&le, &r, "\r\r\n\n\r");
    EXPECT_STREQ(r.lines, "||||"); /* CR, CRLF, LF, CR: four empty lines */
}

TEST(lineedit_swallows_escape_sequences)
{
    struct lineedit le;
    struct run r = {0};

    le_reset(&le);
    FEED(&le, &r, "\x1b[Ahi\x1b[1;5D\n");      /* Up, Ctrl-Left */
    FEED(&le, &r, "\x1bOPx\n");                /* F1 as SS3 */
    FEED(&le, &r, "ab\x1b[12\n");              /* cut short by Enter, which still ends the line */
    FEED(&le, &r, "\x1bq\n");                  /* a lone ESC is dropped, the byte kept */
    FEED(&le, &r, "\x1b[1111111111111111q\n"); /* 16 bytes is too long to be a sequence */
    EXPECT_STREQ(r.lines, "hi|x|ab|q|q|");
    EXPECT_STREQ(r.echo, "hi\nx\nab\nq\nq\n");
}

TEST(lineedit_caps_the_line_and_ignores_the_rest)
{
    struct lineedit le;
    struct run r = {0};
    char forty[41];

    memset(forty, 'a', 40);
    forty[40] = '\0';
    le_reset(&le);
    feed(&le, &r, forty, 40);
    FEED(&le, &r, "\n");
    EXPECT_EQ(strlen(r.lines), LE_LINE_MAX - 1 + 1); /* 31 characters and the '|' */
    EXPECT_EQ(r.echo_len, LE_LINE_MAX - 1 + 1);

    r = (struct run){0};
    FEED(&le, &r,
         "a\x80\xff"
         "b\tc\x01\x7f\x7f"
         "d\n"); /* high bytes, Tab and ^A ignored */
    EXPECT_STREQ(r.lines, "ad|");
}

/*
 * The independent model: the same rules, written as a state machine over
 * a string, for the fuzz to hold the editor against.
 */
struct model {
    char line[64];
    size_t len;
    int esc; /* 0 none, 1 after ESC, 2 in a sequence */
    int esc_len;
    bool after_cr;
};

static enum le_event model_feed(struct model *m, uint8_t c)
{
    bool cr = m->after_cr;

    m->after_cr = false;
    if (m->esc == 2) {
        if (c >= 0x20) {
            m->esc_len++;
            if ((c >= 0x40 && c <= 0x7E) || m->esc_len >= 16)
                m->esc = 0;
            return LE_MORE;
        }
        m->esc = 0;
    } else if (m->esc == 1) {
        m->esc = 0;
        if (c == '[' || c == 'O') {
            m->esc = 2;
            m->esc_len = 0;
            return LE_MORE;
        }
    }
    if (c == '\r' || (c == '\n' && !cr)) {
        m->after_cr = c == '\r';
        return LE_DONE;
    }
    if (c == '\n')
        return LE_MORE;
    if (c == 0x7F || c == 0x08) {
        if (m->len)
            m->len--;
    } else if (c == 0x15) {
        m->len = 0;
    } else if (c == 0x03) {
        m->len = 0;
        return LE_CANCEL;
    } else if (c == 0x04) {
        if (!m->len)
            return LE_EOF;
    } else if (c == 0x1B) {
        m->esc = 1;
    } else if (c >= 0x20 && c <= 0x7E && m->len < LE_LINE_MAX - 1) {
        m->line[m->len++] = (char)c;
    }
    return LE_MORE;
}

#define COLS 40
#define ROWS 4

static void no_draw(void *ctx, uint32_t col, uint32_t row, uint8_t ch)
{
    (void)ctx, (void)col, (void)row, (void)ch;
}

/* Interesting bytes are common; the rest of the byte range shows up too. */
static uint8_t random_byte(void)
{
    static const uint8_t common[] = {'a',  'z',  ' ', '~', 0x7F, 0x08, '\r', '\n', 0x15, 0x03,
                                     0x04, 0x1B, '[', 'O', '1',  ';',  'A',  '\t', 0x80, 0xFF};
    int pick = rand() % 4;

    if (pick == 0)
        return (uint8_t)rand();
    return common[rand() % (int)sizeof(common)];
}

TEST(lineedit_fuzz_against_model_and_text_grid)
{
    static uint8_t cells[COLS * ROWS], shown[COLS * ROWS];
    struct textgrid g;
    struct lineedit le;
    struct model m = {0};
    char echo[LE_ECHO_MAX];
    usize echo_len;

    memset(cells, ' ', sizeof(cells));
    memset(shown, ' ', sizeof(shown));
    textgrid_init(&g, cells, shown, COLS, ROWS, no_draw, NULL);
    textgrid_write(&g, "sh> ", 4);
    le_reset(&le);
    srand(7);

    for (int i = 0; i < 200000; i++) {
        uint8_t c = random_byte();
        enum le_event want = model_feed(&m, c);
        enum le_event got = le_feed(&le, c, echo, &echo_len);

        if (got != want || echo_len > LE_ECHO_MAX) {
            test_fail(__FILE__, __LINE__, "byte %d (%#x): event %d, model %d", i, c, got, want);
            return;
        }
        textgrid_write(&g, echo, echo_len);
        if (got == LE_DONE &&
            (le.len != m.len || memcmp(le.buf, m.line, m.len) != 0 || le.buf[le.len] != '\0')) {
            test_fail(__FILE__, __LINE__, "byte %d: line differs from the model", i);
            return;
        }
        if (got != LE_MORE) { /* what the shell does next: a new prompt */
            if (got == LE_EOF)
                textgrid_write(&g, "\n", 1);
            textgrid_write(&g, "sh> ", 4);
            m.len = 0;
            continue;
        }
        /* The cursor row shows exactly the prompt and the line; nothing wrapped. */
        bool ok = !g.wrap_pending && g.col == 4 + le.len && le.len == m.len &&
                  memcmp(le.buf, m.line, m.len) == 0;

        for (uint32_t col = 0; ok && col < COLS; col++) {
            uint8_t want_ch = col < 4            ? (uint8_t) "sh> "[col]
                              : col < 4 + le.len ? (uint8_t)le.buf[col - 4]
                                                 : ' ';
            ok = textgrid_cell(&g, col, g.row) == want_ch;
        }
        if (!ok) {
            test_fail(__FILE__, __LINE__,
                      "byte %d (%#x): screen row or cursor wrong (col %u, len %zu)", i, c, g.col,
                      (size_t)le.len);
            return;
        }
    }
}
