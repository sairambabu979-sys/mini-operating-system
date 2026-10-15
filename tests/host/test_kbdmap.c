// SPDX-License-Identifier: MIT
/*
 * The set 1 decoder against the independent US key table, the control
 * and extended keys, every reachable state with every byte, and random
 * garbage followed by a release-everything suffix.
 */
#include "test.h"

#include "../common/kbd_keymap.h"

#include <lib/kbdmap.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Feeds a byte sequence; returns how many bytes it typed, written to `out`. */
static size_t feed(struct kbdmap *k, const uint8_t *sc, size_t n, uint8_t *out)
{
    size_t typed = 0;

    for (size_t i = 0; i < n; i++)
        if (kbdmap_feed(k, sc[i], &out[typed]))
            typed++;
    return typed;
}

#define FEED(k, out, ...)                                                                          \
    feed((k), (const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}), (out))

static bool in_alphabet(uint8_t b)
{
    return (b >= 0x20 && b <= 0x7E) || b == '\t' || b == '\n' || b == 0x7F || b == 0x1B ||
           (b >= 0x01 && b <= 0x1A);
}

TEST(kbdmap_us_layout_matches_reference)
{
    struct kbdmap k = {0};
    uint8_t out[8];

    for (size_t i = 0; i < TEST_KEY_COUNT; i++) {
        const struct test_key *t = &test_keys[i];
        uint8_t expect = t->ch == '\b' ? 0x7F : (uint8_t)t->ch;
        size_t n = t->shift ? FEED(&k, out, TEST_LSHIFT_MAKE, t->make, t->make | 0x80,
                                   TEST_LSHIFT_MAKE | 0x80)
                            : FEED(&k, out, t->make, t->make | 0x80);

        if (n != 1 || out[0] != expect)
            test_fail(__FILE__, __LINE__, "key 0x%02x (shift %d): %zu byte(s), 0x%02x, want 0x%02x",
                      t->make, t->shift, n, n ? out[0] : 0, expect);
    }
    EXPECT_EQ(k.mods, 0);
    EXPECT_EQ(k.prefix, 0);
}

TEST(kbdmap_control_keys)
{
    struct kbdmap k = {0};
    uint8_t out[8];

    EXPECT_EQ(FEED(&k, out, 0x1D, 0x2E, 0xAE, 0x9D), 1); /* Ctrl-C */
    EXPECT_EQ(out[0], 0x03);
    EXPECT_EQ(FEED(&k, out, 0x1D, 0x20, 0xA0, 0x9D), 1); /* Ctrl-D */
    EXPECT_EQ(out[0], 0x04);
    EXPECT_EQ(FEED(&k, out, 0x1D, 0x16, 0x96, 0x9D), 1); /* Ctrl-U */
    EXPECT_EQ(out[0], 0x15);
    EXPECT_EQ(FEED(&k, out, 0xE0, 0x1D, 0x2E, 0xAE, 0xE0, 0x9D), 1); /* right Ctrl-C */
    EXPECT_EQ(out[0], 0x03);
    EXPECT_EQ(FEED(&k, out, 0x1D, 0x2A, 0x2E, 0xAE, 0xAA, 0x9D), 1); /* Ctrl-Shift-C */
    EXPECT_EQ(out[0], 0x03);
    /* Ctrl with a non-letter types nothing: 1, Enter, Backspace, Tab, Esc, Space, keypad. */
    EXPECT_EQ(FEED(&k, out, 0x1D, 0x02, 0x82, 0x1C, 0x9C, 0x0E, 0x8E, 0x9D), 0);
    EXPECT_EQ(FEED(&k, out, 0x1D, 0x0F, 0x8F, 0x01, 0x81, 0x39, 0xB9, 0x9D), 0);
    EXPECT_EQ(FEED(&k, out, 0x1D, 0xE0, 0x1C, 0xE0, 0x9C, 0xE0, 0x35, 0xE0, 0xB5, 0x9D), 0);
    EXPECT_EQ(FEED(&k, out, 0xE0, 0x1D, 0xE0, 0x1C, 0xE0, 0x9C, 0xE0, 0x9D), 0);
    /* Left and right Ctrl are separate: releasing one leaves the other held. */
    EXPECT_EQ(FEED(&k, out, 0x1D, 0xE0, 0x1D, 0xE0, 0x9D, 0x2E, 0xAE, 0x9D), 1);
    EXPECT_EQ(out[0], 0x03);
    EXPECT_EQ(FEED(&k, out, 0xE0, 0x1D, 0x1D, 0x9D, 0x2E, 0xAE, 0xE0, 0x9D), 1);
    EXPECT_EQ(out[0], 0x03);
    EXPECT_EQ(FEED(&k, out, 0x01, 0x81), 1); /* Esc */
    EXPECT_EQ(out[0], 0x1B);
    EXPECT_EQ(FEED(&k, out, 0xE0, 0x1C, 0xE0, 0x9C), 1); /* keypad Enter */
    EXPECT_EQ(out[0], '\n');
    EXPECT_EQ(FEED(&k, out, 0xE0, 0x35, 0xE0, 0xB5), 1); /* keypad / */
    EXPECT_EQ(out[0], '/');
    EXPECT_EQ(FEED(&k, out, 0x4F, 0xCF, 0x2A, 0x4F, 0xCF, 0xAA),
              2); /* keypad 1, with and without Shift */
    EXPECT_EQ(out[0], '1');
    EXPECT_EQ(out[1], '1');
    EXPECT_EQ(FEED(&k, out, 0x37, 0xB7), 1); /* keypad * */
    EXPECT_EQ(out[0], '*');
    EXPECT_EQ(k.mods, 0);
}

