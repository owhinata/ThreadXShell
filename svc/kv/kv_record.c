/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    kv_record.c
 * @brief   The key-value store's record format and its key and value checks
 *          (issue #214; moved unchanged out of wio-lite-ai's src/kv.c).
 *
 * See kv_record.h.  No mutable storage (the shared storage audit).
 */
#include "kv_record.h"

#include <stddef.h>
#include <string.h>

int kv_key_ok(const char *key)
{
	size_t i;

	if (key == NULL || key[0] == '\0')
		return 0;
	for (i = 0u; key[i] != '\0'; i++) {
		char c = key[i];

		if (i >= KV_KEY_MAX)
			return 0;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
			return 0;
	}
	return 1;
}

static void kv_put_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xFFu);
	p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void kv_put_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xFFu);
	p[1] = (uint8_t)((v >> 8) & 0xFFu);
	p[2] = (uint8_t)((v >> 16) & 0xFFu);
	p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint16_t kv_get_u16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint32_t kv_rec_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int kv_type_ok(uint8_t type)
{
	return type == KV_TYPE_STR || type == KV_TYPE_U32 ||
	       type == KV_TYPE_BOOL || type == KV_TYPE_BYTES;
}

const char *kv_type_name(uint8_t type)
{
	switch (type) {
	case KV_TYPE_STR:   return "str";
	case KV_TYPE_U32:   return "u32";
	case KV_TYPE_BOOL:  return "bool";
	case KV_TYPE_BYTES: return "bytes";
	default:            return "?";
	}
}

/* Types that fix their own length are checked here, so a u32 can never be stored
 * as three bytes and read back as garbage later. */
static int kv_value_len_ok(uint8_t type, uint32_t len)
{
	if (len == 0u || len > KV_VALUE_MAX)
		return 0;
	if (type == KV_TYPE_U32)
		return len == 4u;
	if (type == KV_TYPE_BOOL)
		return len == 1u;
	return 1;
}

/*
 * Content check for the types that constrain their own bytes, applied on the way
 * in AND on the way out.  Only KV_TYPE_BOOL does: kv.h promises callers a value of
 * 0 or 1, and kv_get_bool() hands back `data[0] != 0`, so a stored 2 would read as
 * a perfectly ordinary `true` -- a record this firmware could not have written,
 * accepted as if it had.  Checking the length alone would let that through.
 */
int kv_value_bytes_ok(uint8_t type, const uint8_t *val, uint32_t len)
{
	if (!kv_value_len_ok(type, len))
		return 0;
	if (type == KV_TYPE_BOOL)
		return val[0] <= 1u;
	return 1;
}

uint32_t kv_rec_pack(uint8_t *out, uint8_t type, const void *val, uint32_t vlen,
                     const char *desc)
{
	uint32_t dlen = (desc != NULL) ? (uint32_t)strlen(desc) : 0u;
	uint32_t total;

	if (!kv_type_ok(type) || !kv_value_bytes_ok(type, val, vlen) ||
	    dlen > KV_DESC_MAX)
		return 0u;
	total = KV_REC_HDR + dlen + vlen;

	kv_put_u32(out + 0, KV_REC_MAGIC);
	out[4] = (uint8_t)KV_REC_VER;
	out[5] = type;
	kv_put_u16(out + 6, (uint16_t)dlen);
	kv_put_u32(out + 8, total);
	if (dlen != 0u)
		memcpy(out + KV_REC_HDR, desc, dlen);
	memcpy(out + KV_REC_HDR + dlen, val, vlen);
	return total;
}

/*
 * THE ORDER OF THESE CHECKS IS LOAD-BEARING.  Every length here is unsigned, so a
 * subtraction that goes negative wraps to something enormous instead of failing --
 * which is exactly how a corrupt record turns into an out-of-bounds read.  So each
 * step only subtracts quantities an earlier step has already proved large enough:
 * the header is present before any field is read, total_len is confirmed to equal
 * the real length before anything is subtracted from it, and desc_len is bounded
 * against the remainder before the value length is derived from it.
 */
int kv_rec_unpack(const uint8_t *raw, uint32_t rawlen, struct kv_value *out)
{
	uint32_t total, dlen, vlen;
	uint8_t type;

	out->type = KV_TYPE_INVALID;
	out->len = 0u;
	out->data[0] = '\0';
	out->desc[0] = '\0';

	if (rawlen < KV_REC_HDR)                        /* 1: header readable? */
		return KV_ERR_FORMAT;
	if (kv_rec_u32(raw) != KV_REC_MAGIC)            /* 2: ours?            */
		return KV_ERR_FORMAT;
	if (raw[4] != (uint8_t)KV_REC_VER)
		return KV_ERR_FORMAT;
	type = raw[5];
	if (!kv_type_ok(type))
		return KV_ERR_FORMAT;
	total = kv_rec_u32(raw + 8);
	if (total != rawlen)                            /* 3: self-consistent? */
		return KV_ERR_FORMAT;
	dlen = kv_get_u16(raw + 6);
	if (dlen > total - KV_REC_HDR)                  /* 4: safe to subtract */
		return KV_ERR_FORMAT;
	vlen = total - KV_REC_HDR - dlen;               /* 5: now derivable    */
	if (dlen > KV_DESC_MAX ||
	    !kv_value_bytes_ok(type, raw + KV_REC_HDR + dlen, vlen))
		return KV_ERR_FORMAT;

	out->type = type;
	out->len = vlen;
	memcpy(out->data, raw + KV_REC_HDR + dlen, vlen);
	out->data[vlen] = '\0';
	if (dlen != 0u)
		memcpy(out->desc, raw + KV_REC_HDR, dlen);
	out->desc[dlen] = '\0';
	return KV_OK;
}
