// SPDX-License-Identifier: MIT
/*
 * Freestanding printf engine. See <lib/printf.h> for the supported subset.
 *
 * The engine is sink-based: k_vformat() pushes characters to a callback, so
 * the same code backs the serial console, the log ring buffer and
 * k_snprintf(). It is unit-tested on the host against glibc's snprintf.
 */
#include <lib/printf.h>
#include <lib/string.h>

enum fmt_flags {
    F_LEFT = 1u << 0,  /* '-' */
    F_PLUS = 1u << 1,  /* '+' */
    F_SPACE = 1u << 2, /* ' ' */
    F_ALT = 1u << 3,   /* '#' */
    F_ZERO = 1u << 4,  /* '0' */
    F_UPPER = 1u << 5, /* %X */
};

enum fmt_length {
    LEN_INT,
    LEN_CHAR,
    LEN_SHORT,
    LEN_LONG,
    LEN_LLONG,
    LEN_SIZE,
    LEN_PTRDIFF,
    LEN_MAX
};

struct fmt_spec {
    unsigned flags;
    int width;     /* minimum field width, 0 if none */
    int precision; /* -1 if none */
    enum fmt_length length;
};

struct sink {
    fmt_putc_fn out;
    void *ctx;
    int count;
};

static void emit(struct sink *s, char c)
{
    s->out(c, s->ctx);
    s->count++;
}

static void emit_repeat(struct sink *s, char c, int n)
{
    while (n-- > 0)
        emit(s, c);
}

static void format_integer(struct sink *s, u64 value, bool negative, unsigned base,
                           const struct fmt_spec *spec)
{
    const char *digit_chars = (spec->flags & F_UPPER) ? "0123456789ABCDEF" : "0123456789abcdef";
    char digits[24]; /* 2^64 in octal is 22 digits */
    char prefix[3];
    int ndigits = 0;
    int nprefix = 0;
    int precision = spec->precision;
    u64 v = value;

    /* C17: a zero value with zero precision produces no digits. */
    if (!(value == 0 && precision == 0)) {
        do {
            digits[ndigits++] = digit_chars[v % base];
            v /= base;
        } while (v != 0);
    }

    if (negative)
        prefix[nprefix++] = '-';
    else if (spec->flags & F_PLUS)
        prefix[nprefix++] = '+';
    else if (spec->flags & F_SPACE)
        prefix[nprefix++] = ' ';

    if ((spec->flags & F_ALT) && base == 16 && value != 0) {
        prefix[nprefix++] = '0';
        prefix[nprefix++] = (spec->flags & F_UPPER) ? 'X' : 'x';
    }
    /* '#' with octal: increase precision so the first digit is a zero. */
    if ((spec->flags & F_ALT) && base == 8 && (ndigits == 0 || digits[ndigits - 1] != '0')) {
        if (precision <= ndigits)
            precision = ndigits + 1;
    }

    int zeros = precision > ndigits ? precision - ndigits : 0;
    int total = nprefix + zeros + ndigits;
    int pad = spec->width > total ? spec->width - total : 0;

    /* '0' is ignored when '-' is present or a precision is given. */
    if (!(spec->flags & F_LEFT) && (spec->flags & F_ZERO) && spec->precision < 0) {
        zeros += pad;
        pad = 0;
    }

    if (!(spec->flags & F_LEFT))
        emit_repeat(s, ' ', pad);
    for (int i = 0; i < nprefix; i++)
        emit(s, prefix[i]);
    emit_repeat(s, '0', zeros);
    while (ndigits > 0)
        emit(s, digits[--ndigits]);
    if (spec->flags & F_LEFT)
        emit_repeat(s, ' ', pad);
}

static void format_string(struct sink *s, const char *str, const struct fmt_spec *spec)
{
    if (str == NULL)
        str = "(null)";

    int len = (int)(spec->precision >= 0 ? strnlen(str, (usize)spec->precision) : strlen(str));
    int pad = spec->width > len ? spec->width - len : 0;

    if (!(spec->flags & F_LEFT))
        emit_repeat(s, ' ', pad);
    for (int i = 0; i < len; i++)
        emit(s, str[i]);
    if (spec->flags & F_LEFT)
        emit_repeat(s, ' ', pad);
}

/*
 * size_t, ptrdiff_t and intmax_t alias `long` on LP64, but they are distinct C
 * types and each must be fetched as itself to stay correct on other ABIs.
 */
// NOLINTBEGIN(bugprone-branch-clone)
static s64 fetch_signed(va_list *ap, enum fmt_length len)
{
    switch (len) {
    case LEN_CHAR:
        return (signed char)va_arg(*ap, int);
    case LEN_SHORT:
        return (short)va_arg(*ap, int);
    case LEN_LONG:
        return va_arg(*ap, long);
    case LEN_LLONG:
        return va_arg(*ap, long long);
    case LEN_SIZE:
        return va_arg(*ap, sptr); /* signed counterpart of size_t */
    case LEN_PTRDIFF:
        return va_arg(*ap, ptrdiff_t);
    case LEN_MAX:
        return va_arg(*ap, intmax_t);
    case LEN_INT:
    default:
        return va_arg(*ap, int);
    }
}

