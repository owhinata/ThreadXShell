/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for this board's blob store read side, src/blob.c (issue #194),
 * compiled unchanged.
 *
 * WHAT IT IS FOR.  Since issue #194 the three boards' stores answer the shared
 * results of svc/blob/blob_err.h, and this board's numbers moved to fit them.
 * What must not move with them is the CLASSIFICATION this file makes, which no
 * console can stage on demand:
 *
 *   - a lease that is refused while the part is merely in use (bring-up by
 *     another caller, a writer's reservation) is BUSY -- try again;
 *   - a lease that is refused because the port is FAULTED is FAULT -- terminal,
 *     a reset is required, and calling it busy would have a caller retrying
 *     forever;
 *   - a read of the window with no reservation out (the _reserved forms) or
 *     with a token that is not live (the _leased forms) is BUSY, never a read;
 *   - no slot is PARAM, a header with nothing to check is EMPTY, a payload
 *     that does not add up is CRC;
 *
 * each from all three entry points that can meet it, and every lease taken is
 * given back.
 *
 * [!] THE MEMORY-MAPPED WINDOW IS REAL MEMORY HERE.  src/blob.c reads the
 * flash through NOR_XIP_BASE + offset, a fixed address; the test maps 16 MB of
 * anonymous memory at exactly that address (MAP_FIXED_NOREPLACE, so it never
 * replaces anything of its own) and lays headers and payloads into it.  If the
 * mapping is refused the test FAILS rather than skipping the reads.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "blob.h"
#include "blob_map.h"
#include "blob_stage.h"
#include "blob_state.h"
#include "crc32.h"
#include "nor_flash.h"
#include "nor_seam.h"

static int failures;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
			failures++;                                            \
		}                                                              \
	} while (0)

#define WINDOW_BYTES  0x01000000u

/* ---- the stand-ins for port/nor ------------------------------------------- */

/* The interval the seam would hand the store (board.cmake: NOR_PART_FW_END,
 * NOR_PART_BLOB_END, NOR_ERASE_GRAN -- passed on the compile line, as the
 * firmware's are). */
const struct nor_seam_limits nor_seam_limits = {
	NOR_WRITABLE_LO, NOR_WRITABLE_HI, NOR_ERASE_UNIT
};

#define TOKEN  0x00010003u

static uint8_t *win;                    /* NOR_XIP_BASE, mapped */
static enum nor_state state = NOR_ST_XIP;
static int grant = 1;                   /* nor_acquire() hands out a lease */
static int grant_after = -1;            /* refuse from this acquire on     */
static uint32_t reservation;            /* nor_reservation_owner()         */
static uint32_t live_token;             /* the token nor_lease_held() likes */
static unsigned acquires, releases, refused, invalidates;

int nor_acquire(enum nor_lease_slot slot, uint32_t *out)
{
	CHECK(slot == NOR_LEASE_BLOB, "a lease for slot %d", (int)slot);
	if (!grant || (grant_after >= 0 && (int)acquires >= grant_after)) {
		refused++;
		*out = 0u;
		return -1;
	}
	acquires++;
	*out = TOKEN;
	return 0;
}

int nor_release(uint32_t token)
{
	CHECK(token == TOKEN, "released %08X", (unsigned)token);
	releases++;
	return 0;
}

int nor_lease_held(uint32_t token)
{
	return token != 0u && token == live_token;
}

enum nor_state nor_lifecycle_state(void)
{
	return state;
}

uint32_t nor_reservation_owner(void)
{
	return reservation;
}

void nor_alias_invalidate(uint32_t off, uint32_t len)
{
	(void)off;
	(void)len;
	invalidates++;
}

static void reset(void)
{
	state = NOR_ST_XIP;
	grant = 1;
	grant_after = -1;
	reservation = 0u;
	live_token = 0u;
	acquires = releases = refused = invalidates = 0u;
}

static void check_balanced(const char *what)
{
	CHECK(acquires == releases, "%s: %u lease(s) taken, %u given back", what,
	      acquires, releases);
}

