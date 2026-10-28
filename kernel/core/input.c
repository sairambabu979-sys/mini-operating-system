// SPDX-License-Identifier: MIT
/*
 * Console input queue. See <core/input.h>.
 *
 * The ring indices run free (u32) and are masked on access: head - tail
 * is the number of bytes queued. Both are touched only under input_lock.
 */
#define pr_fmt(fmt) "input: " fmt

#include <arch/irqflags.h>
#include <core/input.h>
#include <core/irq.h>
#include <core/klog.h>
#include <core/spinlock.h>
#include <core/time.h>
#include <drivers/i8042.h>
#include <drivers/serial.h>
#include <lib/kbdmap.h>
#include <sched/sched.h>

static_assert((INPUT_QUEUE_SIZE & (INPUT_QUEUE_SIZE - 1)) == 0, "the ring is masked");

static struct spinlock input_lock = SPINLOCK_INIT("input");
static struct wait_queue input_wq = WAIT_QUEUE_INIT(input_wq);
static u8 ring[INPUT_QUEUE_SIZE];
static u32 head, tail; /* free-running: head - tail bytes queued */
static struct input_stats stats;
static bool present;
static bool have_kbd;
static struct kbdmap kbd; /* decoder state: under input_lock */

#define KBD_DRAIN_MAX 16 /* bytes per call: QEMU raises a fresh edge for each one */
#define KBD_IRQ       1

static bool have_com1;
static bool com1_dead_reported;

#define COM1_DRAIN_MAX 64 /* bytes per IRQ 4; more waiting re-arms the interrupt */
#define COM1_IRQ       4

/* Under input_lock. Returns false if the byte was dropped. */
static bool push_locked(u8 byte)
{
    if (head - tail == INPUT_QUEUE_SIZE) {
        stats.dropped++;
        return false;
    }
    ring[head++ % INPUT_QUEUE_SIZE] = byte;
    return true;
}

void input_inject(const u8 *bytes, usize n)
{
    irqflags_t flags = spin_lock_irqsave(&input_lock);
    bool pushed = false;

    for (usize i = 0; i < n; i++) {
        stats.injected++;
        pushed |= push_locked(bytes[i]);
    }
    spin_unlock_irqrestore(&input_lock, flags);
    if (pushed)
        wake_up_all(&input_wq);
}

static bool racing_armed; /* see input_test_inject_at_next_sleep() */
static u8 racing_byte;

/*
 * The sleep condition of input_read(): takes up to max bytes, puts their
 * count in *taken, and is false if there were none. wait_event()
 * evaluates it only after the task is marked BLOCKED and on the queue, so
 * a byte pushed from then on wakes it and no wakeup is lost (the argument
 * is sched.c's, and is tested there). False is followed by a sleep, and is
 * counted so a test can tell that a reader is about to wait for the byte
 * it injects.
 */
static bool take(u8 *buf, usize max, usize *taken)
{
    irqflags_t flags = spin_lock_irqsave(&input_lock);
    bool race = false;
    usize n = 0;

    for (; n < max && tail != head; n++)
        buf[n] = ring[tail++ % INPUT_QUEUE_SIZE];
    if (!n) {
        stats.sleeps++;
        race = racing_armed;
        racing_armed = false;
    }
    spin_unlock_irqrestore(&input_lock, flags);
    if (race) /* as an interrupt between this check and the sleep would */
        input_inject(&racing_byte, 1);
    *taken = n;
    return n != 0;
}

usize input_read(u8 *buf, usize max)
{
    usize n;

    wait_event(&input_wq, take(buf, max, &n));
    return n;
}

void input_poll_kbd(void)
{
    irqflags_t flags = spin_lock_irqsave(&input_lock);
    bool pushed = false;
    int i;

    for (i = 0; i < KBD_DRAIN_MAX; i++) {
        int sc = i8042_read_kbd();
        u8 c;

        if (sc == I8042_EMPTY)
            break;
        if (sc == I8042_AUX)
            continue;
        stats.scancodes++;
        if (kbdmap_feed(&kbd, (u8)sc, &c))
            pushed |= push_locked(c);
    }
    if (i == 0)
        stats.empty_kbd_polls++;
    spin_unlock_irqrestore(&input_lock, flags);
    if (pushed)
        wake_up_all(&input_wq);
}

/* Queues bytes from COM1, then wakes readers. Any context. */
static void push_com1(const u8 *bytes, usize n)
{
    irqflags_t flags = spin_lock_irqsave(&input_lock);
    bool pushed = false;

    for (usize i = 0; i < n; i++) {
        stats.com1_bytes++;
        pushed |= push_locked(bytes[i]);
    }
    spin_unlock_irqrestore(&input_lock, flags);
    if (pushed)
        wake_up_all(&input_wq);
}

