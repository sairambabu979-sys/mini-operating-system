// SPDX-License-Identifier: MIT
/*
 * Kernel UndefinedBehaviorSanitizer runtime (debug builds).
 *
 * The compiler instruments every operation with defined failure modes
 * (overflow, shifts, bounds, misalignment, null deref, invalid bool/enum
 * loads...) and calls these handlers when one goes wrong. Undefined
 * behaviour in a kernel is always a bug, so the default response is panic.
 *
 * The data layouts mirror compiler-rt's ubsan_handlers.h. This file itself
 * is built with -fno-sanitize=all (see the Makefile) to avoid recursion.
 */
#include <core/panic.h>
#include <core/printk.h>
#include <core/ubsan.h>

struct source_location {
    const char *file;
    u32 line;
    u32 column;
};

struct type_descriptor {
    u16 kind; /* 0: integer, 1: float */
    u16 info; /* integer: bit 0 = signed, bits 1+ = log2(width) */
    char name[];
};

struct overflow_data {
    struct source_location loc;
    const struct type_descriptor *type;
};

struct shift_data {
    struct source_location loc;
    const struct type_descriptor *lhs_type;
    const struct type_descriptor *rhs_type;
};

struct out_of_bounds_data {
    struct source_location loc;
    const struct type_descriptor *array_type;
    const struct type_descriptor *index_type;
};

struct type_mismatch_data_v1 {
    struct source_location loc;
    const struct type_descriptor *type;
    u8 log_alignment;
    u8 type_check_kind;
};

struct invalid_value_data {
    struct source_location loc;
    const struct type_descriptor *type;
};

struct unreachable_data {
    struct source_location loc;
};

struct nonnull_arg_data {
    struct source_location loc;
    struct source_location attr_loc;
    int arg_index;
};

struct nonnull_return_data {
    struct source_location attr_loc;
};

struct pointer_overflow_data {
    struct source_location loc;
};

struct vla_bound_data {
    struct source_location loc;
    const struct type_descriptor *type;
};

struct invalid_builtin_data {
    struct source_location loc;
    u8 kind;
};

struct alignment_assumption_data {
    struct source_location loc;
    struct source_location assumption_loc;
    const struct type_descriptor *type;
};

static struct ubsan_expectation *expectation;

void ubsan_expect(struct ubsan_expectation *e)
{
    *e = (struct ubsan_expectation){.armed = true};
    expectation = e;
}

void ubsan_expect_clear(void)
{
    expectation = NULL;
}

static void report(const struct source_location *loc, const char *what)
{
    if (expectation && expectation->armed) {
        expectation->armed = false;
        expectation->hits++;
        expectation->kind = what;
        expectation->line = loc->line;
        return;
    }
    panic("UBSAN: %s at %s:%u:%u", what, loc->file, loc->line, loc->column);
}

/* Each handler is reached only from compiler-generated calls. */
#define UBSAN_HANDLER(name, ...) void __ubsan_handle_##name(__VA_ARGS__);

UBSAN_HANDLER(add_overflow, struct overflow_data *d, void *lhs, void *rhs)
UBSAN_HANDLER(sub_overflow, struct overflow_data *d, void *lhs, void *rhs)
UBSAN_HANDLER(mul_overflow, struct overflow_data *d, void *lhs, void *rhs)
UBSAN_HANDLER(negate_overflow, struct overflow_data *d, void *old)
UBSAN_HANDLER(divrem_overflow, struct overflow_data *d, void *lhs, void *rhs)
UBSAN_HANDLER(shift_out_of_bounds, struct shift_data *d, void *lhs, void *rhs)
UBSAN_HANDLER(out_of_bounds, struct out_of_bounds_data *d, void *index)
UBSAN_HANDLER(type_mismatch_v1, struct type_mismatch_data_v1 *d, void *ptr)
UBSAN_HANDLER(load_invalid_value, struct invalid_value_data *d, void *val)
UBSAN_HANDLER(builtin_unreachable, struct unreachable_data *d)
UBSAN_HANDLER(nonnull_arg, struct nonnull_arg_data *d)
UBSAN_HANDLER(nonnull_return_v1, struct nonnull_return_data *d, struct source_location *loc)
UBSAN_HANDLER(pointer_overflow, struct pointer_overflow_data *d, void *base, void *result)
UBSAN_HANDLER(vla_bound_not_positive, struct vla_bound_data *d, void *bound)
UBSAN_HANDLER(invalid_builtin, struct invalid_builtin_data *d)
UBSAN_HANDLER(alignment_assumption, struct alignment_assumption_data *d, void *ptr, void *align,
              void *offset)

