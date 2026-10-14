/* SPDX-License-Identifier: MIT */
/*
 * QEMU isa-debug-exit device: lets tests end the VM with a status code.
 *
 * QEMU exits with status (code << 1) | 1, so SUCCESS -> 33, FAILURE -> 35.
 * Without the device (real hardware, plain `make run`) the write is ignored
 * and qemu_exit() halts instead.
 */
#ifndef MINIOS_DRIVERS_QEMU_H
#define MINIOS_DRIVERS_QEMU_H

#include <compiler.h>

#define QEMU_DEBUG_EXIT_PORT 0xF4

enum qemu_exit_code {
    QEMU_EXIT_SUCCESS = 0x10,
    QEMU_EXIT_FAILURE = 0x11,
};

__noreturn void qemu_exit(enum qemu_exit_code code);

#endif /* MINIOS_DRIVERS_QEMU_H */
