/* SPDX-License-Identifier: MIT */
/*
 * Access to the boot information handed over by the loader.
 */
#ifndef MINIOS_CORE_BOOT_H
#define MINIOS_CORE_BOOT_H

#include <boot/boot_info.h>

/* Valid after kmain() has validated it; never NULL afterwards. */
const struct boot_info *boot_info_get(void);

/* RFLAGS as the bootloader handed over control (IF must be clear). */
u64 boot_entry_rflags(void);

#endif /* MINIOS_CORE_BOOT_H */
