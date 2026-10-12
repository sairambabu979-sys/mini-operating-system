// SPDX-License-Identifier: MIT
/*
 * Kernel command line parsing. See <lib/cmdline.h>.
 */
#include <lib/cmdline.h>
#include <lib/string.h>

/*
 * Iterates over space-separated tokens. On return *len holds the token
 * length; returns NULL when no tokens remain.
 */
static const char *next_token(const char *p, usize *len)
{
    while (*p == ' ')
        p++;
    if (*p == '\0')
        return NULL;

    const char *start = p;

    while (*p != '\0' && *p != ' ')
        p++;
    *len = (usize)(p - start);
    return start;
}

bool cmdline_has_flag(const char *cmdline, const char *flag)
{
    usize flag_len = strlen(flag);
    usize len;

    for (const char *t = next_token(cmdline, &len); t; t = next_token(t + len, &len)) {
        if (len == flag_len && strncmp(t, flag, flag_len) == 0)
            return true;
    }
    return false;
}

bool cmdline_get(const char *cmdline, const char *key, char *buf, usize size)
{
    usize key_len = strlen(key);
    usize len;

    for (const char *t = next_token(cmdline, &len); t; t = next_token(t + len, &len)) {
        if (len > key_len && t[key_len] == '=' && strncmp(t, key, key_len) == 0) {
            usize vlen = len - key_len - 1;

            if (size > 0) {
                usize n = vlen < size - 1 ? vlen : size - 1;

                memcpy(buf, t + key_len + 1, n);
                buf[n] = '\0';
            }
            return true;
        }
    }
    return false;
}
