/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    fal_cfg.h
 * @brief   FAL's device and partition table for shell/test/test_kv_store.c
 *          (issue #214): a flash model in RAM instead of a board's part.
 *
 * The store's partition is NOT at offset 0 and has a neighbour on each side
 * ("below", "above"), so an access that strays out of it lands somewhere the
 * model can see.  The device's length also runs past the last partition.
 */
#ifndef _FAL_CFG_H_
#define _FAL_CFG_H_

#define KV_TEST_DEV_NAME   "testnor"
#define KV_TEST_PART_NAME  "kv"
#define KV_TEST_SECTOR     4096u
#define KV_TEST_DEV_SIZE   (512u * 1024u)
#define KV_TEST_BELOW_LEN  (64u * 1024u)
#define KV_TEST_PART_OFF   KV_TEST_BELOW_LEN
#define KV_TEST_PART_LEN   (64u * 1024u)      /* 16 sectors: GC comes quickly */
#define KV_TEST_ABOVE_OFF  (KV_TEST_PART_OFF + KV_TEST_PART_LEN)
#define KV_TEST_ABOVE_LEN  (64u * 1024u)

#define FAL_PART_HAS_TABLE_CFG

extern const struct fal_flash_dev kv_test_flash;

#define FAL_FLASH_DEV_TABLE                                                    \
{                                                                              \
	&kv_test_flash,                                                            \
}

#define FAL_PART_TABLE                                                         \
{                                                                              \
	{FAL_PART_MAGIC_WORD, "below", KV_TEST_DEV_NAME, 0, KV_TEST_BELOW_LEN, 0}, \
	{FAL_PART_MAGIC_WORD, KV_TEST_PART_NAME, KV_TEST_DEV_NAME,                 \
	 KV_TEST_PART_OFF, KV_TEST_PART_LEN, 0},                                   \
	{FAL_PART_MAGIC_WORD, "above", KV_TEST_DEV_NAME,                           \
	 KV_TEST_ABOVE_OFF, KV_TEST_ABOVE_LEN, 0},                                 \
}

void kv_fdb_print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define FAL_PRINTF(...)                kv_fdb_print(__VA_ARGS__)

#endif /* _FAL_CFG_H_ */
