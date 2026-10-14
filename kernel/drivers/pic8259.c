// SPDX-License-Identifier: MIT
/*
 * 8259A PIC driver.
 *
 * The BIOS leaves IRQ 0-7 on vectors 8-15, colliding with CPU exceptions
 * (#DF is vector 8), so the first job is a remap.
 */
#include <arch/io.h>
#include <drivers/pic8259.h>

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define ICW1_INIT_ICW4 0x11 /* edge-triggered, cascade, ICW4 follows */
#define ICW4_8086      0x01
#define OCW2_EOI       0x20
#define OCW3_READ_ISR  0x0B

static u16 mask_shadow = 0xFFFF;

static void write_masks(void)
{
    outb(PIC1_DATA, (u8)mask_shadow);
    outb(PIC2_DATA, (u8)(mask_shadow >> 8));
}

void pic_init(u8 vector_base)
{
    outb(PIC1_CMD, ICW1_INIT_ICW4);
    io_wait();
    outb(PIC2_CMD, ICW1_INIT_ICW4);
    io_wait();
    outb(PIC1_DATA, vector_base); /* ICW2: vector offsets */
    io_wait();
    outb(PIC2_DATA, (u8)(vector_base + 8));
    io_wait();
    outb(PIC1_DATA, 1u << PIC_CASCADE); /* ICW3: slave on input 2 */
    io_wait();
    outb(PIC2_DATA, PIC_CASCADE); /* ICW3: slave identity */
    io_wait();
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    /* Everything masked except the cascade, so slave lines can be enabled. */
    mask_shadow = (u16) ~(1u << PIC_CASCADE);
    write_masks();
}

void pic_mask(unsigned line)
{
    mask_shadow |= (u16)(1u << line);
    write_masks();
}

void pic_unmask(unsigned line)
{
    mask_shadow &= (u16) ~(1u << line);
    write_masks();
}

bool pic_is_masked(unsigned line)
{
    return mask_shadow & (1u << line);
}

void pic_eoi(unsigned line)
{
    if (line >= 8)
        outb(PIC2_CMD, OCW2_EOI);
    outb(PIC1_CMD, OCW2_EOI);
}

static u8 read_isr(u16 cmd_port)
{
    outb(cmd_port, OCW3_READ_ISR);
    return inb(cmd_port);
}

bool pic_handle_spurious(unsigned line)
{
    if (line == 7 && !(read_isr(PIC1_CMD) & 0x80))
        return true; /* nothing in service: no EOI */
    if (line == 15 && !(read_isr(PIC2_CMD) & 0x80)) {
        /* The master did see a real request on the cascade line. */
        outb(PIC1_CMD, OCW2_EOI);
        return true;
    }
    return false;
}
