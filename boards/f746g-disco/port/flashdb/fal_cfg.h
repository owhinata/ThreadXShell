/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    fal_cfg.h
 * @brief   FAL (Flash Abstraction Layer) device and partition table over the
 *          QSPI NOR (issue #214).
 *
 * FAL includes this by name from fal.h.  It declares which flash devices exist
 * and how they are carved up; the device itself is port/flashdb/fal_qspi.c.
 */
#ifndef _FAL_CFG_H_
#define _FAL_CFG_H_

/* Names are matched by strncmp at run time, so they must agree with the ones the
 * partition table and the shared store (svc/kv/kv.c, through fal_qspi.c's
 * kv_board_facts) use -- hence the macros. */
#define QSPI_FAL_DEV_NAME              "n25q128"
#define KV_PART_NAME                   "kv"

/*
 * Layout of the 16 MB QSPI NOR (src/blob.h has the whole map):
 *
 *   0x000000  kv        1 MB   FlashDB KVDB, 4 KB sectors x 256 (THIS)
 *   0x100000  blob      7 MB   the asset store's 13 slots (src/blob_slots.c),
 *                              owned by src/blob.c
 *   0x800000  (unused)  8 MB   unallocated
 *
 * [!] THE FAL DEVICE IS THE FIRST MEGABYTE, NOT THE PART (KV_FAL_DEV_LEN).  The
 * blob store has no partition entry here, as on wio-lite-ai, and on this board
 * the device FlashDB is given does not even reach it: FAL checks a partition
 * against nothing, so the device's own ops refuse an offset at or past
 * KV_FAL_DEV_LEN.  Whatever FlashDB is asked for, it has no route out of the
 * first megabyte.  0x100000 is a 64 KB boundary, so a 64 KB erase inside the
 * partition cannot reach the blob store either.
 *
 * The same offset and size as wio-lite-ai's, so the two boards' stores are laid
 * out alike.  1 MB is far more than the key space needs; FlashDB is
 * log-structured, so the spare sectors are what turn a rewrite into an append.
 */
#define KV_FAL_DEV_LEN                 (1024u * 1024u)
#define KV_PART_OFFSET                 0u
#define KV_PART_LEN                    KV_FAL_DEV_LEN

#define FAL_PART_HAS_TABLE_CFG

extern const struct fal_flash_dev qspi_fal_dev;

#define FAL_FLASH_DEV_TABLE                                                    \
{                                                                              \
	&qspi_fal_dev,                                                             \
}

/* FAL_PART_MAGIC_WORD is defined inside fal_partition.c, which is the only place
 * this macro is expanded -- expansion is lazy, so the reference resolves there. */
#define FAL_PART_TABLE                                                         \
{                                                                              \
	{FAL_PART_MAGIC_WORD, KV_PART_NAME, QSPI_FAL_DEV_NAME,                     \
	 KV_PART_OFFSET, KV_PART_LEN, 0},                                          \
}

/* Route FAL's own logging into the same line-assembling shim FlashDB uses, so it
 * lands in `dmesg` rather than on a console someone may be typing into.  fal.h
 * includes this file before fal_def.h, whose defaults are #ifndef-guarded. */
void kv_fdb_print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define FAL_PRINTF(...)                kv_fdb_print(__VA_ARGS__)

#endif /* _FAL_CFG_H_ */
