/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host-only FileX shim for the shared `camera` command's tests (issue #143)
 * and the shared `sd` command's (issue #232).
 *
 * shell/cmds/cmd_camera.c writes `camera save` through FileX when a board says
 * CAMERA_SVC_HAS_SAVE, and shell/cmds/fs_cmd_core.h (struct fs_device) includes
 * fx_api.h.  shell/cmds/cmd_sd.c formats through fx_media_format() and reads
 * the media's FAT32 flag.  This supplies only the names those use; the
 * functions are the tests' stubs (camera_svc_stub.c, test_sd_cmd.c).  Found
 * ahead of the real FileX via the test include path, and the real one is
 * never on it.
 */
#ifndef FX_API_H
#define FX_API_H

#include "tx_api.h"   /* the host ThreadX type shim: UINT, ULONG, CHAR, VOID */

/* The one field a command reads (`sd format`'s FAT32 check), by its name. */
typedef struct { UINT fx_media_32_bit_FAT; } FX_MEDIA;
typedef struct { int opaque; } FX_FILE;

#define FX_TRUE             1
#define FX_FALSE            0
#define FX_NULL             0

#define FX_SUCCESS          0x00u
#define FX_ALREADY_CREATED  0x0Bu
#define FX_NO_MORE_SPACE    0x0Au
#define FX_OPEN_FOR_WRITE   1u

UINT fx_file_create(FX_MEDIA *media, CHAR *name);
UINT fx_file_open(FX_MEDIA *media, FX_FILE *file, CHAR *name, UINT type);
UINT fx_file_truncate(FX_FILE *file, ULONG size);
UINT fx_file_write(FX_FILE *file, VOID *buf, ULONG size);
UINT fx_file_close(FX_FILE *file);
UINT fx_media_flush(FX_MEDIA *media);
UINT fx_media_format(FX_MEDIA *media_ptr, VOID (*driver)(FX_MEDIA *media),
                     VOID *driver_info_ptr, UCHAR *memory_ptr, UINT memory_size,
                     CHAR *volume_name, UINT number_of_fats,
                     UINT directory_entries, UINT hidden_sectors,
                     ULONG total_sectors, UINT bytes_per_sector,
                     UINT sectors_per_cluster, UINT heads,
                     UINT sectors_per_track);

#endif /* FX_API_H */
