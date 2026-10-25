/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host-only FileX shim for the shared `camera` command's tests (issue #143).
 *
 * shell/cmds/cmd_camera.c writes `camera save` through FileX when a board says
 * CAMERA_SVC_HAS_SAVE, and shell/cmds/fs_cmd_core.h (struct fs_device) includes
 * fx_api.h.  This supplies only the names those two use; the functions are the
 * test's stubs (camera_svc_stub.c).  Found ahead of the real FileX via the
 * test include path, and the real one is never on it.
 */
#ifndef FX_API_H
#define FX_API_H

#include "tx_api.h"   /* the host ThreadX type shim: UINT, ULONG, CHAR, VOID */

typedef struct { int opaque; } FX_MEDIA;
typedef struct { int opaque; } FX_FILE;

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

#endif /* FX_API_H */