void __ubsan_handle_add_overflow(struct overflow_data *d, void *lhs, void *rhs)
{
    (void)lhs;
    (void)rhs;
    report(&d->loc, "integer overflow in addition");
}

void __ubsan_handle_sub_overflow(struct overflow_data *d, void *lhs, void *rhs)
{
    (void)lhs;
    (void)rhs;
    report(&d->loc, "integer overflow in subtraction");
}

void __ubsan_handle_mul_overflow(struct overflow_data *d, void *lhs, void *rhs)
{
    (void)lhs;
    (void)rhs;
    report(&d->loc, "integer overflow in multiplication");
}

void __ubsan_handle_negate_overflow(struct overflow_data *d, void *old)
{
    (void)old;
    report(&d->loc, "integer overflow in negation");
}

void __ubsan_handle_divrem_overflow(struct overflow_data *d, void *lhs, void *rhs)
{
    (void)lhs;
    (void)rhs;
    report(&d->loc, "division overflow or division by zero");
}

void __ubsan_handle_shift_out_of_bounds(struct shift_data *d, void *lhs, void *rhs)
{
    (void)lhs;
    (void)rhs;
    report(&d->loc, "shift out of bounds");
}

void __ubsan_handle_out_of_bounds(struct out_of_bounds_data *d, void *index)
{
    (void)index;
    report(&d->loc, "array index out of bounds");
}

void __ubsan_handle_type_mismatch_v1(struct type_mismatch_data_v1 *d, void *ptr)
{
    uptr alignment = (uptr)1 << d->log_alignment;

    if (ptr == NULL)
        report(&d->loc, "null pointer dereference");
    else if (alignment > 1 && ((uptr)ptr & (alignment - 1)) != 0)
        report(&d->loc, "misaligned access");
    else
        report(&d->loc, "access to object of insufficient size");
}

void __ubsan_handle_load_invalid_value(struct invalid_value_data *d, void *val)
{
    (void)val;
    report(&d->loc, "load of invalid value (bool or enum)");
}

void __ubsan_handle_builtin_unreachable(struct unreachable_data *d)
{
    report(&d->loc, "__builtin_unreachable() reached");
    panic("UBSAN: execution continued past __builtin_unreachable()");
}

void __ubsan_handle_nonnull_arg(struct nonnull_arg_data *d)
{
    report(&d->loc, "null passed as a nonnull argument");
}

void __ubsan_handle_nonnull_return_v1(struct nonnull_return_data *d, struct source_location *loc)
{
    (void)d;
    report(loc, "null returned from a returns_nonnull function");
}

void __ubsan_handle_pointer_overflow(struct pointer_overflow_data *d, void *base, void *result)
{
    (void)base;
    (void)result;
    report(&d->loc, "pointer arithmetic overflow");
}

void __ubsan_handle_vla_bound_not_positive(struct vla_bound_data *d, void *bound)
{
    (void)bound;
    report(&d->loc, "variable-length array bound not positive");
}

void __ubsan_handle_invalid_builtin(struct invalid_builtin_data *d)
{
    report(&d->loc, "invalid argument to builtin (e.g. __builtin_clz(0))");
}

void __ubsan_handle_alignment_assumption(struct alignment_assumption_data *d, void *ptr,
                                         void *align, void *offset)
{
    (void)ptr;
    (void)align;
    (void)offset;
    report(&d->loc, "alignment assumption violated");
}