TEST(kbdmap_ignores_extended_and_lock_keys)
{
    struct kbdmap k = {0};
    uint8_t out[8];

    EXPECT_EQ(FEED(&k, out, 0xE0, 0x48, 0xE0, 0xC8, 0xE0, 0x4B, 0xE0, 0xCB), 0); /* Up, Left */
    EXPECT_EQ(FEED(&k, out, 0xE0, 0x2A, 0xE0, 0x37, 0xE0, 0xB7, 0xE0, 0xAA), 0); /* PrtSc */
    EXPECT_EQ(FEED(&k, out, 0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5), 0);             /* Pause */
    EXPECT_EQ(FEED(&k, out, 0xE1, 0x1E, 0x1F, 0x20, 0xA0), 1); /* E1 swallows exactly two bytes */
    EXPECT_EQ(out[0], 'd');
    EXPECT_EQ(FEED(&k, out, 0x3A, 0xBA, 0x45, 0xC5, 0x46, 0xC6), 0); /* the lock keys */
    EXPECT_EQ(FEED(&k, out, 0x3B, 0xBB, 0x57, 0xD7, 0x58, 0xD8), 0); /* F1, F11, F12 */
    EXPECT_EQ(FEED(&k, out, 0x38, 0xB8, 0xE0, 0x38, 0xE0, 0xB8), 0); /* both Alts */
    EXPECT_EQ(FEED(&k, out, 0x1E, 0x9E), 1);                         /* and nothing stuck: */
    EXPECT_EQ(out[0], 'a');
    EXPECT_EQ(k.mods, 0);
    EXPECT_EQ(k.prefix, 0);
}

TEST(kbdmap_shifts_and_fillers)
{
    struct kbdmap k = {0};
    uint8_t out[8];

    /* Left and right Shift are separate: releasing one leaves the other held. */
    EXPECT_EQ(FEED(&k, out, 0x2A, 0x36, 0xAA, 0x1E, 0x9E), 1);
    EXPECT_EQ(out[0], 'A');
    EXPECT_EQ(FEED(&k, out, 0xB6, 0x1E, 0x9E), 1);
    EXPECT_EQ(out[0], 'a');
    /* Releasing the right Shift leaves the left one held, too. */
    EXPECT_EQ(FEED(&k, out, 0x2A, 0x36, 0xB6, 0x1E, 0x9E, 0xAA), 1);
    EXPECT_EQ(out[0], 'A');
    /* The fake shifts around E0 keys shift nothing, with or without a real Shift held. */
    EXPECT_EQ(FEED(&k, out, 0xE0, 0x2A, 0x1E, 0x9E, 0xE0, 0xAA), 1);
    EXPECT_EQ(out[0], 'a');
    EXPECT_EQ(FEED(&k, out, 0xE0, 0x36, 0x1E, 0x9E, 0xE0, 0xB6), 1);
    EXPECT_EQ(out[0], 'a');
    /* Shift + Left, then a */
    EXPECT_EQ(FEED(&k, out, 0x2A, 0xE0, 0xAA, 0xE0, 0x4B, 0xE0, 0xCB, 0xE0, 0x2A, 0x1E, 0x9E, 0xAA),
              1);
    EXPECT_EQ(out[0], 'A');
    /* right Shift + Delete, then a */
    EXPECT_EQ(FEED(&k, out, 0x36, 0xE0, 0xB6, 0xE0, 0x53, 0xE0, 0xD3, 0xE0, 0x36, 0x1E, 0x9E, 0xB6),
              1);
    EXPECT_EQ(out[0], 'A');
    /* Shift + keypad / */
    EXPECT_EQ(FEED(&k, out, 0x2A, 0xE0, 0xAA, 0xE0, 0x35, 0xE0, 0xB5, 0xE0, 0x2A, 0xAA), 1);
    EXPECT_EQ(out[0], '/');
    EXPECT_EQ(k.mods, 0);
    /* ACK, resend and echo type nothing and change nothing. */
    EXPECT_EQ(FEED(&k, out, 0xFA, 0xFE, 0xEE, 0x2A, 0xFA, 0x1E, 0x9E, 0xAA), 1);
    EXPECT_EQ(out[0], 'A');
    /* ... nor a pending prefix: the E0 survives an ACK, and so does the E1 count. */
    EXPECT_EQ(FEED(&k, out, 0xE0, 0xFA, 0x2A, 0x1E, 0x9E, 0xE0, 0xAA), 1);
    EXPECT_EQ(out[0], 'a');
    EXPECT_EQ(FEED(&k, out, 0xE1, 0xFA, 0x1E, 0x1F, 0x20, 0xA0), 1);
    EXPECT_EQ(out[0], 'd');
    /* An error byte drops a pending prefix: the 0x1C after it is plain Enter. */
    EXPECT_EQ(FEED(&k, out, 0xE0, 0x00, 0x1C, 0x9C), 1);
    EXPECT_EQ(out[0], '\n');
    EXPECT_EQ(FEED(&k, out, 0xE1, 0xFF, 0x1E, 0x9E), 1);
    EXPECT_EQ(out[0], 'a');
}

