/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for this board's blob store write side, src/blob_writer.c (issue
 * #194), compiled unchanged against stand-ins for port/nor and the read side.
 *
 * The same subject as wio-lite-ai's and f746g-disco's test_blob_writer.c, cut
 * to what this board's writer decides itself -- it is thin wraps of
 * port/nor/nor_write.c, whose transaction is test_nor_write.c's:
 *
 *   - [!] WHY A RESERVATION IS REFUSED.  A lease or a reservation refused
 *     while the port is merely in use (another reader, a bring-up, another
 *     writer) is BUSY -- try again; refused because the port is FAULTED it is
 *     FAULT -- terminal, a reset is required, and calling it busy would have
 *     the operator retrying forever.  From both refusal points, and from every
 *     state that is not FAULTED.
 *   - [!] WHICH WRITER RESULTS ARE FAULTS.  The header erase maps the writer's
 *     status to a shared result: OK is OK, BUSY is BUSY, FAULTED is FAULT, and
 *     a refusal before the wire (REFUSED), a part that did not answer
 *     (NO_TRANSPORT) and a vendor that stopped part way (INCOMPLETE) are IO --
 *     NOT faults, by the port's own rules (nor_write.h).  The slot erase and
 *     the program return the writer's status as it is (blob_svc.h: the
 *     coordinator logs it and treats anything but 0 as "stop").
 *   - the lease the reservation brings the port up with is given back exactly
 *     once whether the reservation is then had or refused; a refused one hands
 *     out token 0; an unreserve is one nor_unreserve() of that token.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "blob.h"
#include "blob_err.h"
#include "blob_svc.h"
#include "nor_flash.h"
#include "nor_seam.h"
#include "nor_write.h"

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

/* ---- the stand-ins --------------------------------------------------------- */

const struct nor_seam_limits nor_seam_limits = {
	0x00200000u, 0x00FFE000u, 0x00001000u
};
uint8_t blob_stage_buf[64u * 1024u];

#define LEASE  0x00010003u
#define RESV   0x00020005u

static enum nor_state state = NOR_ST_XIP;
static int lease_ok = 1, reserve_ok = 1;
static enum nor_write_status write_st = NOR_WRITE_OK;
static unsigned acquires, releases, reserves, unreserves, erases, programs;
static uint32_t unreserved_token, erase_addr, erase_len, erase_token;
static unsigned logs;

int nor_acquire(enum nor_lease_slot slot, uint32_t *out)
{
	CHECK(slot == NOR_LEASE_BLOB, "a lease for slot %d", (int)slot);
	if (!lease_ok) {
		*out = 0u;
		return -1;
	}
	acquires++;
	*out = LEASE;
	return 0;
}

int nor_release(uint32_t token)
{
	CHECK(token == LEASE, "released %08X", (unsigned)token);
	releases++;
	return 0;
}

int nor_reserve(uint32_t *out)
{
	/* nor_reserve() starts from an up window: the lease must be back. */
	CHECK(acquires == releases, "reserved with a lease still out");
	if (!reserve_ok) {
		*out = 0x5A5A5A5Au;   /* garbage the writer must not hand on */
		return -1;
	}
	reserves++;
	*out = RESV;
	return 0;
}

int nor_unreserve(uint32_t token)
{
	unreserves++;
	unreserved_token = token;
	return 0;
}

enum nor_state nor_lifecycle_state(void)
{
	return state;
}

enum nor_write_status nor_write_erase(uint32_t token, uint32_t addr,
                                      uint32_t len,
                                      const struct nor_erase_progress *prog,
                                      struct nor_write_report *r)
{
	(void)prog;
	erases++;
	erase_token = token;
	erase_addr = addr;
	erase_len = len;
	memset(r, 0, sizeof *r);
	r->done = (write_st == NOR_WRITE_OK) ? len : 0u;
	return write_st;
}

enum nor_write_status nor_write_program(uint32_t token, uint32_t addr,
                                        const void *data, uint32_t len,
                                        struct nor_write_report *r)
{
	(void)token; (void)addr; (void)data;
	programs++;
	memset(r, 0, sizeof *r);
	r->done = (write_st == NOR_WRITE_OK) ? len : 0u;
	return write_st;
}

const char *nor_write_status_name(enum nor_write_status s)
{
	return s == NOR_WRITE_OK ? "ok" : "not ok";
}

/* The read side, as far as the writer reaches it. */
int blob_slot_geometry(unsigned slot, uint32_t *base, uint32_t *payload_addr,
                       uint32_t *payload_max)
{
	if (slot > 10u)
		return BLOB_ERR_PARAM;
	if (base)         *base = 0x00200000u + slot * 0x00100000u;
	if (payload_addr) *payload_addr = 0x00201000u + slot * 0x00100000u;
	if (payload_max)  *payload_max = 0x000FF000u;
	return BLOB_OK;
}

int blob_read_reserved(uint32_t addr, void *buf, uint32_t len)
{
	(void)addr; (void)buf; (void)len;
	return BLOB_OK;
}

int blob_stat_reserved(unsigned slot, struct blob_info *out,
                       enum blob_hdr_reject *why)
{
	(void)slot; (void)why;
	memset(out, 0, sizeof *out);
	return BLOB_OK;
}

void log_write(unsigned level, const char *tag, const char *fmt, ...)
{
	(void)level; (void)tag; (void)fmt;
	logs++;
}

