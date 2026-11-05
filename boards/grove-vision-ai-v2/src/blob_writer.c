/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blob_writer.c
 * @brief   The asset store's write side, for the shared `blob` command
 *          (svc/blob/blob_svc.h, issue #194).
 *
 * Moved here from cmds/cmd_blob.c when the command became shared, and taking no
 * console: the reservation, the slot erase and the program, each a thin wrap of
 * port/nor's writer.  [!] NOTHING HERE REACHES THE VENDOR'S ERASE OR PROGRAM.
 * Every write goes through nor_write_erase() / nor_write_program(), whose one
 * transaction (claim, window down, JEDEC re-read, operation, window up, read
 * back, commit) is port/nor/nor_write.c's, and that file stays the only
 * authorised caller of the seam (cmake/check_nor_seam.py's --allow-caller).
 * What this file adds is the mapping from the writer's statuses to the shared
 * results and the log lines a transfer leaves behind (the console belongs to
 * the sender while a write runs).
 */
#include "blob.h"

#include "blob_stage.h"   /* blob_stage_buf -- what `blob write` stages in */
#include "blob_svc.h"
#include "log.h"
#include "nor_flash.h"
#include "nor_seam.h"
#include "nor_write.h"

uint8_t *blob_svc_stage(uint32_t *len)
{
	*len = BLOB_STAGE_BYTES;
	return blob_stage_buf;
}

void blob_svc_interval(uint32_t *lo, uint32_t *hi, uint32_t *unit)
{
	*lo = nor_seam_limits.lo;
	*hi = nor_seam_limits.hi;
	*unit = nor_seam_limits.unit;
}

/* The port comes up on first use -- the first lease brings it up -- so there is
 * no "never came up" to ask about before a write; a bring-up that failed is a
 * faulted port, and the reservation says so. */
int blob_svc_ready(void)
{
	return 1;
}

static int refusal(void)
{
	return nor_lifecycle_state() == NOR_ST_FAULTED ? BLOB_ERR_FAULT
	                                               : BLOB_ERR_BUSY;
}

/*
 * The writer reservation (#91), after bringing the port up if nobody has --
 * nor_reserve() starts from an up window, and the first lease is what brings
 * it up.  Refused while a reader holds the window or another writer the part
 * (BUSY), or for good on a faulted port (FAULT): told apart because only one of
 * them is worth retrying.
 */
int blob_svc_reserve(uint32_t *token)
{
	uint32_t lease = 0u;

	*token = 0u;
	if (nor_acquire(NOR_LEASE_BLOB, &lease) != 0)
		return refusal();
	(void)nor_release(lease);
	if (nor_reserve(token) != 0) {
		*token = 0u;
		return refusal();
	}
	return BLOB_OK;
}

void blob_svc_unreserve(uint32_t token)
{
	(void)nor_unreserve(token);
}

/* The writer's status as a shared result.  [!] Only FAULTED is terminal: a
 * refusal before the wire (REFUSED) and a part that did not answer the JEDEC
 * read (NO_TRANSPORT) are not faults, and neither is a vendor that gave up part
 * way (INCOMPLETE) -- the port's own rules (nor_write.h), kept as they are. */
static int result_of(enum nor_write_status st)
{
	switch (st) {
	case NOR_WRITE_OK:      return BLOB_OK;
	case NOR_WRITE_BUSY:    return BLOB_ERR_BUSY;
	case NOR_WRITE_FAULTED: return BLOB_ERR_FAULT;
	default:                break;
	}
	return BLOB_ERR_IO;
}

int blob_svc_erase_header(uint32_t token, unsigned slot)
{
	struct nor_write_report r;
	enum nor_write_status st;
	uint32_t base = 0u;
	int rc;

	rc = blob_slot_geometry(slot, &base, NULL, NULL);
	if (rc != BLOB_OK)
		return rc;
	/* [!] THE HEADER UNIT ONLY: one sector, one transaction. */
	st = nor_write_erase(token, base, nor_seam_limits.unit, NULL, &r);
	if (st != NOR_WRITE_OK)
		log_write(LOG_LEVEL_ERR, "blob", "erase slot %u header 0x%lx: %s",
		          slot, (unsigned long)base, nor_write_status_name(st));
	return result_of(st);
}

int blob_svc_erase_span(uint32_t token, uint32_t addr, uint32_t len,
                        blob_svc_tick_fn tick, void *ctx, uint32_t *done,
                        int *cancelled)
{
	/* The cancel point is INSIDE the one transaction (nor_write.h says why);
	 * the tick runs with the window down and must not read the store. */
	struct nor_erase_progress prog = { ctx, tick };
	struct nor_write_report r;
	enum nor_write_status st = nor_write_erase(token, addr, len, &prog, &r);

	*done = r.done;
	*cancelled = r.cancelled ? 1 : 0;
	if (st != NOR_WRITE_OK)
		log_write(LOG_LEVEL_ERR, "blob", "erase 0x%lx+%lu: %s (%lu done)",
		          (unsigned long)addr, (unsigned long)len,
		          nor_write_status_name(st), (unsigned long)r.done);
	return (int)st;
}

int blob_svc_program(uint32_t token, uint32_t addr, const void *data,
                     uint32_t len)
{
	struct nor_write_report r;
	enum nor_write_status st = nor_write_program(token, addr, data, len, &r);

	if (st != NOR_WRITE_OK) {
		log_write(LOG_LEVEL_ERR, "blob", "program 0x%lx+%lu: %s (%lu done)",
		          (unsigned long)addr, (unsigned long)len,
		          nor_write_status_name(st), (unsigned long)r.done);
		/* [!] THE MISMATCHING BYTE, not just the verdict.  This runs with
		 * the console handed to the protocol, so the log is the only place
		 * it can be said -- and "read 0x%02x, wanted 0x%02x" at a known
		 * offset is the difference between knowing what went wrong and
		 * spending a flash cycle guessing. */
		if (r.bad_valid)
			log_write(LOG_LEVEL_ERR, "blob",
			          "first bad at 0x%lx: read %02x, wanted %02x",
			          (unsigned long)r.bad_off, r.bad_got, r.bad_want);
	}
	return (int)st;
}

int blob_svc_read_back(uint32_t addr, void *buf, uint32_t len)
{
	return blob_read_reserved(addr, buf, len) == BLOB_OK ? 0 : -1;
}

int blob_svc_stat_reserved(unsigned slot, struct blob_info *out)
{
	return blob_stat_reserved(slot, out, NULL);
}