/* With either Ctrl held, a letter types its control byte and every other key nothing. */
TEST(kbdmap_ctrl_with_every_key)
{
    for (int right = 0; right < 2; right++) {
        for (size_t i = 0; i < TEST_KEY_COUNT; i++) {
            struct kbdmap k = {0};
            const struct test_key *t = &test_keys[i];
            uint8_t out[4];
            size_t n = right ? FEED(&k, out, 0xE0, 0x1D, t->make, t->make | 0x80, 0xE0, 0x9D)
                             : FEED(&k, out, 0x1D, t->make, t->make | 0x80, 0x9D);
            bool letter = t->ch >= 'a' && t->ch <= 'z';
            bool capital = t->ch >= 'A' && t->ch <= 'Z'; /* the same key as its letter */

            if (letter && (n != 1 || out[0] != (t->ch & 0x1F)))
                test_fail(__FILE__, __LINE__, "Ctrl-%c (right %d): %zu byte(s)", t->ch, right, n);
            if (!letter && !capital && n != 0)
                test_fail(__FILE__, __LINE__, "Ctrl with key 0x%02x (right %d) typed %#x", t->make,
                          right, out[0]);
            EXPECT_EQ(k.mods, 0);
        }
    }
}

/* The keypad types its own characters, the same with either Shift. Shift-Space is a space. */
TEST(kbdmap_keypad_ignores_shift)
{
    for (size_t i = 0; i < TEST_KEYPAD_COUNT; i++) {
        const struct test_key *t = &test_keypad[i];
        uint8_t make = t->make, brk = t->make | 0x80;
        struct kbdmap k = {0};
        uint8_t out[8];

        if (FEED(&k, out, make, brk, 0x2A, make, brk, 0xAA, 0x36, make, brk, 0xB6) != 3 ||
            out[0] != (uint8_t)t->ch || out[1] != (uint8_t)t->ch || out[2] != (uint8_t)t->ch)
            test_fail(__FILE__, __LINE__, "keypad 0x%02x does not type '%c' three times", make,
                      t->ch);
    }
    struct kbdmap k = {0};
    uint8_t out[4];

    EXPECT_EQ(FEED(&k, out, 0x2A, 0x39, 0xB9, 0xAA), 1);
    EXPECT_EQ(out[0], ' ');
}

/* Every reachable state, every byte: at most one byte out, always in the alphabet, state valid. */
TEST(kbdmap_every_state_and_byte)
{
    static const uint8_t prefixes[] = {0, 0xE0, 1, 2};

    for (unsigned mods = 0; mods < 16; mods++) {
        for (size_t p = 0; p < sizeof(prefixes); p++) {
            for (unsigned b = 0; b < 256; b++) {
                struct kbdmap k = {.mods = (uint8_t)mods, .prefix = prefixes[p]};
                uint8_t out = 0xAB;

                if (kbdmap_feed(&k, (uint8_t)b, &out) && !in_alphabet(out))
                    test_fail(__FILE__, __LINE__, "mods %u prefix %#x byte %#x typed %#x", mods,
                              prefixes[p], b, out);
                if (k.mods > 15 || (k.prefix != 0 && k.prefix != 0xE0 && k.prefix > 2))
                    test_fail(__FILE__, __LINE__, "mods %u prefix %#x byte %#x left %#x/%#x", mods,
                              prefixes[p], b, k.mods, k.prefix);
            }
        }
    }
}

/* Whatever came before, an error byte and every release resets the decoder. */
TEST(kbdmap_recovers_from_random_input)
{
    static const uint8_t suffix[] = {0x00, 0xAA, 0xB6, 0x9D, 0xE0, 0x9D, 0x1E, 0x9E};
    uint8_t junk[32], out[64];

    srand(1);
    for (int i = 0; i < 10000; i++) {
        struct kbdmap k = {0};
        size_t n = (size_t)(rand() % (int)sizeof(junk));

        for (size_t j = 0; j < n; j++)
            junk[j] = (uint8_t)rand();
        feed(&k, junk, n, out);
        size_t typed = feed(&k, suffix, sizeof(suffix), out);

        if (typed != 1 || out[0] != 'a') {
            test_fail(__FILE__, __LINE__, "after %zu random bytes: %zu typed", n, typed);
            return;
        }
    }
}
