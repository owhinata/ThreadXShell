/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Wio Lite AI ThreadX Shell Project
 */
/**
 * @file    fs_cmd_core.h
 * @brief   Media-independent FileX command core (owhinata/wio-lite-ai#6).
 *
 * The FileX command bodies (ls/cat/write/rm/mkdir/info/umount) are written once
 * and parameterized over a `struct fs_device` vtable, so everything media-specific
 * -- the lazy-mount singleton, the ownership gates, the trailing info line, the
 * mount-failure hint -- is reached through the device rather than hard-coded.
 * Each command file owns its `struct fs_device` instance and registers thin
 * wrappers that bind it (the CLI passes only the leaf subcommand name as argv[0],
 * so the device cannot be recovered from argv).
 *
 * Today microSD (shell/cmds/cmd_sd.c, shared since issue #232) is the only
 * device.  The indirection is kept from the f746 original it was ported from --
 * where the same bodies also served a LevelX/QSPI NOR filesystem until #194.
 *
 * Thread-context only; the bodies use the cli_* output API and the device's
 * ownership gates, so they are safe from background jobs.
 */
#ifndef FS_CMD_CORE_H
#define FS_CMD_CORE_H

#include "fx_api.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cli_instance;

/* Sentinels returned by the ownership gates and by acquire(), deliberately outside
 * the FileX status space so fs_strerror() cannot confuse them with one.  Defined
 * here and nowhere else (issue #233): the mount glue that produces them
 * (shell/cmds/sd_fs_glue.c) includes this header. */
#define FS_ERR_BUSY    0xF0u   /* another owner (umount / format / raw) holds it */
#define FS_ERR_NO_CARD 0xF1u   /* the slot is empty (removable media only)       */
#define FS_ERR_UNWIRED 0xF2u   /* a field of the card driver table is empty:
                                  refused before the media or the card was
                                  touched (issue #233)                         */

/**
 * Media abstraction for the shared command bodies.  One instance per command file
 * (shell/cmds/cmd_sd.c => microSD); the callbacks forward to that media's glue.
 */
struct fs_device {
	const char *name;        /* "sd": message prefix                           */
	const char *mount_hint;  /* second-line hint printed on a mount failure     */
	UINT (*acquire)(FX_MEDIA **out);   /* lazy mount, return the media          */
	UINT (*unmount)(void);
	int  (*is_mounted)(void);
	UINT (*op_begin)(void);  void (*op_end)(void);     /* shared op slot         */
	UINT (*excl_begin)(void);void (*excl_end)(void);   /* exclusive (umount)     */
	void (*dir_lock)(void);  void (*dir_unlock)(void); /* listing serialization  */
	/* Trailing info line(s) for `info`, or NULL.  Called with the mounted media. */
	void (*info_extra)(struct cli_instance *sh, FX_MEDIA *media);
};

/** FileX status -> human string (media-neutral; hints come from mount_hint). */
const char *fs_strerror(UINT status);

/** Mount-on-demand: returns the media or NULL after printing the failure, and
 *  the device's mount_hint unless the failure is FS_ERR_BUSY, FS_ERR_NO_CARD
 *  or FS_ERR_UNWIRED (not about the filesystem on the card).  Exposed for
 *  the commands that write through a medium (`camera save`).  `sd format`
 *  does not use it: it remounts while it still holds the exclusive slot,
 *  which this entry point would refuse. */
FX_MEDIA *fs_core_mount(const struct fs_device *dev, struct cli_instance *sh);

/* Shared command bodies; each binds a device.  Argument arity matches the
 * registered subcommands (argv[0] = leaf name). */
int fs_core_ls    (const struct fs_device *dev, struct cli_instance *sh, int argc, char **argv);
int fs_core_cat   (const struct fs_device *dev, struct cli_instance *sh, int argc, char **argv);
int fs_core_write (const struct fs_device *dev, struct cli_instance *sh, int argc, char **argv);
int fs_core_rm    (const struct fs_device *dev, struct cli_instance *sh, int argc, char **argv);
int fs_core_mkdir (const struct fs_device *dev, struct cli_instance *sh, int argc, char **argv);
int fs_core_info  (const struct fs_device *dev, struct cli_instance *sh, int argc, char **argv);
int fs_core_umount(const struct fs_device *dev, struct cli_instance *sh, int argc, char **argv);

/**
 * Read an entire file from @p dev into @p buf (capacity @p cap bytes) through the
 * device's shared op gate; fails if the file is larger than @p cap.  Returns 0 with
 * *out_len set to the bytes read, or 1 (a message is printed).  The cross-command
 * reuse point for future consumers that need a whole file in RAM (owhinata/wio-lite-ai#7
 * camera, owhinata/wio-lite-ai#9 AI).
 */
int fs_core_read_file(const struct fs_device *dev, struct cli_instance *sh,
                      const char *path, void *buf, uint32_t cap, uint32_t *out_len);

/* The per-device accessors are NOT declared here, so this header stays
 * media-neutral (the f746 port carried a QSPI NOR filesystem beside the microSD
 * until #194).  The microSD's, fs_sd_device(), is declared with the rest of the
 * `sd` command's board interface in shell/cmds/sd_cmd_board.h (issue #232; each
 * board's cmds/fs_devices.h until then); which media a board offers another
 * command is that board's (shell/cmds/camera_cmd_board.h). */

#ifdef __cplusplus
}
#endif

#endif /* FS_CMD_CORE_H */
