// SPDX-License-Identifier: MIT
/*
 * The console. See <core/printk.h>.
 *
 * Every byte goes to the log ring and to COM1 at once, under the console
 * lock. The screen is fed from the ring afterwards, outside that lock:
 *
 *   - Drawing is slow (a scroll can redraw a whole framebuffer), and the
 *     console lock keeps interrupts off. Ticks missed while drawing would
 *     be ticks the clock never counts.
 *   - So once interrupts are live, a message printed with interrupts off
 *     (an IRQ handler, code holding a spinlock) is only logged; whoever
 *     next prints with interrupts on, or the idle loop, draws it.
 *   - One context feeds the screen at a time, with preemption off: a
 *     feeder switched away from mid-draw would hold the screen for as
 *     long as other tasks kept running. Another context that finds it
 *     busy leaves its bytes to the feeder, which loops until it has
 *     caught up, so nothing is dropped and nothing waits.
 *   - A "console" kernel thread drains every 50 ms, so output deferred
 *     from interrupt context reaches the screen even when no task ever
 *     prints and the CPU never idles.
 *   - During boot (before interrupts) and in a panic, drawing is
 *     immediate: that is when the screen matters most.
 *
 * The ring is also what lets the screen show the boot from its first line:
 * the screen needs the memory manager, so it registers after the first
 * messages and is fed them from the ring.
 */
#include <arch/irqflags.h>
#include <core/panic.h>
#include <core/preempt.h>
#include <core/printk.h>
#include <core/spinlock.h>
#include <drivers/serial.h>
#include <lib/printf.h>
#include <sched/sched.h>

#define LOG_SIZE 16384ull /* 16 KiB */

/*
 * Serializes whole messages so lines from IRQ context never interleave
 * mid-line. Bypassed once a panic starts: the panicking context may be the
 * one holding it.
 */
static struct spinlock console_lock = SPINLOCK_INIT("console");
static char log_ring[LOG_SIZE];
static u64 log_bytes; /* ever written; the ring holds the last LOG_SIZE */
static const struct console_screen *screen;
static u64 screen_put;     /* log bytes handed to the screen, one by one */
static bool screen_busy;   /* a context is feeding it right now */
static bool panic_drawing; /* a panic is drawing (a fault in there must not recurse) */
static bool live;          /* interrupts run: defer drawing when they are off */

static void emit(char c)
{
    log_ring[log_bytes++ % LOG_SIZE] = c;
    serial_putc(c);
}

static void console_putc(char c, void *ctx)
{
    (void)ctx;
    emit(c);
}

/*
 * Hands the screen the ring from screen_put up to `end`, then lets it draw.
 * The ring is read outside the lock. Writers append past `end` and so
 * overwrite a slot here only after LOG_SIZE more bytes: with preemption
 * off, that would take interrupt handlers logging 16 KiB during one feed.
 */
static void feed(const struct console_screen *s, u64 end)
{
    if (end - screen_put > LOG_SIZE)
        screen_put = end - LOG_SIZE; /* overrun while deferred: the oldest bytes are gone */
    while (screen_put < end) {
        s->putc(log_ring[screen_put % LOG_SIZE]);
        screen_put++; /* per byte: a panic resumes exactly here */
    }
    if (s->flush)
        s->flush();
}

/* Catches the screen up with the log, unless another context is already doing so. */
static void feed_screen(void)
{
    for (;;) {
        irqflags_t flags = spin_lock_irqsave(&console_lock);
        const struct console_screen *s = screen;
        u64 end = log_bytes;

        if (!s || screen_busy || screen_put == end) {
            spin_unlock_irqrestore(&console_lock, flags);
            return;
        }
        screen_busy = true;
        preempt_disable(); /* before the lock drops: no window to be switched out */
        spin_unlock_irqrestore(&console_lock, flags);

        feed(s, end);

        flags = spin_lock_irqsave(&console_lock);
        screen_busy = false;
        spin_unlock_irqrestore(&console_lock, flags);
        preempt_enable();
    }
}

/* After a message: draw now if this context may, else leave it to one that can. */
static void message_done(void)
{
    if (panic_in_progress()) {
        /*
         * No locks, no deferral. An interrupted feeder never resumes, and
         * screen_put says how far it got, so nothing is drawn twice. If
         * drawing itself faults, the nested panic stays on COM1.
         */
        if (screen && !panic_drawing) {
            panic_drawing = true;
            feed(screen, log_bytes);
            panic_drawing = false;
        }
        return;
    }
    if (!live || arch_irqs_enabled())
        feed_screen();
}

int kvprintf(const char *fmt, va_list ap)
{
    int n;

    if (panic_in_progress()) {
        n = k_vformat(console_putc, NULL, fmt, ap);
    } else {
        irqflags_t flags = spin_lock_irqsave(&console_lock);

        n = k_vformat(console_putc, NULL, fmt, ap);
        spin_unlock_irqrestore(&console_lock, flags);
    }
    message_done();
    return n;
}

void console_write(const char *s, usize len)
{
    if (panic_in_progress()) {
        for (usize i = 0; i < len; i++)
            emit(s[i]);
    } else {
        irqflags_t flags = spin_lock_irqsave(&console_lock);

        for (usize i = 0; i < len; i++)
            emit(s[i]);
        spin_unlock_irqrestore(&console_lock, flags);
    }
    message_done();
}

int kprintf(const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = kvprintf(fmt, ap);
    va_end(ap);
    return n;
}

void console_register_screen(const struct console_screen *s)
{
    irqflags_t flags = spin_lock_irqsave(&console_lock);
    u64 start = log_bytes > LOG_SIZE ? log_bytes - LOG_SIZE : 0;

    /* A ring that wrapped starts mid-line: begin at the next full one. */
    if (start > 0) {
        while (start < log_bytes && log_ring[start % LOG_SIZE] != '\n')
            start++;
        start++; /* past the newline */
    }
    screen_put = start;
    screen = s;
    spin_unlock_irqrestore(&console_lock, flags);
    message_done(); /* the replay */
}

void console_go_live(void)
{
    live = true;
}

bool console_is_live(void)
{
    return live;
}

void console_drain(void)
{
    if (!panic_in_progress())
        feed_screen();
}

static int console_thread(void *arg)
{
    (void)arg;
    for (;;) {
        sleep_ms(50);
        console_drain();
    }
    return 0;
}

void console_start_thread(void)
{
    if (screen && !kthread_create(console_thread, NULL, "console", KTHREAD_DETACHED))
        kprintf("WARNING: console: no drain thread; deferred output waits for idle\n");
}

const struct console_screen *console_screen(void)
{
    return screen;
}

usize console_log_read(char *buf, usize len)
{
    irqflags_t flags = spin_lock_irqsave(&console_lock);
    u64 avail = log_bytes < LOG_SIZE ? log_bytes : LOG_SIZE;
    usize n = len < avail ? len : (usize)avail;

    for (usize i = 0; i < n; i++)
        buf[i] = log_ring[(log_bytes - n + i) % LOG_SIZE];
    spin_unlock_irqrestore(&console_lock, flags);
    return n;
}
