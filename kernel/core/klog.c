// SPDX-License-Identifier: MIT
#include <core/klog.h>
#include <core/printk.h>
#include <lib/string.h>

static enum log_level current_level = LOG_INFO;

static const char *const level_tags[] = {
    [LOG_ERR] = "ERROR: ",
    [LOG_WARN] = "WARNING: ",
    [LOG_INFO] = "",
    [LOG_DEBUG] = "debug: ",
};

static const char *const level_names[] = {
    [LOG_ERR] = "err",
    [LOG_WARN] = "warn",
    [LOG_INFO] = "info",
    [LOG_DEBUG] = "debug",
};

void klog_set_level(enum log_level level)
{
    current_level = level;
}

bool klog_parse_level(const char *name, enum log_level *out)
{
    for (usize i = 0; i < ARRAY_SIZE(level_names); i++) {
        if (strcmp(name, level_names[i]) == 0) {
            *out = (enum log_level)i;
            return true;
        }
    }
    return false;
}

void klog(enum log_level level, const char *fmt, ...)
{
    va_list ap;

    if (level > current_level)
        return;
    kprintf("%s", level_tags[level]);
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}
