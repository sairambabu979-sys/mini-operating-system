/* SPDX-License-Identifier: MIT */
/*
 * The slice of the UEFI specification (2.10) this loader uses.
 *
 * Only what is called is declared; tables keep every member up to the last
 * one used, in spec order, because callers index them by position. Every
 * function uses the Microsoft x64 calling convention, which is the default
 * for the x86_64-unknown-windows target this file is compiled for.
 */
#ifndef MINIOS_BOOT_EFI_H
#define MINIOS_BOOT_EFI_H

#include <compiler.h>
#include <types.h>

typedef u64 efi_status;
typedef void *efi_handle;
typedef u16 char16;

#define EFI_SUCCESS           0ull
#define EFI_ERROR_BIT         (1ull << 63)
#define EFI_INVALID_PARAMETER (EFI_ERROR_BIT | 2)
#define EFI_BUFFER_TOO_SMALL  (EFI_ERROR_BIT | 5)
#define EFI_NOT_FOUND         (EFI_ERROR_BIT | 14)

struct efi_guid {
    u32 data1;
    u16 data2, data3;
    u8 data4[8];
};

struct efi_table_header {
    u64 signature;
    u32 revision, header_size, crc32, reserved;
};

struct efi_simple_text_output {
    void *reset;
    efi_status (*output_string)(struct efi_simple_text_output *self, const char16 *s);
};

enum efi_allocate_type {
    EFI_ALLOCATE_ANY_PAGES = 0,
    EFI_ALLOCATE_MAX_ADDRESS = 1,
    EFI_ALLOCATE_ADDRESS = 2,
};

enum efi_memory_type {
    EFI_RESERVED_MEMORY = 0,
    EFI_LOADER_CODE = 1,
    EFI_LOADER_DATA = 2,
    EFI_BOOT_SERVICES_CODE = 3,
    EFI_BOOT_SERVICES_DATA = 4,
    EFI_RUNTIME_SERVICES_CODE = 5,
    EFI_RUNTIME_SERVICES_DATA = 6,
    EFI_CONVENTIONAL_MEMORY = 7,
    EFI_UNUSABLE_MEMORY = 8,
    EFI_ACPI_RECLAIM_MEMORY = 9,
    EFI_ACPI_MEMORY_NVS = 10,
    EFI_MEMORY_MAPPED_IO = 11,
    EFI_MEMORY_MAPPED_IO_PORT_SPACE = 12,
    EFI_PAL_CODE = 13,
    EFI_PERSISTENT_MEMORY = 14,
};

/* Firmware strides descriptors by the size GetMemoryMap returns, not sizeof. */
struct efi_memory_descriptor {
    u32 type;
    u32 pad;
    u64 physical_start;
    u64 virtual_start;
    u64 number_of_pages;
    u64 attribute;
};

struct efi_boot_services {
    struct efi_table_header hdr;
    void *raise_tpl, *restore_tpl;
    efi_status (*allocate_pages)(enum efi_allocate_type type, enum efi_memory_type mem, u64 pages,
                                 u64 *address);
    void *free_pages;
    efi_status (*get_memory_map)(u64 *size, struct efi_memory_descriptor *map, u64 *key,
                                 u64 *descriptor_size, u32 *descriptor_version);
    efi_status (*allocate_pool)(enum efi_memory_type mem, u64 size, void **buffer);
    void *free_pool;
    void *create_event, *set_timer, *wait_for_event, *signal_event, *close_event, *check_event;
    void *install_protocol_interface, *reinstall_protocol_interface, *uninstall_protocol_interface;
    efi_status (*handle_protocol)(efi_handle handle, const struct efi_guid *protocol,
                                  void **interface);
    void *reserved, *register_protocol_notify, *locate_handle, *locate_device_path,
        *install_configuration_table;
    void *load_image, *start_image, *exit, *unload_image;
    efi_status (*exit_boot_services)(efi_handle image, u64 map_key);
};

struct efi_configuration_table {
    struct efi_guid vendor_guid;
    void *vendor_table;
};

struct efi_system_table {
    struct efi_table_header hdr;
    char16 *firmware_vendor;
    u32 firmware_revision;
    efi_handle console_in_handle;
    void *con_in;
    efi_handle console_out_handle;
    struct efi_simple_text_output *con_out;
    efi_handle standard_error_handle;
    void *std_err;
    void *runtime_services;
    struct efi_boot_services *boot_services;
    u64 number_of_table_entries;
    struct efi_configuration_table *configuration_table;
};

struct efi_loaded_image {
    u32 revision;
    efi_handle parent_handle;
    struct efi_system_table *system_table;
    efi_handle device_handle;
    void *file_path;
    void *reserved;
    u32 load_options_size;
    void *load_options;
    void *image_base;
    u64 image_size;
};

#define EFI_FILE_MODE_READ 1ull

struct efi_file {
    u64 revision;
    efi_status (*open)(struct efi_file *self, struct efi_file **out, const char16 *name, u64 mode,
                       u64 attributes);
    efi_status (*close)(struct efi_file *self);
    void *delete_file;
    efi_status (*read)(struct efi_file *self, u64 *size, void *buffer);
    void *write;
    efi_status (*get_position)(struct efi_file *self, u64 *position);
    efi_status (*set_position)(struct efi_file *self, u64 position);
};

struct efi_simple_file_system {
    u64 revision;
    efi_status (*open_volume)(struct efi_simple_file_system *self, struct efi_file **root);
};

/* Offsets from the specification: a misplaced member would call the wrong service. */
static_assert(__builtin_offsetof(struct efi_system_table, con_out) == 64, "UEFI 4.3");
static_assert(__builtin_offsetof(struct efi_system_table, boot_services) == 96, "UEFI 4.3");
static_assert(__builtin_offsetof(struct efi_system_table, configuration_table) == 112, "UEFI 4.3");
static_assert(sizeof(struct efi_configuration_table) == 24, "UEFI 4.6");
static_assert(__builtin_offsetof(struct efi_boot_services, allocate_pages) == 40, "UEFI 4.4");
static_assert(__builtin_offsetof(struct efi_boot_services, get_memory_map) == 56, "UEFI 4.4");
static_assert(__builtin_offsetof(struct efi_boot_services, handle_protocol) == 152, "UEFI 4.4");
static_assert(__builtin_offsetof(struct efi_boot_services, exit_boot_services) == 232, "UEFI 4.4");
static_assert(__builtin_offsetof(struct efi_loaded_image, image_size) == 72, "UEFI 9.1");
static_assert(sizeof(struct efi_memory_descriptor) == 40, "UEFI 7.2");

#endif /* MINIOS_BOOT_EFI_H */