/*
 * IRQ 4. Only this handler reads the receiver once it is enabled (init
 * and the test hook run with interrupts off), so the drain needs no lock.
 */
static void com1_irq(struct trap_frame *frame, void *ctx)
{
    u8 buf[COM1_DRAIN_MAX];
    bool more, dead;

    (void)frame;
    (void)ctx;
    usize n = serial_rx_drain(buf, sizeof(buf), &more, &dead);

    push_com1(buf, n);
    if (dead) {
        serial_rx_disable();
        if (!com1_dead_reported) {
            com1_dead_reported = true;
            pr_warn("COM1 reads as absent: receive turned off\n");
        }
    } else if (more) {
        serial_rx_kick(); /* the edge for what is still waiting was spent on this IRQ */
    }
}

bool input_have_com1(void)
{
    return have_com1;
}

static void kbd_irq(struct trap_frame *frame, void *ctx)
{
    (void)frame;
    (void)ctx;
    input_poll_kbd();
}

bool input_have_kbd(void)
{
    return have_kbd;
}

bool input_test_kbd_put(u8 scancode)
{
    struct input_stats st;

    if (!have_kbd)
        return false;
    input_get_stats(&st);
    u64 before = st.scancodes;

    if (!i8042_test_put(scancode))
        return false;
    /* A second 0xD2 would overwrite this byte: wait until IRQ 1 took it. */
    u64 end = rdtsc() + 100 * tsc_khz();

    do {
        input_get_stats(&st);
        if (st.scancodes != before)
            return true;
        __builtin_ia32_pause();
    } while (rdtsc() < end);
    return false;
}

bool input_present(void)
{
    return __atomic_load_n(&present, __ATOMIC_RELAXED);
}

void input_get_stats(struct input_stats *out)
{
    irqflags_t flags = spin_lock_irqsave(&input_lock);

    *out = stats;
    out->queued = head - tail;
    spin_unlock_irqrestore(&input_lock, flags);
}

/* Just below the u32 wrap: every test that moves 100 bytes or more crosses it. */
#define TEST_INDEX_START ((u32)0 - 100)

void input_test_flush(void)
{
    irqflags_t flags = spin_lock_irqsave(&input_lock);

    head = tail = TEST_INDEX_START;
    racing_armed = false;
    kbd = (struct kbdmap){0}; /* no Shift or prefix left over from a test that stopped early */
    spin_unlock_irqrestore(&input_lock, flags);
}

void input_test_inject_at_next_sleep(u8 byte)
{
    irqflags_t flags = spin_lock_irqsave(&input_lock);

    racing_byte = byte;
    racing_armed = true;
    spin_unlock_irqrestore(&input_lock, flags);
}

bool input_test_set_present(bool value)
{
    return __atomic_exchange_n(&present, value, __ATOMIC_RELAXED);
}

void input_init(void)
{
    u8 old_config, new_config;

    if (i8042_init(&old_config, &new_config)) {
        if (irq_register(KBD_IRQ, kbd_irq, NULL) == 0) {
            have_kbd = true;
            /*
             * A byte that arrived before the line was unmasked raised its
             * edge while masked, and the I/O APIC drops those: drain by hand.
             */
            input_poll_kbd();
            pr_info("PS/2 keyboard on IRQ 1, 8042 config %#x -> %#x\n", old_config, new_config);
        } else {
            pr_warn("IRQ 1 is not available: no keyboard\n");
        }
    }
    if (serial_is_present()) {
        if (irq_register(COM1_IRQ, com1_irq, NULL) == 0) {
            u8 buf[COM1_DRAIN_MAX];
            bool more, dead;
            /*
             * Whatever arrived before now raised no interrupt: take it and
             * queue it, then enable the interrupt, all with interrupts off.
             * Bytes arriving meanwhile wait in the FIFO and come with the
             * first IRQ 4, so they are queued after these, in order.
             */
            irqflags_t flags = arch_irq_save();
            usize n = serial_rx_drain(buf, sizeof(buf), &more, &dead);

            push_com1(buf, n);
            serial_rx_enable();
            arch_irq_restore(flags);
            have_com1 = true;
            pr_info("COM1 receive on IRQ 4\n");
        } else {
            pr_warn("IRQ 4 is not available: no COM1 receive\n");
        }
    }
    present = have_kbd || have_com1;
    if (!present)
        pr_warn("no input device: read() returns -ENODEV\n");
}
