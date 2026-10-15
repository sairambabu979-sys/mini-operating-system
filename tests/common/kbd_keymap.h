/* SPDX-License-Identifier: MIT */
/*
 * The US keyboard as a typist sees it: every character a key types, the
 * scan code set 1 make code of that key, and whether Shift is held.
 *
 * Shared by the host tests of the decoder and by the KTESTs that type
 * through the real 8042, and written down independently of the kernel's
 * own tables (transcribed from QEMU's qcode-to-set-1 mapping, checked
 * against a running guest), so a wrong entry in the kernel cannot hide
 * behind the same entry here. Plain C types: both builds include it.
 */
#ifndef MINIOS_TESTS_COMMON_KBD_KEYMAP_H
#define MINIOS_TESTS_COMMON_KBD_KEYMAP_H

struct test_key {
    char ch;            /* what the key types; '\b' stands for Backspace, which types 0x7F */
    unsigned char make; /* its set 1 make code; the break code is make | 0x80 */
    unsigned char shift;
};

#define TEST_LSHIFT_MAKE 0x2A

static const struct test_key test_keys[] = {
    {' ', 0x39, 0},  {'!', 0x02, 1},  {'"', 0x28, 1},  {'#', 0x04, 1}, {'$', 0x05, 1},
    {'%', 0x06, 1},  {'&', 0x08, 1},  {'\'', 0x28, 0}, {'(', 0x0A, 1}, {')', 0x0B, 1},
    {'*', 0x09, 1},  {'+', 0x0D, 1},  {',', 0x33, 0},  {'-', 0x0C, 0}, {'.', 0x34, 0},
    {'/', 0x35, 0},  {'0', 0x0B, 0},  {'1', 0x02, 0},  {'2', 0x03, 0}, {'3', 0x04, 0},
    {'4', 0x05, 0},  {'5', 0x06, 0},  {'6', 0x07, 0},  {'7', 0x08, 0}, {'8', 0x09, 0},
    {'9', 0x0A, 0},  {':', 0x27, 1},  {';', 0x27, 0},  {'<', 0x33, 1}, {'=', 0x0D, 0},
    {'>', 0x34, 1},  {'?', 0x35, 1},  {'@', 0x03, 1},  {'A', 0x1E, 1}, {'B', 0x30, 1},
    {'C', 0x2E, 1},  {'D', 0x20, 1},  {'E', 0x12, 1},  {'F', 0x21, 1}, {'G', 0x22, 1},
    {'H', 0x23, 1},  {'I', 0x17, 1},  {'J', 0x24, 1},  {'K', 0x25, 1}, {'L', 0x26, 1},
    {'M', 0x32, 1},  {'N', 0x31, 1},  {'O', 0x18, 1},  {'P', 0x19, 1}, {'Q', 0x10, 1},
    {'R', 0x13, 1},  {'S', 0x1F, 1},  {'T', 0x14, 1},  {'U', 0x16, 1}, {'V', 0x2F, 1},
    {'W', 0x11, 1},  {'X', 0x2D, 1},  {'Y', 0x15, 1},  {'Z', 0x2C, 1}, {'[', 0x1A, 0},
    {'\\', 0x2B, 0}, {']', 0x1B, 0},  {'^', 0x07, 1},  {'_', 0x0C, 1}, {'`', 0x29, 0},
    {'a', 0x1E, 0},  {'b', 0x30, 0},  {'c', 0x2E, 0},  {'d', 0x20, 0}, {'e', 0x12, 0},
    {'f', 0x21, 0},  {'g', 0x22, 0},  {'h', 0x23, 0},  {'i', 0x17, 0}, {'j', 0x24, 0},
    {'k', 0x25, 0},  {'l', 0x26, 0},  {'m', 0x32, 0},  {'n', 0x31, 0}, {'o', 0x18, 0},
    {'p', 0x19, 0},  {'q', 0x10, 0},  {'r', 0x13, 0},  {'s', 0x1F, 0}, {'t', 0x14, 0},
    {'u', 0x16, 0},  {'v', 0x2F, 0},  {'w', 0x11, 0},  {'x', 0x2D, 0}, {'y', 0x15, 0},
    {'z', 0x2C, 0},  {'{', 0x1A, 1},  {'|', 0x2B, 1},  {'}', 0x1B, 1}, {'~', 0x29, 1},
    {'\n', 0x1C, 0}, {'\t', 0x0F, 0}, {'\b', 0x0E, 0},
};

#define TEST_KEY_COUNT (sizeof(test_keys) / sizeof(test_keys[0]))

/*
 * The keypad, which types the same with or without Shift (no Num Lock
 * state): QEMU's kp_* qcodes, also set 1 make codes. Keypad Enter and
 * keypad '/' are E0 keys and tested as such.
 */
static const struct test_key test_keypad[] = {
    {'*', 0x37, 0}, {'7', 0x47, 0}, {'8', 0x48, 0}, {'9', 0x49, 0}, {'-', 0x4A, 0},
    {'4', 0x4B, 0}, {'5', 0x4C, 0}, {'6', 0x4D, 0}, {'+', 0x4E, 0}, {'1', 0x4F, 0},
    {'2', 0x50, 0}, {'3', 0x51, 0}, {'0', 0x52, 0}, {'.', 0x53, 0},
};

#define TEST_KEYPAD_COUNT (sizeof(test_keypad) / sizeof(test_keypad[0]))

#endif /* MINIOS_TESTS_COMMON_KBD_KEYMAP_H */