/* Lay a finished header for @p name over @p len payload bytes at @p slot. */
static void put_blob(unsigned slot, const char *name, uint32_t len)
{
	const struct blob_slot *s = blob_map_slot(slot);
	uint32_t pa = blob_map_payload_addr(s, nor_seam_limits.unit), i;
	uint8_t page[BLOB_HDR_PAGE];
	struct blob_info info;

	memset(&win[s->base], 0xFF, nor_seam_limits.unit);
	for (i = 0u; i < len; i++)
		win[pa + i] = (uint8_t)(i * 2654435761u >> 24);
	memset(&info, 0, sizeof info);
	memcpy(info.name, name, strlen(name));
	info.length = len;
	info.crc32 = crc32_update(0u, &win[pa], len);
	CHECK(blob_hdr_encode_body(page, sizeof page, s->base,
	                           blob_map_payload_max(s, nor_seam_limits.unit),
	                           &info) == BLOB_REJECT_NONE, "body encode");
	memcpy(&win[s->base + BLOB_HDR_PAGE], page, BLOB_HDR_PAGE);
	CHECK(blob_hdr_encode_magic(page, sizeof page) == BLOB_REJECT_NONE,
	      "magic encode");
	memcpy(&win[s->base], page, BLOB_HDR_PAGE);
}

/* ---- the path that works --------------------------------------------------- */

static void t_ok(void)
{
	struct blob_info info;
	uint32_t computed = 0u;
	uint8_t b[16];

	reset();
	CHECK(blob_check_map(NULL) == BLOB_MAP_OK, "the shipped table is refused");
	put_blob(2u, "model", 3000u);
	CHECK(blob_stat(2u, &info, NULL) == BLOB_OK && info.state == BLOB_VALID &&
	      strcmp(info.name, "model") == 0, "a stored blob did not stat");
	CHECK(blob_read(2u, 0u, b, sizeof b) == BLOB_OK &&
	      b[1] == (uint8_t)(2654435761u >> 24), "a payload read failed");
	CHECK(blob_verify(2u, &info, &computed) == BLOB_OK &&
	      computed == info.crc32, "a matching payload did not verify");
	CHECK(blob_stat(3u, &info, NULL) == BLOB_OK && info.state == BLOB_EMPTY,
	      "an erased slot did not read EMPTY");
	CHECK(blob_verify(3u, &info, &computed) == BLOB_ERR_EMPTY,
	      "an erased slot verified");
	win[blob_map_payload_addr(blob_map_slot(2u), nor_seam_limits.unit) +
	    2999u] ^= 1u;
	CHECK(blob_verify(2u, &info, &computed) == BLOB_ERR_CRC,
	      "a flipped last byte verified");
	CHECK(invalidates > 0u, "nothing was invalidated before a read");
	check_balanced("the path that works");
}

/* ---- the classification ---------------------------------------------------- */

/* [!] REFUSED BECAUSE SOMEBODY HAS IT, OR BECAUSE NOBODY EVER CAN.  The same
 * refused nor_acquire() from all three leasing entry points, in each state the
 * lifecycle can be in when it refuses. */
static void t_lease_refused(void)
{
	static const struct {
		enum nor_state st; int want; const char *what;
	} c[] = {
		{ NOR_ST_ENABLING, BLOB_ERR_BUSY,  "bring-up by another caller" },
		{ NOR_ST_RESERVED, BLOB_ERR_BUSY,  "a writer's reservation"     },
		{ NOR_ST_XIP,      BLOB_ERR_BUSY,  "every lease out"            },
		{ NOR_ST_FAULTED,  BLOB_ERR_FAULT, "the port faulted"           },
	};
	struct blob_info info;
	uint32_t computed;
	uint8_t b[4];
	unsigned i;
	int rc;

	for (i = 0u; i < sizeof c / sizeof c[0]; i++) {
		reset();
		put_blob(1u, "x", 100u);
		state = c[i].st;
		grant = 0;
		rc = blob_stat(1u, &info, NULL);
		CHECK(rc == c[i].want, "%s: blob_stat answered %d, not %d",
		      c[i].what, rc, c[i].want);
		rc = blob_read(1u, 0u, b, sizeof b);
		CHECK(rc == c[i].want, "%s: blob_read answered %d, not %d",
		      c[i].what, rc, c[i].want);
		rc = blob_verify(1u, &info, &computed);
		CHECK(rc == c[i].want, "%s: blob_verify answered %d, not %d",
		      c[i].what, rc, c[i].want);
		/* The caller holds the window; only the staging claim is refused. */
		live_token = TOKEN;
		rc = blob_verify_leased(1u, TOKEN, &info, &computed);
		CHECK(rc == c[i].want, "%s: blob_verify_leased answered %d, not %d",
		      c[i].what, rc, c[i].want);
		CHECK(refused == 4u && acquires == 0u,
		      "%s: %u refused, %u granted", c[i].what, refused, acquires);
		check_balanced(c[i].what);
	}
}

/* A read of the window that nothing is holding up is issue #90: refused BUSY,
 * never performed -- and never FAULT, even on a faulted port, because the
 * refusal is about the caller's claim, not the part. */
