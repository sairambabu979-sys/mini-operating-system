/* SPDX-License-Identifier: MIT */
/*
 * Leveled kernel logging.
 *
 * Files set a prefix by defining pr_fmt before including this header:
 *
 *     #define pr_fmt(fmt) "pmm: " fmt
 *     #include <core/klog.h>
 *
 * Messages above the current level (set with loglevel= on the command
 * line) are dropped.
 */
#ifndef MINIOS_CORE_KLOG_H
#define MINIOS_CORE_KLOG_H

#include <compiler.h>
#include <types.h>

enum log_level {
    LOG_ERR = 0,
    LOG_WARN = 1,
    LOG_INFO = 2,
    LOG_DEBUG = 3,
};

#ifndef pr_fmt
#define pr_fmt(fmt) fmt
#endif

void klog(enum log_level level, const char *fmt, ...) __printf(2, 3);
void klog_set_level(enum log_level level);

/* Parses "err", "warn", "info" or "debug". Returns false if unrecognized. */
bool klog_parse_level(const char *name, enum log_level *out);

#define pr_err(fmt, ...)   klog(LOG_ERR, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warn(fmt, ...)  klog(LOG_WARN, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_info(fmt, ...)  klog(LOG_INFO, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_debug(fmt, ...) klog(LOG_DEBUG, pr_fmt(fmt), ##__VA_ARGS__)

#endif /* MINIOS_CORE_KLOG_H */
