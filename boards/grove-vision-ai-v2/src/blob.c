/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blob.c
 * @brief   The asset store's read side (#92).
 *
 * Thin on purpose.  The decisions are in blob_map.c and blob_state.c, where a
 * host test can walk them; what is left here is the part that needs the board:
 * the lease, the invalidate, and the address arithmetic that turns a slot into
 * a pointer into the memory-mapped window.  See blob.h for why each read takes
 * a lease and invalidates before it looks.
 */
#include "blob.h"

#include <string.h>

#include "blob_stage.h"  /* the buffer verify reads through            */
#include "crc32.h"
#include "nor_flash.h"   /* nor_acquire / nor_release / nor_alias_invalidate */
#include "nor_seam.h"    /* nor_seam_limits -- the interval, from the seam    */

/* The one place a slot becomes an address.  Reading the alias through a plain
 * pointer rather than a volatile one is deliberate: the lease is out, so
 * nothing else may write this flash or take the window down, and the decoder
 * walks the bytes several times over -- an invalidate before the read is what
 * makes them current, not a qualifier that would only stop the compiler
 * caching them in registers. */
static const uint8_t *alias(uint32_t off)
{
	return (const uint8_t *)(NOR_XIP_BASE + off);
}

enum blob_map_verdict blob_check_map(unsigned *bad)
{
	return blob_map_check(nor_seam_limits.lo, nor_seam_limits.hi,
	                      nor_seam_limits.unit, blob_map_table(),
	                      blob_map_count(), bad);
}

/* Why an acquire was refused.  nor_acquire() answers -1 for a part somebody
 * else is using and for a part nobody can use again, and only the second calls
 * for a reset. */
static int acquire_failure(void)
{
	if (nor_lifecycle_state() == NOR_ST_FAULTED)
		return BLOB_ERR_FAULT;
	return BLOB_ERR_BUSY;
}

/* The slot, once the table has been checked and the index is in range. */
static const struct blob_slot *checked_slot(unsigned slot, int *err)
{
	const struct blob_slot *s;

	if (blob_check_map(NULL) != BLOB_MAP_OK) {
		*err = BLOB_ERR_MAP;
		return NULL;
	}
	s = blob_map_slot(slot);
	if (s == NULL) {
		*err = BLOB_ERR_PARAM;
		return NULL;
	}
	return s;
}

uint32_t blob_payload_addr(unsigned slot)
{
	int err = 0;
	const struct blob_slot *s = checked_slot(slot, &err);

	return s ? blob_map_payload_addr(s, nor_seam_limits.unit) : 0u;
}

uint32_t blob_payload_max(unsigned slot)
{
	int err = 0;
	const struct blob_slot *s = checked_slot(slot, &err);

	return s ? blob_map_payload_max(s, nor_seam_limits.unit) : 0u;
}

/* The decode itself, with the window already held by somebody -- a lease or a
 * reservation.  Both wrappers below are the same three lines around it, and
 * having one copy is what keeps the invalidate from being forgotten in one of
 * them. */
static void stat_locked(const struct blob_slot *s, struct blob_info *out,
                        enum blob_hdr_reject *why)
{
	nor_alias_invalidate(s->base, BLOB_HDR_SPAN);
	(void)blob_hdr_decode(alias(s->base), BLOB_HDR_SPAN, s->base,
	                      blob_map_payload_max(s, nor_seam_limits.unit),
	                      out, why);
}

int blob_stat(unsigned slot, struct blob_info *out, enum blob_hdr_reject *why)
{
	const struct blob_slot *s;
	uint32_t token = 0u;
	int err = 0;

	if (out == NULL)
		return BLOB_ERR_PARAM;
	s = checked_slot(slot, &err);
	if (s == NULL)
		return err;

	if (nor_acquire(NOR_LEASE_BLOB, &token) != 0)
		return acquire_failure();
	stat_locked(s, out, why);
	(void)nor_release(token);
	return BLOB_OK;
}

int blob_stat_reserved(unsigned slot, struct blob_info *out,
                       enum blob_hdr_reject *why)
{
	const struct blob_slot *s;
	int err = 0;

	if (out == NULL)
		return BLOB_ERR_PARAM;
	s = checked_slot(slot, &err);
	if (s == NULL)
		return err;
	/* Checked for the same reason blob_read_reserved() checks: a read of the
	 * alias with neither a lease nor a reservation is issue #90. */
	if (nor_reservation_owner() == 0u)
		return BLOB_ERR_BUSY;
	stat_locked(s, out, why);
	return BLOB_OK;
}

int blob_read(unsigned slot, uint32_t off, void *buf, uint32_t len)
{
	const struct blob_slot *s;
	uint32_t token = 0u, cap, addr;
	int err = 0;

	if (buf == NULL || len == 0u)
		return BLOB_ERR_PARAM;
	s = checked_slot(slot, &err);
	if (s == NULL)
		return err;

	/* Subtraction, so a length near 2^32 cannot wrap into looking as though
	 * it fits -- the same discipline as nor_span.c, and for the same reason:
	 * this is what decides which flash gets read. */
	cap = blob_map_payload_max(s, nor_seam_limits.unit);
	if (off >= cap || len > cap - off)
		return BLOB_ERR_PARAM;
	addr = blob_map_payload_addr(s, nor_seam_limits.unit) + off;

	if (nor_acquire(NOR_LEASE_BLOB, &token) != 0)
		return acquire_failure();

	nor_alias_invalidate(addr, len);
	memcpy(buf, alias(addr), len);

	/* The lease goes back before the caller prints.  Printing can block for
	 * as long as the console's flow control takes, and holding the window
	 * for that would refuse `nn` for a reason that has nothing to do with
	 * the flash. */
	(void)nor_release(token);
	return BLOB_OK;
}

