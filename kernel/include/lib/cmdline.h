/* SPDX-License-Identifier: MIT */
/*
 * Kernel command line parsing.
 *
 * The command line is a space-separated list of options, each either a bare
 * flag ("ktest") or a key=value pair ("loglevel=debug").
 */
#ifndef MINIOS_LIB_CMDLINE_H
#define MINIOS_LIB_CMDLINE_H

#include <types.h>

/* True if `flag` appears as a bare option (not as the key of key=value). */
bool cmdline_has_flag(const char *cmdline, const char *flag);

/*
 * Looks up `key` and copies its value into `buf` (NUL-terminated, truncated
 * to `size`). Returns false if the key is absent.
 */
bool cmdline_get(const char *cmdline, const char *key, char *buf, usize size);

#endif /* MINIOS_LIB_CMDLINE_H */