static void reset(void)
{
	state = NOR_ST_XIP;
	lease_ok = reserve_ok = 1;
	write_st = NOR_WRITE_OK;
	acquires = releases = reserves = unreserves = erases = programs = 0u;
	unreserved_token = erase_addr = erase_len = erase_token = 0u;
	logs = 0u;
}

/* ---- the reservation ------------------------------------------------------- */

static void t_reserve(void)
{
	static const enum nor_state not_faulted[] = {
		NOR_ST_OFF, NOR_ST_ENABLING, NOR_ST_XIP, NOR_ST_RESERVED,
		NOR_ST_WRITING,
	};
	uint32_t token;
	unsigned i;
	int rc;

	reset();
	rc = blob_svc_reserve(&token);
	CHECK(rc == BLOB_OK && token == RESV, "reserve: rc %d token %08X", rc,
	      (unsigned)token);
	CHECK(acquires == 1u && releases == 1u && reserves == 1u,
	      "the bring-up lease once and back once: %u / %u / %u", acquires,
	      releases, reserves);
	blob_svc_unreserve(token);
	CHECK(unreserves == 1u && unreserved_token == RESV,
	      "unreserve: %u of %08X", unreserves, (unsigned)unreserved_token);

	/* [!] The two refusal points, in every state: FAULT only when FAULTED. */
	for (i = 0u; i <= sizeof not_faulted / sizeof not_faulted[0]; i++) {
		enum nor_state st = i < sizeof not_faulted / sizeof not_faulted[0]
		                    ? not_faulted[i] : NOR_ST_FAULTED;
		int want = st == NOR_ST_FAULTED ? BLOB_ERR_FAULT : BLOB_ERR_BUSY;

		reset();
		state = st;
		lease_ok = 0;
		token = 0xFFFFFFFFu;
		rc = blob_svc_reserve(&token);
		CHECK(rc == want && token == 0u && reserves == 0u &&
		      releases == 0u,
		      "lease refused in state %d: rc %d (want %d) token %08X",
		      (int)st, rc, want, (unsigned)token);

		reset();
		state = st;
		reserve_ok = 0;
		token = 0xFFFFFFFFu;
		rc = blob_svc_reserve(&token);
		CHECK(rc == want && token == 0u,
		      "reservation refused in state %d: rc %d (want %d) token "
		      "%08X", (int)st, rc, want, (unsigned)token);
		CHECK(acquires == 1u && releases == 1u,
		      "a refused reservation still gives the lease back once: "
		      "%u / %u", acquires, releases);
		CHECK(unreserves == 0u, "nothing to unreserve after a refusal");
	}
}

/* ---- the writer's results -------------------------------------------------- */

static void t_results(void)
{
	static const struct {
		enum nor_write_status st;
		int want;
		const char *what;
	} map[] = {
		{ NOR_WRITE_OK,           BLOB_OK,        "OK" },
		{ NOR_WRITE_BUSY,         BLOB_ERR_BUSY,  "BUSY" },
		{ NOR_WRITE_FAULTED,      BLOB_ERR_FAULT, "FAULTED" },
		/* [!] none of these is a fault (nor_write.h) */
		{ NOR_WRITE_REFUSED,      BLOB_ERR_IO,    "REFUSED before the wire" },
		{ NOR_WRITE_NO_TRANSPORT, BLOB_ERR_IO,    "NO_TRANSPORT" },
		{ NOR_WRITE_INCOMPLETE,   BLOB_ERR_IO,    "INCOMPLETE" },
	};
	uint32_t done = 0u;
	int cancelled = 0;
	unsigned i;
	int rc;

	for (i = 0u; i < sizeof map / sizeof map[0]; i++) {
		reset();
		write_st = map[i].st;
		rc = blob_svc_erase_header(RESV, 3u);
		CHECK(rc == map[i].want, "header erase, writer %s: rc %d, want %d",
		      map[i].what, rc, map[i].want);
		CHECK(erases == 1u && erase_token == RESV &&
		      erase_addr == 0x00500000u && erase_len == 0x00001000u,
		      "header erase, writer %s: one erase of the header unit "
		      "(%u at %08X+%u)", map[i].what, erases, (unsigned)erase_addr,
		      (unsigned)erase_len);
		CHECK((map[i].st == NOR_WRITE_OK) == (logs == 0u),
		      "header erase, writer %s: logged %u", map[i].what, logs);

		/* The span erase and the program hand the status on as it is. */
		reset();
		write_st = map[i].st;
		rc = blob_svc_erase_span(RESV, 0x00500000u, 0x00100000u, NULL, NULL,
		                         &done, &cancelled);
		CHECK(rc == (int)map[i].st && cancelled == 0,
		      "span erase, writer %s: rc %d", map[i].what, rc);
		reset();
		write_st = map[i].st;
		rc = blob_svc_program(RESV, 0x00501000u, blob_stage_buf, 256u);
		CHECK(rc == (int)map[i].st && programs == 1u,
		      "program, writer %s: rc %d", map[i].what, rc);
	}

	/* A slot that does not exist is not erased at all. */
	reset();
	rc = blob_svc_erase_header(RESV, 11u);
	CHECK(rc == BLOB_ERR_PARAM && erases == 0u, "no such slot: rc %d, %u "
	      "erase(s)", rc, erases);
}

int main(void)
{
	t_reserve();
	t_results();
	if (failures != 0) {
		printf("test_blob_writer_grove: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_blob_writer_grove: all passed\n");
	return 0;
}
