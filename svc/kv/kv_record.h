/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    kv_record.h
 * @brief   What the key-value store writes under a key, and which keys and
 *          values it accepts (issue #214; moved out of wio-lite-ai's src/kv.c).
 *
 * Pure functions over the caller's buffers: no FlashDB, no lock, no storage.
 * svc/kv/kv.c is their one caller in a firmware; shell/test/test_kv_record.c
 * pins their edges.
 *
 * What FlashDB stores under a key is not the bare value but a record carrying the
 * value's type and a human description alongside it.  That is the point of the
 * store: what a setting means lives next to the setting on the external flash, not
 * in a table compiled into the firmware, so the internal flash holds only this
 * pack/unpack and nothing that has to be reflashed when a setting is added.
 *
 *   offset 0   magic     u32   identifies our records and rejects everything else
 *          4   ver       u8    format version; an unknown one is refused, not guessed
 *          5   type      u8    KV_TYPE_*
 *          6   desc_len  u16
 *          8   total_len u32   whole record INCLUDING this 12-byte header
 *         12   desc[desc_len]  UTF-8, no terminator
 *              value[total_len - 12 - desc_len]
 *
 * Everything is packed and unpacked BYTE BY BYTE in little-endian order.  The
 * struct is never overlaid on the flash image: doing that would make the on-flash
 * layout depend on the compiler's padding and alignment rules, i.e. on a build
 * detail, for a format that has to survive firmware changes.
 */
#ifndef SVC_KV_RECORD_H
#define SVC_KV_RECORD_H

#include <stdint.h>

#include "kv.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KV_REC_MAGIC   0x31564B57u   /* bytes 'W','K','V','1' in memory order */
#define KV_REC_VER     1u
#define KV_REC_HDR     12u
#define KV_REC_MAX     (KV_REC_HDR + KV_DESC_MAX + KV_VALUE_MAX)

/*
 * Keys are dotted paths.  Restricting the alphabet is not decoration: keys are
 * typed on a shell line, printed in tables and compared by name, so a key with a
 * space or a control character in it would be storable but not addressable.
 * Nonzero when @p key is 1..KV_KEY_MAX characters of [A-Za-z0-9._-].
 */
int kv_key_ok(const char *key);

/** Nonzero for the four KV_TYPE_* a record may carry. */
int kv_type_ok(uint8_t type);

/*
 * Length and content check for a value of @p type, applied on the way in AND on
 * the way out.  Types that fix their own length are checked here, so a u32 can
 * never be stored as three bytes and read back as garbage later; only
 * KV_TYPE_BOOL constrains its bytes (0 or 1).  @p val is read only when the length
 * is right.
 */
int kv_value_bytes_ok(uint8_t type, const uint8_t *val, uint32_t len);

/** Little-endian u32 at @p p (the record's fields and a KV_TYPE_U32 value). */
uint32_t kv_rec_u32(const uint8_t *p);

/* Build a record into @p out (KV_REC_MAX bytes).  Returns its length, or 0 if the
 * inputs do not fit. */
uint32_t kv_rec_pack(uint8_t *out, uint8_t type, const void *val, uint32_t vlen,
                     const char *desc);

/* Decode a record into @p out: KV_OK, or KV_ERR_FORMAT with @p out emptied
 * (type KV_TYPE_INVALID, both strings ""). */
int kv_rec_unpack(const uint8_t *raw, uint32_t rawlen, struct kv_value *out);

#ifdef __cplusplus
}
#endif

#endif /* SVC_KV_RECORD_H */
