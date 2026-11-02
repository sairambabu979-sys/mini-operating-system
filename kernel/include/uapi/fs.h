/* SPDX-License-Identifier: MIT */
#ifndef MINIOS_UAPI_FS_H
#define MINIOS_UAPI_FS_H
#include <types.h>
#define FS_PATH_MAX 256
#define FS_REGULAR 1u
#define FS_DIRECTORY 2u
#define FS_FD_MAX 16
#define FS_SEEK_SET 0
#define FS_SEEK_CUR 1
#define FS_SEEK_END 2
struct fs_stat {
    u64 size;
    u32 type;
    u32 reserved;
};
struct fs_dirent {
    char name[FS_PATH_MAX];
    struct fs_stat stat;
};
#endif