static void t_no_claim(void)
{
	const struct blob_slot *s = blob_map_slot(1u);
	uint32_t pa = blob_map_payload_addr(s, nor_seam_limits.unit), computed;
	struct blob_info info;
	uint8_t b[4] = { 0, 0, 0, 0 };
	unsigned k;

	for (k = 0u; k < 2u; k++) {
		reset();
		put_blob(1u, "x", 100u);
		state = k ? NOR_ST_FAULTED : NOR_ST_XIP;
		invalidates = 0u;
		CHECK(blob_stat_reserved(1u, &info, NULL) == BLOB_ERR_BUSY,
		      "a reserved stat with no reservation was not BUSY");
		CHECK(blob_read_reserved(pa, b, sizeof b) == BLOB_ERR_BUSY &&
		      b[0] == 0u, "a reserved read with no reservation was not BUSY");
		CHECK(blob_stat_leased(1u, TOKEN, &info, NULL) == BLOB_ERR_BUSY,
		      "a leased stat on a dead token was not BUSY");
		CHECK(blob_verify_leased(1u, TOKEN, &info, &computed) ==
		      BLOB_ERR_BUSY, "a leased verify on a dead token was not BUSY");
		CHECK(invalidates == 0u && acquires == 0u,
		      "a refused read still touched the window or took a lease");
	}

	/* With the claim out, the same calls read. */
	reset();
	put_blob(1u, "x", 100u);
	reservation = 1u;
	CHECK(blob_stat_reserved(1u, &info, NULL) == BLOB_OK &&
	      info.state == BLOB_VALID, "a reserved stat failed");
	CHECK(blob_read_reserved(pa, b, sizeof b) == BLOB_OK &&
	      b[1] == (uint8_t)(2654435761u >> 24), "a reserved read failed");
	reservation = 0u;
	live_token = TOKEN;
	CHECK(blob_stat_leased(1u, TOKEN, &info, NULL) == BLOB_OK,
	      "a leased stat failed");
	CHECK(blob_verify_leased(1u, TOKEN, &info, &computed) == BLOB_OK &&
	      computed == info.crc32, "a leased verify failed");
	check_balanced("claims out");
}

static void t_param(void)
{
	const unsigned n = blob_map_count();
	const struct blob_slot *s = blob_map_slot(0u);
	uint32_t pm = blob_map_payload_max(s, nor_seam_limits.unit), computed;
	struct blob_info info;
	uint8_t b[4];

	reset();
	CHECK(blob_stat(n, &info, NULL) == BLOB_ERR_PARAM &&
	      blob_read(n, 0u, b, 1u) == BLOB_ERR_PARAM &&
	      blob_verify(n, &info, &computed) == BLOB_ERR_PARAM &&
	      blob_stat_leased(n, TOKEN, &info, NULL) == BLOB_ERR_PARAM,
	      "a slot past the table was not PARAM");
	CHECK(blob_stat(0u, NULL, NULL) == BLOB_ERR_PARAM,
	      "a NULL out was not PARAM");
	CHECK(blob_read(0u, pm, b, 1u) == BLOB_ERR_PARAM &&
	      blob_read(0u, pm - 2u, b, 4u) == BLOB_ERR_PARAM &&
	      blob_read(0u, 0u, b, 0xFFFFFFFFu) == BLOB_ERR_PARAM,
	      "a read outside the payload was not PARAM");
	reservation = 1u;
	CHECK(blob_read_reserved(nor_seam_limits.lo - 4u, b, 4u) == BLOB_ERR_PARAM &&
	      blob_read_reserved(nor_seam_limits.hi - 2u, b, 4u) == BLOB_ERR_PARAM,
	      "a reserved read outside the interval was not PARAM");
	/* PARAM is decided before a lease is asked for. */
	CHECK(acquires == 0u && refused == 0u, "a PARAM refusal took a lease");
}

int main(void)
{
	void *p = mmap((void *)(uintptr_t)NOR_XIP_BASE, WINDOW_BYTES,
	               PROT_READ | PROT_WRITE,
	               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);

	if (p != (void *)(uintptr_t)NOR_XIP_BASE) {
		printf("FAIL: could not map the window at 0x%08X\n",
		       (unsigned)NOR_XIP_BASE);
		printf("test_blob_store: 1 failure(s)\n");
		return 1;
	}
	win = (uint8_t *)p;
	memset(win, 0xFF, WINDOW_BYTES);

	t_ok();
	t_lease_refused();
	t_no_claim();
	t_param();

	if (failures != 0) {
		printf("test_blob_store: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_blob_store: all passed\n");
	return 0;
}