int blob_slot_geometry(unsigned slot, uint32_t *base, uint32_t *payload_addr,
                       uint32_t *payload_max)
{
	const struct blob_slot *s;
	int err = 0;

	s = checked_slot(slot, &err);
	if (s == NULL)
		return err;
	if (base != NULL)
		*base = s->base;
	if (payload_addr != NULL)
		*payload_addr = blob_map_payload_addr(s, nor_seam_limits.unit);
	if (payload_max != NULL)
		*payload_max = blob_map_payload_max(s, nor_seam_limits.unit);
	return BLOB_OK;
}

int blob_read_reserved(uint32_t addr, void *buf, uint32_t len)
{
	if (buf == NULL || len == 0u)
		return BLOB_ERR_PARAM;
	/* Checked, not assumed.  Without a reservation out, nothing is holding
	 * the window up and this would be the #90 read all over again. */
	if (nor_reservation_owner() == 0u)
		return BLOB_ERR_BUSY;
	if (addr < nor_seam_limits.lo || len > nor_seam_limits.hi - addr)
		return BLOB_ERR_PARAM;

	nor_alias_invalidate(addr, len);
	memcpy(buf, alias(addr), len);
	return BLOB_OK;
}

/* The CRC walk, with the window held by somebody and the BLOB lease -- which is
 * what makes blob_stage_buf single-user -- already taken.  Both entry points
 * below are the same acquire around this, and having one copy is what keeps the
 * invalidate-per-chunk from being forgotten in one of them. */
static int verify_locked(const struct blob_slot *s, struct blob_info *out,
                         uint32_t *computed)
{
	struct blob_info info;
	uint32_t off = 0u, crc = 0u, addr;

	stat_locked(s, &info, NULL);
	if (info.state != BLOB_VALID && info.state != BLOB_INCOMPLETE) {
		if (out != NULL)
			*out = info;
		return BLOB_ERR_EMPTY;
	}

	addr = blob_map_payload_addr(s, nor_seam_limits.unit);
	while (off < info.length) {
		uint32_t take = info.length - off;

		if (take > BLOB_STAGE_BYTES)
			take = BLOB_STAGE_BYTES;
		nor_alias_invalidate(addr + off, take);
		memcpy(blob_stage_buf, alias(addr + off), take);
		crc = crc32_update(crc, blob_stage_buf, take);
		off += take;
	}

	if (computed != NULL)
		*computed = crc;
	if (out != NULL)
		*out = info;
	return (crc == info.crc32) ? BLOB_OK : BLOB_ERR_CRC;
}

int blob_verify(unsigned slot, struct blob_info *out, uint32_t *computed)
{
	const struct blob_slot *s;
	uint32_t token = 0u;
	int err = 0, rc;

	s = checked_slot(slot, &err);
	if (s == NULL)
		return err;

	/* [!] ONE LEASE FOR BOTH HALVES.  Reading the header under one lease and
	 * the payload under another leaves a window in between for a background
	 * `nor erase` or a `blob write` to change the slot -- and the result of
	 * that race is a PASS against a header that is no longer there.  The
	 * caller gets back the header this actually checked against, so what it
	 * prints and what was compared cannot drift either. */
	if (nor_acquire(NOR_LEASE_BLOB, &token) != 0)
		return acquire_failure();
	rc = verify_locked(s, out, computed);
	(void)nor_release(token);
	return rc;
}

int blob_stat_leased(unsigned slot, uint32_t token, struct blob_info *out,
                     enum blob_hdr_reject *why)
{
	const struct blob_slot *s;
	int err = 0;

	if (out == NULL)
		return BLOB_ERR_PARAM;
	s = checked_slot(slot, &err);
	if (s == NULL)
		return err;
	/* Checked for the same reason blob_stat_reserved() checks its
	 * reservation: a read of the alias that nothing is holding up is
	 * issue #90.  A token that is not live is the caller's mistake, not
	 * contention, but BUSY is the only refusal the read side has for "the
	 * window is not yours" and inventing a second one here would give the
	 * three entry points different vocabularies for the same failure. */
	if (!nor_lease_held(token))
		return BLOB_ERR_BUSY;
	stat_locked(s, out, why);
	return BLOB_OK;
}

int blob_verify_leased(unsigned slot, uint32_t token, struct blob_info *out,
                       uint32_t *computed)
{
	const struct blob_slot *s;
	uint32_t stage_token = 0u;
	int err = 0, rc;

	s = checked_slot(slot, &err);
	if (s == NULL)
		return err;
	if (!nor_lease_held(token))
		return BLOB_ERR_BUSY;

	/* [!] NOT FOR THE WINDOW -- the caller's token is holding that up.  This
	 * is the claim on blob_stage_buf: the buffer has one user at a time, and
	 * on the read side the thing that arranges it is this lease being
	 * single-instance.  Without it a `blob verify` and an `nn open` would walk
	 * different slots through the same 64 KB. */
	if (nor_acquire(NOR_LEASE_BLOB, &stage_token) != 0)
		return acquire_failure();
	rc = verify_locked(s, out, computed);
	(void)nor_release(stage_token);
	return rc;
}
