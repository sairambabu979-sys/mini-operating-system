// SPDX-License-Identifier: MIT
/*
 * 8042 PS/2 controller, keyboard port. See <drivers/i8042.h>.
 */
#define pr_fmt(fmt) "i8042: " fmt

#include <arch/io.h>
#include <arch/irqflags.h>
#include <core/klog.h>
#include <core/time.h>
#include <drivers/i8042.h>

#define PORT_DATA   0x60
#define PORT_STATUS 0x64 /* read */
#define PORT_CMD    0x64 /* write */

#define ST_OUT_FULL 0x01 /* a byte waits at PORT_DATA */
#define ST_IN_FULL  0x02 /* the controller has not taken the last write yet */
#define ST_AUX      0x20 /* the waiting byte is from the mouse port */
#define ST_ABSENT   0xFF

#define CMD_READ_CONFIG  0x20
#define CMD_WRITE_CONFIG 0x60
#define CMD_AUX_OFF      0xA7
#define CMD_KBD_OFF      0xAD
#define CMD_KBD_ON       0xAE
#define CMD_FAKE_KBD     0xD2 /* the next data byte comes back as if from the keyboard */

#define KBD_ENABLE_SCANNING 0xF4
#define KBD_ACK             0xFA

#define WAIT_US     10000 /* for the controller */
#define ACK_WAIT_US 25000 /* for the keyboard, behind it */
#define FLUSH_MAX   16

static u64 aux_dropped;

static u64 deadline_us(u64 us)
{
    return rdtsc() + us * tsc_khz() / 1000;
}

/* Waits for (status & mask) == want; false at the deadline, or at once with no controller. */
static bool wait_status(u8 mask, u8 want, u64 us)
{
    u64 end = deadline_us(us);

    for (;;) {
        u8 st = inb(PORT_STATUS);

        if (st == ST_ABSENT)
            return false;
        if ((st & mask) == want)
            return true;
        if (rdtsc() > end)
            return false;
        __builtin_ia32_pause();
    }
}

static bool write_cmd(u8 cmd)
{
    if (!wait_status(ST_IN_FULL, 0, WAIT_US))
        return false;
    outb(PORT_CMD, cmd);
    return true;
}

static bool write_data(u8 value)
{
    if (!wait_status(ST_IN_FULL, 0, WAIT_US))
        return false;
    outb(PORT_DATA, value);
    return true;
}

static bool read_data(u8 *value, u64 us)
{
    if (!wait_status(ST_OUT_FULL, ST_OUT_FULL, us))
        return false;
    *value = inb(PORT_DATA);
    return true;
}

bool i8042_read_config(u8 *config)
{
    irqflags_t flags = arch_irq_save(); /* the IRQ 1 handler must not take the reply */
    bool ok = write_cmd(CMD_READ_CONFIG) && read_data(config, WAIT_US);

    arch_irq_restore(flags);
    return ok;
}

static bool write_config(u8 config)
{
    return write_cmd(CMD_WRITE_CONFIG) && write_data(config);
}

/* Drops whatever the firmware or a keypress left in the output buffer. */
static void flush(void)
{
    for (int i = 0; i < FLUSH_MAX; i++) {
        u8 st = inb(PORT_STATUS);

        if (st == ST_ABSENT || !(st & ST_OUT_FULL))
            return;
        (void)inb(PORT_DATA);
    }
}

/* Enables scanning; the keyboard answers with an ACK, read here by polling. */
static bool enable_scanning(void)
{
    u64 end = deadline_us(ACK_WAIT_US);
    u8 reply;

    if (!write_data(KBD_ENABLE_SCANNING))
        return false;
    while (rdtsc() <= end)
        if (read_data(&reply, WAIT_US) && reply == KBD_ACK)
            return true;
    return false;
}

bool i8042_init(u8 *old_config, u8 *new_config)
{
    u8 config, readback;

    if (inb(PORT_STATUS) == ST_ABSENT) {
        pr_warn("no controller (status 0xff)\n");
        return false;
    }
    if (!write_cmd(CMD_KBD_OFF) || !write_cmd(CMD_AUX_OFF))
        goto dead;
    flush();
    if (!i8042_read_config(&config))
        goto dead;
    *old_config = config;
    /*
     * The keyboard port stays off until 0xAE: with its clock bit clear,
     * a key still queued in the keyboard would land in the output buffer
     * and be read back as the configuration byte. Its IRQ stays off to
     * the end, so nothing below can be interrupted by a keystroke.
     */
    config =
        (config & I8042_CFG_SYSTEM) | I8042_CFG_KBD_OFF | I8042_CFG_AUX_OFF | I8042_CFG_TRANSLATE;
    if (!write_config(config) || !i8042_read_config(&readback))
        goto dead;
    if (readback != config) {
        pr_warn("config byte did not stick: wrote %#x, read back %#x\n", config, readback);
        return false;
    }
    if (!write_cmd(CMD_KBD_ON))
        goto dead;
    if (!enable_scanning()) /* also drops a key that was waiting in the keyboard */
        pr_warn("keyboard did not acknowledge enable (0xf4); carrying on\n");
    config = (config & (u8)~I8042_CFG_KBD_OFF) | I8042_CFG_KBD_IRQ; /* as 0xAE left it */
    if (!write_config(config))
        goto dead;
    *new_config = config;
    return true;

dead:
    pr_warn("controller stopped answering\n");
    return false;
}

int i8042_read_kbd(void)
{
    u8 st = inb(PORT_STATUS);

    if (st == ST_ABSENT || !(st & ST_OUT_FULL))
        return I8042_EMPTY;
    u8 byte = inb(PORT_DATA);

    if (st & ST_AUX) {
        aux_dropped++;
        return I8042_AUX;
    }
    return byte;
}

u64 i8042_aux_dropped(void)
{
    return aux_dropped;
}

bool i8042_output_full(void)
{
    u8 st = inb(PORT_STATUS);

    return st != ST_ABSENT && (st & ST_OUT_FULL);
}

bool i8042_test_put(u8 byte)
{
    irqflags_t flags = arch_irq_save(); /* the command and its byte go together */
    bool ok = write_cmd(CMD_FAKE_KBD) && write_data(byte);

    arch_irq_restore(flags);
    return ok;
}
