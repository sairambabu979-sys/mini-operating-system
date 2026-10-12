/* SPDX-License-Identifier: MIT */
/*
 * 8254 programmable interval timer: channel 0 is the legacy tick (IRQ 0),
 * channel 2 a polled one-shot for calibration that needs no interrupt.
 */
#ifndef MINIOS_DRIVERS_PIT_H
#define MINIOS_DRIVERS_PIT_H

#include <types.h>

#define PIT_INPUT_HZ 1193182u

/* Channel 0 in mode 2 (rate generator): IRQ 0 every `divisor` input clocks. */
void pit_start_periodic(u16 divisor);

/* Channel 0 in mode 0, never reloaded: at most one more edge on IRQ 0. */
void pit_stop(void);

/* The divisor last given to pit_start_periodic(), and a latched read of channel 0. */
u16 pit_divisor(void);
u16 pit_read_counter(void);

/* Channel 2 counts `count` input clocks once; pit_oneshot_done() polls its output. */
void pit_oneshot_start(u16 count);
bool pit_oneshot_done(void);

#endif /* MINIOS_DRIVERS_PIT_H */
