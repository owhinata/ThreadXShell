/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blob_write.c
 * @brief   The `blob write` coordinator (#92).
 *
 * No hardware and no shell: everything it does to the world goes through the
 * vtable in blob_write.h, so test/test_blob_write.c compiles this exact file on
 * the host and fails each operation in turn.  What it is really checking is the
 * unwinding -- see that header for why that is the part worth a test.
 */
#include "blob_write.h"

#include <string.h>

#include "blob_map.h"     /* BLOB_MAX_SLOTS -- the views array bound */
#include "blob_stage.h"
#include "crc32.h"

/* The run's own state.  The sink callbacks get this as their ctx, which is how
 * bytes arriving from the protocol reach the staging buffer and the writer. */
struct wr {
	const struct blob_write_ops *ops;
	struct blob_write_report    *rep;
	struct blob_write            bk;   /* the phase bookkeeping (blob_state) */
	uint32_t token;
	uint32_t payload_addr;
	uint32_t staged;                   /* bytes sitting in the staging buffer */
	uint32_t programmed;               /* bytes already in the flash          */
	int      broken;                   /* sticky: no more programs, no header */
	/* Told apart from @ref broken because they mean different things to an
	 * operator: a program that did not take is the flash, and a rejected or
	 * overrunning transfer is the sender. */
	int      prog_failed;
};

const char *blob_write_result_name(enum blob_write_result r)
{
	switch (r) {
	case BLOB_WRITE_STORED:        return "stored";
	case BLOB_WRITE_BUSY:          return "the NOR is somebody else's just now";
	case BLOB_WRITE_NO_CONSOLE:    return "the console could not be claimed";
	case BLOB_WRITE_CANCELLED:     return "cancelled during the erase";
	case BLOB_WRITE_ERASE_FAILED:  return "the erase did not finish";
	case BLOB_WRITE_XFER_FAILED:   return "the transfer did not complete";
	case BLOB_WRITE_PROG_FAILED:   return "a program did not take";
	case BLOB_WRITE_VERIFY_FAILED: return "the read-back does not match";
	case BLOB_WRITE_NO_SLOT:       return "no slot to write into";
	case BLOB_WRITE_REFUSED:       return "refused";
	default:                       break;
	}
	return "?";
}

/* Hand the staging buffer to the writer.  [!] The transaction is counted BEFORE
 * the call, so a failed attempt is counted too: the count is a proxy for how
 * many times the part's status register was written, and a failure writes it
 * just as a success does. */
static int flush(struct wr *w)
{
	int rc;

	if (w->staged == 0u)
		return 0;
	w->rep->transactions++;
	rc = w->ops->program(w->ops->ctx, w->token,
	                     w->payload_addr + w->programmed,
	                     blob_stage_buf, w->staged);
	w->rep->vendor_rc = rc;
	if (rc != 0) {
		w->broken = 1;
		w->prog_failed = 1;
		blob_write_break(&w->bk);
		return -1;
	}
	w->programmed += w->staged;
	w->staged = 0u;
	return 0;
}

/* ---- the sink the protocol drives ----------------------------------------- */

static int sink_begin(void *ctx, const char *name, uint32_t size)
{
	struct wr *w = (struct wr *)ctx;

	if (w->ops->note_sender_name != NULL)
		w->ops->note_sender_name(w->ops->ctx, name, size);
	w->rep->declared = size;
	/* [!] THE SENDER'S FILENAME IS NOT THE KEY.  It is noted for the log and
	 * dropped; what gets stored is the name the operator typed, so that the
	 * thing `blob list` shows is the thing they asked for. */
	if (blob_write_begin(&w->bk, size) != BLOB_WR_OK) {
		w->broken = 1;
		return -1;   /* rejects the file: the receiver cancels the batch */
	}
	return 0;
}

static int sink_write(void *ctx, const uint8_t *data, uint32_t len)
{
	struct wr *w = (struct wr *)ctx;
	uint32_t off = 0u;

	if (w->broken)
		return -1;
	/* Accounting and the payload CRC first, so that an overrun is refused
	 * before any of it reaches the flash. */
	if (blob_write_data(&w->bk, data, len) != BLOB_WR_OK) {
		w->broken = 1;
		return -1;
	}
	while (off < len) {
		uint32_t room = BLOB_STAGE_BYTES - w->staged;
		uint32_t take = len - off;

		if (take > room)
			take = room;
		memcpy(&blob_stage_buf[w->staged], data + off, take);
		w->staged += take;
		off += take;
		if (w->staged == BLOB_STAGE_BYTES && flush(w) != 0)
			return -1;
	}
	return 0;
}

/* ---- the header ------------------------------------------------------------ */

/* Body first, magic second, each its own transaction and each read back by the
 * writer.  Written together they would be one program that could land the magic
 * over a half-formed body: a slot that looks valid and is not. */
static int write_header(struct wr *w, const char *name, uint32_t base)
{
	struct blob_info info;

	memset(&info, 0, sizeof info);
	if (strlen(name) > BLOB_NAME_MAX)
		return -1;
	memcpy(info.name, name, strlen(name));
	info.length = w->bk.received;
	info.crc32  = w->bk.crc;

	if (blob_hdr_encode_body(blob_stage_buf, BLOB_STAGE_BYTES, base,
	                         w->bk.capacity, &info) != BLOB_REJECT_NONE)
		return -1;
	w->rep->transactions++;
	if (w->ops->program(w->ops->ctx, w->token, base + NOR_PROGRAM_PAGE,
	                    blob_stage_buf, BLOB_BODY_SIZE) != 0)
		return -1;

	if (blob_hdr_encode_magic(blob_stage_buf, BLOB_STAGE_BYTES) !=
	    BLOB_REJECT_NONE)
		return -1;
	w->rep->transactions++;
	if (w->ops->program(w->ops->ctx, w->token, base, blob_stage_buf,
	                    BLOB_MAGIC_SIZE) != 0)
		return -1;
	return 0;
}

/* Re-read the payload and check it against the CRC of the stream that arrived.
 * [!] Against the STREAM, not against the flash: a CRC taken from a read-back
 * would compare the flash with itself and pass however badly the write went. */
static int verify_payload(struct wr *w)
{
	uint32_t off = 0u, crc = 0u;

	while (off < w->bk.received) {
		uint32_t take = w->bk.received - off;

		if (take > BLOB_STAGE_BYTES)
			take = BLOB_STAGE_BYTES;
		if (w->ops->read_back(w->ops->ctx, w->payload_addr + off,
		                      blob_stage_buf, take) != 0)
			return -1;
		crc = crc32_update(crc, blob_stage_buf, take);
		off += take;
	}
	w->rep->verified = crc;
	return (crc == w->bk.crc) ? 0 : -1;
}

/* ---- the run --------------------------------------------------------------- */

enum blob_write_result blob_write_run(const struct blob_write_ops *ops,
                                      const char *name, int want,
                                      struct blob_write_report *rep)
{
	struct blob_slot_view views[BLOB_MAX_SLOTS];
	struct ym_sink sink;
	struct wr w;
	enum blob_write_result res;
	uint32_t base = 0u, payload_addr = 0u, payload_max = 0u, erased = 0u;
	unsigned count, i, target = 0u;
	int cancelled = 0, claimed = 0, rc;

	if (ops == NULL || rep == NULL || name == NULL)
		return BLOB_WRITE_REFUSED;
	memset(rep, 0, sizeof *rep);
	memset(&w, 0, sizeof w);
	w.ops = ops;
	w.rep = rep;

	count = ops->slot_count(ops->ctx);
	if (count == 0u || count > BLOB_MAX_SLOTS) {
		rep->choice = BLOB_CHOICE_BAD_MAP;
		return BLOB_WRITE_NO_SLOT;
	}

	/* [!] LOCAL UNTIL IT IS COMMITTED.  Nothing below returns without going
	 * through the one exit, so the token is given back exactly once -- and a
	 * refused reserve never produces one to give back. */
	if (ops->reserve(ops->ctx, &w.token) != 0 || w.token == 0u)
		return BLOB_WRITE_BUSY;

	/* Every decision from here is made on a part nobody else can write. */
	for (i = 0u; i < count; i++) {
		struct blob_info info;

		if (ops->stat(ops->ctx, i, &info) != 0) {
			res = BLOB_WRITE_REFUSED;
			goto out;
		}
		views[i].state = info.state;
		views[i].name_match = (uint8_t)(info.state == BLOB_VALID &&
		                                strcmp(info.name, name) == 0);
	}
	rep->choice = blob_choose_target(views, count, want, &target);
	if (rep->choice != BLOB_CHOICE_REUSE && rep->choice != BLOB_CHOICE_FRESH) {
		res = BLOB_WRITE_NO_SLOT;
		goto out;
	}
	rep->slot = target;
	if (ops->geometry(ops->ctx, target, &base, &payload_addr,
	                  &payload_max) != 0) {
		res = BLOB_WRITE_REFUSED;
		goto out;
	}
	w.payload_addr = payload_addr;
	blob_write_arm(&w.bk, target, payload_max);

	if (ops->announce != NULL)
		ops->announce(ops->ctx, target, base,
		              (payload_addr - base) + payload_max);

	/* The whole slot, one transaction, cancellable between sectors.  ~40 s
	 * for 2 MB on this die, which is why it is here and not inside the
	 * protocol's timeouts. */
	rep->transactions++;
	/* The WHOLE slot: the header unit and the payload.  Derived from the two
	 * addresses rather than taking the erase unit as a third parameter, so
	 * there is no way for this to disagree with the map. */
	rc = ops->erase(ops->ctx, w.token, base,
	                (payload_addr - base) + payload_max, &erased, &cancelled);
	rep->erased = erased;
	rep->vendor_rc = rc;
	if (cancelled) {
		res = BLOB_WRITE_CANCELLED;
		goto out;
	}
	/* [!] ONLY AN OK ERASE MAY BE PROGRAMMED INTO.  Bits only go 1 -> 0, so
	 * programming over an erase that stopped early reads back as the AND of
	 * the two -- which nor_write.c latches as a TERMINAL fault.  A perfectly
	 * ordinary interruption would kill the port for the session. */
	if (rc != 0) {
		res = BLOB_WRITE_ERASE_FAILED;
		goto out;
	}

	/* After the erase: the erase wants a live line editor for its cancel, and
	 * nobody should be handed a raw console for the 40 s it can take. */
	if (ops->claim_console(ops->ctx) != 0) {
		res = BLOB_WRITE_NO_CONSOLE;
		goto out;
	}
	claimed = 1;

	sink.ctx   = &w;
	sink.begin = sink_begin;
	sink.write = sink_write;
	rc = ops->receive(ops->ctx, &sink);
	rep->received = w.bk.received;
	rep->crc      = w.bk.crc;
	if (rc != 0 || w.broken) {
		res = w.prog_failed ? BLOB_WRITE_PROG_FAILED
		                    : BLOB_WRITE_XFER_FAILED;
		goto out;
	}
	/* The batch closed.  Everything declared has to have arrived, or the
	 * length in the header would describe a payload that is not there. */
	if (blob_write_close(&w.bk) != BLOB_WR_OK) {
		res = BLOB_WRITE_XFER_FAILED;
		goto out;
	}
	if (flush(&w) != 0) {
		res = BLOB_WRITE_PROG_FAILED;
		goto out;
	}
	rep->received = w.bk.received;
	rep->crc      = w.bk.crc;

	/* The console goes back before the header: what follows prints, and the
	 * protocol is over. */
	ops->release_console(ops->ctx);
	claimed = 0;

	/* [!] The commit check is REDUNDANT HERE and kept anyway.  Every path to
	 * this line has already been through the three gates that could leave the
	 * bookkeeping unfinished -- a transfer that reported an error, a close
	 * that found fewer bytes than were declared, and a final flush that did
	 * not take -- so a mutation that deletes it changes nothing this test can
	 * see.  It stays because the phase machine is what says whether a header
	 * may be written, and this file should ASK rather than conclude that from
	 * its own control flow. */
	if (blob_write_commit_check(&w.bk) != BLOB_WR_OK ||
	    write_header(&w, name, base) != 0) {
		res = BLOB_WRITE_PROG_FAILED;
		goto out;
	}
	res = (verify_payload(&w) == 0) ? BLOB_WRITE_STORED
	                                : BLOB_WRITE_VERIFY_FAILED;

out:
	/* THE ONE EXIT.  Both claims are given back here and nowhere else, in the
	 * order they were taken in reverse, and each only if it was taken. */
	if (claimed)
		ops->release_console(ops->ctx);
	ops->unreserve(ops->ctx, w.token);
	blob_write_reset(&w.bk);
	return res;
}
