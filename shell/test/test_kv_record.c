/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    test_kv_record.c
 * @brief   Host test of the key-value store's record format and its key and
 *          value checks (svc/kv/kv_record.c, issue #214).
 *
 * Every edge the decoder has to hold: the header is read only when it is all
 * there, the magic, the version and the type are each refused on their own, the
 * stored total must be the real length, the description length is bounded against
 * the rest before the value length is derived from it (an unsigned wrap there is
 * an out-of-bounds read), the value's length and bytes fit the type, and the two
 * maxima (64 B of description, 256 B of value) are inside while one more is not.
 * The encoder is pinned byte for byte, since records already on a board's flash
 * have to keep decoding.  Key checks: 1..31 characters of [A-Za-z0-9._-].
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kv.h"
#include "kv_record.h"

static int failures;

#define CHECK(cond)                                                         \
	do {                                                                    \
		if (!(cond)) {                                                      \
			fprintf(stderr, "test_kv_record: %s:%d: %s\n", __FILE__,        \
			        __LINE__, #cond);                                       \
			failures++;                                                     \
		}                                                                   \
	} while (0)

static uint8_t rec[KV_REC_MAX + 16u];

/* Write a header by hand, independent of kv_rec_pack(). */
static void hdr(uint8_t *p, uint32_t magic, uint8_t ver, uint8_t type,
                uint16_t dlen, uint32_t total)
{
	p[0] = (uint8_t)magic; p[1] = (uint8_t)(magic >> 8);
	p[2] = (uint8_t)(magic >> 16); p[3] = (uint8_t)(magic >> 24);
	p[4] = ver;
	p[5] = type;
	p[6] = (uint8_t)dlen; p[7] = (uint8_t)(dlen >> 8);
	p[8] = (uint8_t)total; p[9] = (uint8_t)(total >> 8);
	p[10] = (uint8_t)(total >> 16); p[11] = (uint8_t)(total >> 24);
}

/* An unpack that failed must leave nothing behind. */
static int emptied(const struct kv_value *v)
{
	return v->type == KV_TYPE_INVALID && v->len == 0u &&
	       v->data[0] == '\0' && v->desc[0] == '\0';
}

static void test_keys(void)
{
	char k[KV_KEY_MAX + 2];
	const char bad[] = " /:\t\x01\x7f\xc3";
	size_t i;

	CHECK(!kv_key_ok(NULL));
	CHECK(!kv_key_ok(""));
	CHECK(kv_key_ok("a"));
	CHECK(kv_key_ok("net.shell.port"));
	CHECK(kv_key_ok("A-Z_0.9"));
	memset(k, 'x', KV_KEY_MAX);
	k[KV_KEY_MAX] = '\0';
	CHECK(kv_key_ok(k));                      /* 31: the longest */
	k[KV_KEY_MAX] = 'x';
	k[KV_KEY_MAX + 1] = '\0';
	CHECK(!kv_key_ok(k));                     /* 32: one too many */
	for (i = 0; i < sizeof bad - 1u; i++) {
		char t[4] = { 'a', bad[i], 'b', '\0' };

		CHECK(!kv_key_ok(t));
		t[0] = bad[i];
		CHECK(!kv_key_ok(t));                 /* also as the first */
	}
	/* KV_KEY_MAX is FlashDB's name buffer less its terminator. */
	CHECK(KV_KEY_MAX == 31);
}

static void test_types(void)
{
	uint8_t b0 = 0u, b1 = 1u, b2 = 2u, four[4] = { 1, 2, 3, 4 };
	uint8_t big[KV_VALUE_MAX + 1u];
	unsigned t;

	for (t = 0u; t < 256u; t++)
		CHECK(kv_type_ok((uint8_t)t) == (t <= 3u));
	CHECK(strcmp(kv_type_name(KV_TYPE_STR), "str") == 0);
	CHECK(strcmp(kv_type_name(KV_TYPE_U32), "u32") == 0);
	CHECK(strcmp(kv_type_name(KV_TYPE_BOOL), "bool") == 0);
	CHECK(strcmp(kv_type_name(KV_TYPE_BYTES), "bytes") == 0);
	CHECK(strcmp(kv_type_name(KV_TYPE_INVALID), "?") == 0);
	CHECK(strcmp(kv_type_name(4u), "?") == 0);

	memset(big, 'v', sizeof big);
	CHECK(kv_value_bytes_ok(KV_TYPE_BOOL, &b0, 1u));
	CHECK(kv_value_bytes_ok(KV_TYPE_BOOL, &b1, 1u));
	CHECK(!kv_value_bytes_ok(KV_TYPE_BOOL, &b2, 1u));
	CHECK(!kv_value_bytes_ok(KV_TYPE_BOOL, four, 2u));
	CHECK(kv_value_bytes_ok(KV_TYPE_U32, four, 4u));
	CHECK(!kv_value_bytes_ok(KV_TYPE_U32, four, 3u));
	CHECK(!kv_value_bytes_ok(KV_TYPE_U32, big, 5u));
	CHECK(!kv_value_bytes_ok(KV_TYPE_STR, big, 0u));
	CHECK(!kv_value_bytes_ok(KV_TYPE_BYTES, big, 0u));
	CHECK(kv_value_bytes_ok(KV_TYPE_STR, big, KV_VALUE_MAX));
	CHECK(!kv_value_bytes_ok(KV_TYPE_STR, big, KV_VALUE_MAX + 1u));
	CHECK(kv_value_bytes_ok(KV_TYPE_BYTES, big, KV_VALUE_MAX));
	CHECK(!kv_value_bytes_ok(KV_TYPE_BYTES, big, KV_VALUE_MAX + 1u));
	CHECK(kv_rec_u32(four) == 0x04030201u);
}

static void test_pack_bytes(void)
{
	static const uint8_t want[] = {
		0x57, 0x4B, 0x56, 0x31,     /* 'W' 'K' 'V' '1' */
		0x01,                       /* version          */
		0x01,                       /* KV_TYPE_U32      */
		0x02, 0x00,                 /* desc_len 2       */
		0x12, 0x00, 0x00, 0x00,     /* total 18         */
		'h', 'i',
		0x78, 0x56, 0x34, 0x12,
	};
	uint8_t v[4] = { 0x78, 0x56, 0x34, 0x12 };
	struct kv_value out;

	memset(rec, 0xA5, sizeof rec);
	CHECK(kv_rec_pack(rec, KV_TYPE_U32, v, 4u, "hi") == sizeof want);
	CHECK(memcmp(rec, want, sizeof want) == 0);
	CHECK(rec[sizeof want] == 0xA5);          /* nothing past the record */
	CHECK(kv_rec_unpack(rec, sizeof want, &out) == KV_OK);
	CHECK(out.type == KV_TYPE_U32 && out.len == 4u);
	CHECK(kv_rec_u32(out.data) == 0x12345678u);
	CHECK(strcmp(out.desc, "hi") == 0);

	/* No description: a NULL and "" both store none. */
	CHECK(kv_rec_pack(rec, KV_TYPE_STR, "x", 1u, NULL) == KV_REC_HDR + 1u);
	CHECK(rec[6] == 0 && rec[7] == 0);
	CHECK(kv_rec_pack(rec, KV_TYPE_STR, "x", 1u, "") == KV_REC_HDR + 1u);
}

static void test_pack_refusals(void)
{
	char d[KV_DESC_MAX + 2];
	uint8_t v[KV_VALUE_MAX + 1u];
	uint8_t two = 2u;
	struct kv_value out;
	uint32_t n;

	memset(v, 'q', sizeof v);
	memset(d, 'd', KV_DESC_MAX);
	d[KV_DESC_MAX] = '\0';
	n = kv_rec_pack(rec, KV_TYPE_BYTES, v, KV_VALUE_MAX, d);
	CHECK(n == KV_REC_MAX);                   /* both maxima: the biggest record */
	CHECK(kv_rec_unpack(rec, n, &out) == KV_OK);
	CHECK(out.len == KV_VALUE_MAX && strlen(out.desc) == KV_DESC_MAX);
	CHECK(out.data[KV_VALUE_MAX] == '\0');

	d[KV_DESC_MAX] = 'd';
	d[KV_DESC_MAX + 1] = '\0';
	CHECK(kv_rec_pack(rec, KV_TYPE_BYTES, v, 1u, d) == 0u);   /* desc 65 */
	CHECK(kv_rec_pack(rec, KV_TYPE_BYTES, v, KV_VALUE_MAX + 1u, NULL) == 0u);
	CHECK(kv_rec_pack(rec, KV_TYPE_BYTES, v, 0u, NULL) == 0u);
	CHECK(kv_rec_pack(rec, KV_TYPE_BOOL, &two, 1u, NULL) == 0u);
	CHECK(kv_rec_pack(rec, KV_TYPE_U32, v, 2u, NULL) == 0u);
	CHECK(kv_rec_pack(rec, 4u, v, 1u, NULL) == 0u);
	CHECK(kv_rec_pack(rec, KV_TYPE_INVALID, v, 1u, NULL) == 0u);
}

static void test_unpack_edges(void)
{
	struct kv_value out;
	uint32_t n;

	/* A good record to break one field at a time: str "abc", desc "de". */
	n = kv_rec_pack(rec, KV_TYPE_STR, "abc", 3u, "de");
	CHECK(n == KV_REC_HDR + 5u);
	CHECK(kv_rec_unpack(rec, n, &out) == KV_OK);
	CHECK(strcmp((const char *)out.data, "abc") == 0 && strcmp(out.desc, "de") == 0);

	/* 1: a header shorter than 12 bytes is never read into. */
	for (uint32_t len = 0u; len < KV_REC_HDR; len++) {
		memset(&out, 0x5A, sizeof out);
		CHECK(kv_rec_unpack(rec, len, &out) == KV_ERR_FORMAT);
		CHECK(emptied(&out));
	}
	/* 2: magic, version, type -- each alone. */
	hdr(rec, KV_REC_MAGIC ^ 1u, KV_REC_VER, KV_TYPE_STR, 2u, n);
	CHECK(kv_rec_unpack(rec, n, &out) == KV_ERR_FORMAT && emptied(&out));
	hdr(rec, KV_REC_MAGIC, KV_REC_VER + 1u, KV_TYPE_STR, 2u, n);
	CHECK(kv_rec_unpack(rec, n, &out) == KV_ERR_FORMAT && emptied(&out));
	hdr(rec, KV_REC_MAGIC, 0u, KV_TYPE_STR, 2u, n);
	CHECK(kv_rec_unpack(rec, n, &out) == KV_ERR_FORMAT);
	hdr(rec, KV_REC_MAGIC, KV_REC_VER, 4u, 2u, n);
	CHECK(kv_rec_unpack(rec, n, &out) == KV_ERR_FORMAT && emptied(&out));
	/* 3: the stored total must be the length handed in, both ways. */
	hdr(rec, KV_REC_MAGIC, KV_REC_VER, KV_TYPE_STR, 2u, n + 1u);
	CHECK(kv_rec_unpack(rec, n, &out) == KV_ERR_FORMAT);
	hdr(rec, KV_REC_MAGIC, KV_REC_VER, KV_TYPE_STR, 2u, n - 1u);
	CHECK(kv_rec_unpack(rec, n, &out) == KV_ERR_FORMAT);
	hdr(rec, KV_REC_MAGIC, KV_REC_VER, KV_TYPE_STR, 2u, n);
	CHECK(kv_rec_unpack(rec, n + 1u, &out) == KV_ERR_FORMAT);
	/* 4: a description longer than what follows the header: no wrap. */
	hdr(rec, KV_REC_MAGIC, KV_REC_VER, KV_TYPE_STR, (uint16_t)(n - KV_REC_HDR + 1u), n);
	CHECK(kv_rec_unpack(rec, n, &out) == KV_ERR_FORMAT && emptied(&out));
	hdr(rec, KV_REC_MAGIC, KV_REC_VER, KV_TYPE_STR, 0xFFFFu, n);
	CHECK(kv_rec_unpack(rec, n, &out) == KV_ERR_FORMAT && emptied(&out));
	/* ... all of it description leaves a zero-length value: refused. */
	hdr(rec, KV_REC_MAGIC, KV_REC_VER, KV_TYPE_STR, (uint16_t)(n - KV_REC_HDR), n);
	CHECK(kv_rec_unpack(rec, n, &out) == KV_ERR_FORMAT);
	/* A header alone (no value at all). */
	hdr(rec, KV_REC_MAGIC, KV_REC_VER, KV_TYPE_STR, 0u, KV_REC_HDR);
	CHECK(kv_rec_unpack(rec, KV_REC_HDR, &out) == KV_ERR_FORMAT);

	/* 5: the value's own rules, on the way out. */
	{
		uint8_t b = 1u;

		n = kv_rec_pack(rec, KV_TYPE_BOOL, &b, 1u, NULL);
		CHECK(kv_rec_unpack(rec, n, &out) == KV_OK && out.data[0] == 1u);
		rec[KV_REC_HDR] = 2u;                 /* a bool this firmware never wrote */
		CHECK(kv_rec_unpack(rec, n, &out) == KV_ERR_FORMAT && emptied(&out));
		hdr(rec, KV_REC_MAGIC, KV_REC_VER, KV_TYPE_U32, 0u, KV_REC_HDR + 3u);
		CHECK(kv_rec_unpack(rec, KV_REC_HDR + 3u, &out) == KV_ERR_FORMAT);
	}
	/* A description over 64 B that does fit the record is still refused. */
	{
		static uint8_t r[KV_REC_HDR + KV_DESC_MAX + 2u];
		uint32_t tot = (uint32_t)sizeof r;

		memset(r, 'z', sizeof r);
		hdr(r, KV_REC_MAGIC, KV_REC_VER, KV_TYPE_STR, KV_DESC_MAX + 1u, tot);
		CHECK(kv_rec_unpack(r, tot, &out) == KV_ERR_FORMAT && emptied(&out));
		hdr(r, KV_REC_MAGIC, KV_REC_VER, KV_TYPE_STR, KV_DESC_MAX, tot);
		CHECK(kv_rec_unpack(r, tot, &out) == KV_OK && out.len == 2u);
	}
	/* A value over 256 B with no description is refused. */
	{
		static uint8_t r[KV_REC_HDR + KV_VALUE_MAX + 1u];

		memset(r, 'y', sizeof r);
		hdr(r, KV_REC_MAGIC, KV_REC_VER, KV_TYPE_BYTES, 0u, (uint32_t)sizeof r);
		CHECK(kv_rec_unpack(r, (uint32_t)sizeof r, &out) == KV_ERR_FORMAT);
		hdr(r, KV_REC_MAGIC, KV_REC_VER, KV_TYPE_BYTES, 0u, (uint32_t)sizeof r - 1u);
		CHECK(kv_rec_unpack(r, (uint32_t)sizeof r - 1u, &out) == KV_OK);
		CHECK(out.len == KV_VALUE_MAX);
	}
	/* Zeros inside a BYTES value survive, with len the authority. */
	{
		uint8_t z[3] = { 0u, 7u, 0u };

		n = kv_rec_pack(rec, KV_TYPE_BYTES, z, 3u, NULL);
		CHECK(kv_rec_unpack(rec, n, &out) == KV_OK && out.len == 3u);
		CHECK(memcmp(out.data, z, 3u) == 0 && out.data[3] == '\0');
	}
}

int main(void)
{
	test_keys();
	test_types();
	test_pack_bytes();
	test_pack_refusals();
	test_unpack_edges();
	if (failures != 0) {
		fprintf(stderr, "test_kv_record: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_kv_record: OK\n");
	return 0;
}
