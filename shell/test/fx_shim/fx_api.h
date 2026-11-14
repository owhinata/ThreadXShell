/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host-only FileX shim for the shared `camera` command's tests (issue #143),
 * the shared `sd` command's (issue #232), and the shared microSD mount glue's,
 * FileX media driver's and filesystem bodies' (issue #233).
 *
 * shell/cmds/cmd_camera.c writes `camera save` through FileX when a board says
 * CAMERA_SVC_HAS_SAVE, and shell/cmds/fs_cmd_core.h (struct fs_device) includes
 * fx_api.h.  shell/cmds/cmd_sd.c formats through fx_media_format() and reads
 * the media's FAT32 flag.  shell/cmds/sd_fs_glue.c opens, closes and aborts
 * the media, shell/cmds/fx_sd_driver.c serves FileX's driver requests, and
 * shell/cmds/fs_cmd_core.c walks directories and files.  This supplies only the
 * names those use, with FileX's own values for every constant (the tests
 * print and compare them); the functions are the tests' stubs
 * (camera_svc_stub.c, test_sd_cmd.c, sd_fake.c).  Found ahead of the real
 * FileX via the test include path, and the real one is never on it.
 */
#ifndef FX_API_H
#define FX_API_H

#include "tx_api.h"   /* the host ThreadX type shim: UINT, ULONG, CHAR, VOID */

/* The fields a file here reads or writes, by their FileX names. */
typedef struct FX_MEDIA_STRUCT {
	UINT   fx_media_32_bit_FAT;
	UINT   fx_media_bytes_per_sector;
	UINT   fx_media_sectors_per_cluster;
	ULONG  fx_media_total_clusters;
	UINT   fx_media_driver_request;
	UINT   fx_media_driver_status;
	UCHAR *fx_media_driver_buffer;
	ULONG  fx_media_driver_logical_sector;
	ULONG  fx_media_driver_sectors;
	UINT   fx_media_driver_free_sector_update;
} FX_MEDIA;
typedef unsigned long long ULONG64;
typedef struct { ULONG64 fx_file_current_file_size; } FX_FILE;

#define FX_TRUE             1
#define FX_FALSE            0
#define FX_NULL             0

#define FX_MAX_LONG_NAME_LEN 256
#define FX_OPEN_FOR_READ    0u
#define FX_OPEN_FOR_WRITE   1u
#define FX_DIRECTORY        0x10u

#define FX_SUCCESS          0x00u
#define FX_BOOT_ERROR       0x01u
#define FX_MEDIA_INVALID    0x02u
#define FX_NOT_FOUND        0x04u
#define FX_NOT_A_FILE       0x05u
#define FX_ACCESS_ERROR     0x06u
#define FX_FILE_CORRUPT     0x08u
#define FX_END_OF_FILE      0x09u
#define FX_NO_MORE_SPACE    0x0Au
#define FX_ALREADY_CREATED  0x0Bu
#define FX_INVALID_NAME     0x0Cu
#define FX_NOT_DIRECTORY    0x0Eu
#define FX_NO_MORE_ENTRIES  0x0Fu
#define FX_DIR_NOT_EMPTY    0x10u
#define FX_MEDIA_NOT_OPEN   0x11u
#define FX_WRITE_PROTECT    0x23u
#define FX_IO_ERROR         0x90u

#define FX_DRIVER_READ             0u
#define FX_DRIVER_WRITE            1u
#define FX_DRIVER_FLUSH            2u
#define FX_DRIVER_ABORT            3u
#define FX_DRIVER_INIT             4u
#define FX_DRIVER_BOOT_READ        5u
#define FX_DRIVER_RELEASE_SECTORS  6u
#define FX_DRIVER_BOOT_WRITE       7u
#define FX_DRIVER_UNINIT           8u

VOID fx_system_initialize(VOID);
UINT fx_media_open(FX_MEDIA *media_ptr, CHAR *media_name,
                   VOID (*media_driver)(FX_MEDIA *), VOID *driver_info_ptr,
                   VOID *memory_ptr, ULONG memory_size);
UINT fx_media_close(FX_MEDIA *media_ptr);
UINT fx_media_abort(FX_MEDIA *media_ptr);
UINT fx_media_flush(FX_MEDIA *media);
UINT fx_media_extended_space_available(FX_MEDIA *media_ptr,
                                       ULONG64 *available_bytes_ptr);
UINT fx_media_format(FX_MEDIA *media_ptr, VOID (*driver)(FX_MEDIA *media),
                     VOID *driver_info_ptr, UCHAR *memory_ptr, UINT memory_size,
                     CHAR *volume_name, UINT number_of_fats,
                     UINT directory_entries, UINT hidden_sectors,
                     ULONG total_sectors, UINT bytes_per_sector,
                     UINT sectors_per_cluster, UINT heads,
                     UINT sectors_per_track);
UINT fx_directory_default_set(FX_MEDIA *media_ptr, CHAR *new_path_name);
UINT fx_directory_first_full_entry_find(FX_MEDIA *media_ptr,
                                        CHAR *directory_name, UINT *attributes,
                                        ULONG *size, UINT *year, UINT *month,
                                        UINT *day, UINT *hour, UINT *minute,
                                        UINT *second);
UINT fx_directory_next_full_entry_find(FX_MEDIA *media_ptr,
                                       CHAR *directory_name, UINT *attributes,
                                       ULONG *size, UINT *year, UINT *month,
                                       UINT *day, UINT *hour, UINT *minute,
                                       UINT *second);
UINT fx_directory_create(FX_MEDIA *media_ptr, CHAR *directory_name);
UINT fx_directory_delete(FX_MEDIA *media_ptr, CHAR *directory_name);
UINT fx_file_create(FX_MEDIA *media, CHAR *name);
UINT fx_file_delete(FX_MEDIA *media_ptr, CHAR *file_name);
UINT fx_file_open(FX_MEDIA *media, FX_FILE *file, CHAR *name, UINT type);
UINT fx_file_read(FX_FILE *file_ptr, VOID *buffer_ptr, ULONG request_size,
                  ULONG *actual_size);
UINT fx_file_truncate(FX_FILE *file, ULONG size);
UINT fx_file_write(FX_FILE *file, VOID *buf, ULONG size);
UINT fx_file_close(FX_FILE *file);

#endif /* FX_API_H */