static u64 fetch_unsigned(va_list *ap, enum fmt_length len)
{
    switch (len) {
    case LEN_CHAR:
        return (unsigned char)va_arg(*ap, unsigned int);
    case LEN_SHORT:
        return (unsigned short)va_arg(*ap, unsigned int);
    case LEN_LONG:
        return va_arg(*ap, unsigned long);
    case LEN_LLONG:
        return va_arg(*ap, unsigned long long);
    case LEN_SIZE:
        return va_arg(*ap, usize);
    case LEN_PTRDIFF:
        return (u64)va_arg(*ap, ptrdiff_t);
    case LEN_MAX:
        return va_arg(*ap, uintmax_t);
    case LEN_INT:
    default:
        return va_arg(*ap, unsigned int);
    }
}
// NOLINTEND(bugprone-branch-clone)

static unsigned unsigned_base(char conv)
{
    switch (conv) {
    case 'o':
        return 8;
    case 'x':
    case 'X':
        return 16;
    default:
        return 10;
    }
}

static const char *parse_spec(const char *p, struct fmt_spec *spec, va_list *ap)
{
    spec->flags = 0;
    spec->width = 0;
    spec->precision = -1;
    spec->length = LEN_INT;

    for (;; p++) {
        if (*p == '-')
            spec->flags |= F_LEFT;
        else if (*p == '+')
            spec->flags |= F_PLUS;
        else if (*p == ' ')
            spec->flags |= F_SPACE;
        else if (*p == '#')
            spec->flags |= F_ALT;
        else if (*p == '0')
            spec->flags |= F_ZERO;
        else
            break;
    }

    if (*p == '*') {
        int w = va_arg(*ap, int);

        /* A negative '*' width means '-' flag plus a positive width. */
        if (w < 0) {
            spec->flags |= F_LEFT;
            w = -w;
        }
        spec->width = w;
        p++;
    } else {
        while (*p >= '0' && *p <= '9')
            spec->width = spec->width * 10 + (*p++ - '0');
    }

    if (*p == '.') {
        p++;
        if (*p == '*') {
            int prec = va_arg(*ap, int);

            spec->precision = prec < 0 ? -1 : prec; /* negative = no precision */
            p++;
        } else {
            spec->precision = 0;
            while (*p >= '0' && *p <= '9')
                spec->precision = spec->precision * 10 + (*p++ - '0');
        }
    }

    switch (*p) {
    case 'h':
        p++;
        spec->length = LEN_SHORT;
        if (*p == 'h') {
            p++;
            spec->length = LEN_CHAR;
        }
        break;
    case 'l':
        p++;
        spec->length = LEN_LONG;
        if (*p == 'l') {
            p++;
            spec->length = LEN_LLONG;
        }
        break;
    case 'z':
        p++;
        spec->length = LEN_SIZE;
        break;
    case 't':
        p++;
        spec->length = LEN_PTRDIFF;
        break;
    case 'j':
        p++;
        spec->length = LEN_MAX;
        break;
    default:
        break;
    }
    return p;
}

int k_vformat(fmt_putc_fn out, void *ctx, const char *fmt, va_list ap)
{
    struct sink s = {.out = out, .ctx = ctx, .count = 0};
    struct fmt_spec spec;
    va_list args;

    /* va_copy so the helpers can take a va_list* portably. */
    va_copy(args, ap);

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            emit(&s, *p);
            continue;
        }

        const char *start = p;

        p = parse_spec(p + 1, &spec, &args);

        switch (*p) {
        case 'd':
        case 'i': {
            s64 v = fetch_signed(&args, spec.length);
            u64 mag = v < 0 ? (u64)0 - (u64)v : (u64)v;

            format_integer(&s, mag, v < 0, 10, &spec);
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X':
            /* '+' and ' ' only apply to signed conversions (C17 7.21.6.1p6). */
            spec.flags &= ~(unsigned)(F_PLUS | F_SPACE);
            if (*p == 'X')
                spec.flags |= F_UPPER;
            format_integer(&s, fetch_unsigned(&args, spec.length), false, unsigned_base(*p), &spec);
            break;
        case 'p': {
            struct fmt_spec ps = {.flags = F_ZERO, .width = 16, .precision = -1, .length = LEN_INT};

            emit(&s, '0');
            emit(&s, 'x');
            format_integer(&s, (u64)(uptr)va_arg(args, void *), false, 16, &ps);
            break;
        }
        case 'c': {
            /* Exactly one byte is emitted, even if it is NUL. */
            char c = (char)va_arg(args, int);

            if (!(spec.flags & F_LEFT))
                emit_repeat(&s, ' ', spec.width - 1);
            emit(&s, c);
            if (spec.flags & F_LEFT)
                emit_repeat(&s, ' ', spec.width - 1);
            break;
        }
        case 's':
            format_string(&s, va_arg(args, const char *), &spec);
            break;
        case '%':
            emit(&s, '%');
            break;
        case '\0':
            /* Truncated specifier at end of string: print it verbatim. */
            for (const char *q = start; q < p; q++)
                emit(&s, *q);
            p--;
            break;
        default:
            /* Unknown conversion: print it verbatim so bugs are visible. */
            for (const char *q = start; q <= p; q++)
                emit(&s, *q);
            break;
        }
    }

    va_end(args);
    return s.count;
}

struct buf_ctx {
    char *buf;
    usize size;
    usize pos;
};

static void buf_putc(char c, void *opaque)
{
    struct buf_ctx *b = opaque;

    if (b->pos + 1 < b->size)
        b->buf[b->pos] = c;
    b->pos++;
}

int k_vsnprintf(char *buf, usize size, const char *fmt, va_list ap)
{
    struct buf_ctx b = {.buf = buf, .size = size, .pos = 0};
    int n = k_vformat(buf_putc, &b, fmt, ap);

    if (size > 0)
        buf[b.pos < size ? b.pos : size - 1] = '\0';
    return n;
}

int k_snprintf(char *buf, usize size, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = k_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
