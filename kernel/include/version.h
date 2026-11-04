/* SPDX-License-Identifier: MIT */
#ifndef MINIOS_VERSION_H
#define MINIOS_VERSION_H

#define MINIOS_NAME    "minios"
#define MINIOS_VERSION "1.5.0"

#ifdef CONFIG_DEBUG
#define MINIOS_BUILD "debug"
#else
#define MINIOS_BUILD "release"
#endif

#endif /* MINIOS_VERSION_H */
