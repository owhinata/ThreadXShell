/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the camera's holder table (issue #142, svc/cam_holders.c).
 *
 * WHY THIS EXISTS.  The table decides when a camera starts and stops on three
 * boards, and the cases that matter most cannot be typed on purpose: a start
 * that fails while someone else is trying to register, a rollback that must
 * not take a holder that was already there, an operator stop while nn holds
 * the stream.  Each answer is spelled out here, against the one function.
 *
 * THE PARTS.
 *   1. registration: idempotent, attribute mismatch, full, argument checks.
 *   2. transitions: empty -> non-empty is START, the last one out is STOP,
 *      anything else is NONE.
 *   3. D8: the operator (keeps-stream) is refused while nn is there, and
 *      stays; two keeps-stream holders do not refuse each other.
 *   4. add_or_start: what it added, and that the rollback undoes only that.
 *   5. the claim: every add and remove refused (retryable) while a start is
 *      in flight, commit / abort, and both refused with no claim.
 *   6. the list: order, the cap, attributes.
 *   7. dispositions: every code, and an unknown one fails closed.
 *   8. the late undo (issue #142 review F1): an undo after the join committed
 *      removes only the registrations its ticket names -- never one made
 *      after a clear, nor one the operator renewed on top of the join.
 *
 * [!] WHAT IT DOES NOT COVER.  The lock, and that a board acts on START / STOP
 * in the same locked region, are the board's.  So is "an empty table is not a
 * teardown": nothing here can see a resource.
 */
#include <stdio.h>
#include <string.h>

#include "cam_holders.h"

static int fails;

#define CHECK(cond, what)                                                    \
	do {                                                                 \
		if (!(cond)) {                                               \
			printf("  FAIL %s:%d %s\n", __func__, __LINE__, what); \
			fails++;                                             \
		}                                                            \
	} while (0)

#define EQ(got, want, what)                                                  \
	do {                                                                 \
		long g_ = (long)(got), w_ = (long)(want);                    \
		if (g_ != w_) {                                              \
			printf("  FAIL %s:%d %s: got %ld, wanted %ld\n",     \
			       __func__, __LINE__, what, g_, w_);            \
			fails++;                                             \
		}                                                            \
	} while (0)

static const struct cam_holder manual = { "manual" };
static const struct cam_holder nn = { "nn" };
static const struct cam_holder other = { "other" };
static const struct cam_holder fourth = { "fourth" };
static const struct cam_holder fifth = { "fifth" };
/* Same name as nn, different holder: identity is the pointer. */
static const struct cam_holder nn_twin = { "nn" };

#define KEEP CAM_HOLDER_KEEPS_STREAM

/* Bring a table to "manual is streaming" through the real path. */
static void streaming_manual(struct cam_holders *t)
{
	cam_holders_init(t);
	EQ(cam_holders_add(t, &manual, KEEP), CAM_HOLDERS_T_START, "manual starts");
	EQ(cam_holders_commit(t), CAM_HOLDERS_T_NONE, "start committed");
}

/* ---- 1. registration ---------------------------------------------------- */

static void test_registration(void)
{
	struct cam_holders t;
	struct cam_holders zero;
	unsigned before;

	/* A zero-initialised table is an empty, unclaimed one. */
	memset(&zero, 0, sizeof zero);
	EQ(cam_holders_count(&zero), 0, "zeroed is empty");
	EQ(cam_holders_claimed(&zero), 0, "zeroed is unclaimed");

	streaming_manual(&t);
	EQ(cam_holders_add(&t, &manual, KEEP), CAM_HOLDERS_T_NONE, "second add");
	EQ(cam_holders_count(&t), 1, "idempotent: still one");
	EQ(cam_holders_add(&t, &nn, 0), CAM_HOLDERS_T_NONE, "nn joins");
	EQ(cam_holders_add(&t, &nn, 0), CAM_HOLDERS_T_NONE, "nn again");
	EQ(cam_holders_count(&t), 2, "idempotent: two");

	/* The pointer is the identity, not the name. */
	EQ(cam_holders_add(&t, &nn_twin, 0), CAM_HOLDERS_T_NONE, "twin is new");
	EQ(cam_holders_count(&t), 3, "twin counted");
	CHECK(cam_holders_has(&t, &nn_twin) && cam_holders_has(&t, &nn),
	      "both registered");

	/* A second registration that changes the attribute is refused. */
	before = cam_holders_count(&t);
	EQ(cam_holders_add(&t, &manual, 0), CAM_HOLDERS_E_INVAL,
	   "manual re-registered without keeps");
	EQ(cam_holders_add(&t, &nn, KEEP), CAM_HOLDERS_E_INVAL,
	   "nn re-registered with keeps");
	EQ(cam_holders_count(&t), before, "mismatch changed nothing");

	/* Full: MAX holders fit, the next is refused and changes nothing. */
	EQ(cam_holders_add(&t, &fourth, 0), CAM_HOLDERS_T_NONE, "fourth fits");
	EQ(cam_holders_count(&t), CAM_HOLDERS_MAX, "now full");
	EQ(cam_holders_add(&t, &fifth, 0), CAM_HOLDERS_E_FULL, "fifth refused");
	EQ(cam_holders_count(&t), CAM_HOLDERS_MAX, "full changed nothing");
	CHECK(!cam_holders_has(&t, &fifth), "fifth not there");
	/* ... but a holder already there is still a no-op, not FULL. */
	EQ(cam_holders_add(&t, &nn, 0), CAM_HOLDERS_T_NONE, "dup when full");
	/* One leaves, one more fits. */
	EQ(cam_holders_remove(&t, &fourth), CAM_HOLDERS_T_NONE, "fourth leaves");
	EQ(cam_holders_add(&t, &fifth, 0), CAM_HOLDERS_T_NONE, "fifth fits now");

	/* Arguments. */
	{
		static const struct cam_holder nameless = { NULL };

		EQ(cam_holders_add(&t, NULL, 0), CAM_HOLDERS_E_INVAL, "NULL");
		EQ(cam_holders_add(&t, &nameless, 0), CAM_HOLDERS_E_INVAL,
		   "no name");
		EQ(cam_holders_add(&t, &other, 0x80u), CAM_HOLDERS_E_INVAL,
		   "unknown attribute");
		EQ(cam_holders_add(NULL, &other, 0), CAM_HOLDERS_E_INVAL,
		   "NULL table");
		EQ(cam_holders_remove(&t, NULL), CAM_HOLDERS_E_INVAL,
		   "remove NULL");
	}

	/* A malformed table is refused, not indexed past its end. */
	{
		struct cam_holders bad;

		cam_holders_init(&bad);
		bad.n = CAM_HOLDERS_MAX + 1;
		EQ(cam_holders_add(&bad, &nn, 0), CAM_HOLDERS_E_INVAL,
		   "n past the end");
		EQ(cam_holders_count(&bad), 0, "malformed counts 0");
		cam_holders_init(&bad);
		bad.claim_base = 1;
		EQ(cam_holders_abort(&bad), CAM_HOLDERS_E_INVAL,
		   "base past n");
	}
}

/* ---- 2. transitions ----------------------------------------------------- */

static void test_transitions(void)
{
	struct cam_holders t;

	cam_holders_init(&t);
	/* Removing from an empty table is a no-op, not STOP. */
	EQ(cam_holders_remove(&t, &manual), CAM_HOLDERS_T_NONE, "empty remove");

	/* empty -> non-empty: START, and the table is claimed. */
	EQ(cam_holders_add(&t, &manual, KEEP), CAM_HOLDERS_T_START, "first in");
	EQ(cam_holders_claimed(&t), 1, "claimed by the start");
	EQ(cam_holders_commit(&t), CAM_HOLDERS_T_NONE, "commit");
	EQ(cam_holders_claimed(&t), 0, "released");

	/* Joining a stream is NONE, leaving one others still hold is NONE. */
	EQ(cam_holders_add(&t, &nn, 0), CAM_HOLDERS_T_NONE, "nn joins");
	EQ(cam_holders_claimed(&t), 0, "a join does not claim");
	EQ(cam_holders_remove(&t, &nn), CAM_HOLDERS_T_NONE,
	   "nn leaves, manual stays: no transition");
	EQ(cam_holders_count(&t), 1, "manual alone");

	/* manual only -> empty: STOP. */
	EQ(cam_holders_remove(&t, &manual), CAM_HOLDERS_T_STOP, "manual leaves");
	EQ(cam_holders_count(&t), 0, "empty");
	EQ(cam_holders_remove(&t, &manual), CAM_HOLDERS_T_NONE, "again: no-op");

	/* nn alone is not a state the boards reach (nn always comes with the
	 * operator), but the table answers it truthfully: the last one out is
	 * STOP whoever it is. */
	EQ(cam_holders_add(&t, &nn, 0), CAM_HOLDERS_T_START, "nn alone starts");
	EQ(cam_holders_commit(&t), CAM_HOLDERS_T_NONE, "commit");
	EQ(cam_holders_remove(&t, &nn), CAM_HOLDERS_T_STOP, "nn alone: STOP");
}

/* ---- 3. D8: the operator does not leave while nn is there ---------------- */

static void test_in_use(void)
{
	struct cam_holders t;
	const struct cam_holder *l[CAM_HOLDERS_MAX];

	streaming_manual(&t);
	EQ(cam_holders_add(&t, &nn, 0), CAM_HOLDERS_T_NONE, "nn joins");
	EQ(cam_holders_remove(&t, &manual), CAM_HOLDERS_E_IN_USE,
	   "camera stream stop with nn");
	EQ(cam_holders_count(&t), 2, "refused changed nothing");
	CHECK(cam_holders_has(&t, &manual), "manual still there");
	EQ(cam_holders_list(&t, l, NULL, CAM_HOLDERS_MAX), 2, "list");
	CHECK(l[0] == &manual && l[1] == &nn, "order kept after refusal");

	/* Once nn has left, the same stop goes through and stops. */
	EQ(cam_holders_remove(&t, &nn), CAM_HOLDERS_T_NONE, "nn stream stop");
	EQ(cam_holders_remove(&t, &manual), CAM_HOLDERS_T_STOP, "now it stops");

	/* Two holders that keep the stream do not refuse each other. */
	streaming_manual(&t);
	EQ(cam_holders_add(&t, &other, KEEP), CAM_HOLDERS_T_NONE, "keeper 2");
	EQ(cam_holders_remove(&t, &manual), CAM_HOLDERS_T_NONE,
	   "keeper leaves past a keeper");
	EQ(cam_holders_remove(&t, &other), CAM_HOLDERS_T_STOP, "last keeper");

	/* A holder without the attribute is never refused. */
	streaming_manual(&t);
	EQ(cam_holders_add(&t, &nn, 0), CAM_HOLDERS_T_NONE, "nn joins");
	EQ(cam_holders_add(&t, &other, 0), CAM_HOLDERS_T_NONE, "other joins");
	EQ(cam_holders_remove(&t, &other), CAM_HOLDERS_T_NONE, "other leaves");
	EQ(cam_holders_remove(&t, &manual), CAM_HOLDERS_E_IN_USE,
	   "still refused while nn remains");
}

/* ---- 4. add_or_start: what it added, what the rollback undoes ------------ */

static void test_add_or_start(void)
{
	struct cam_holders t;
	struct cam_holders_ticket tk;

	/* Stopped: the operator is registered on its behalf, then nn. */
	cam_holders_init(&t);
	EQ(cam_holders_add_or_start(&t, &nn, 0, &manual, KEEP, &tk),
	   CAM_HOLDERS_T_START, "nn starts the camera");
	EQ(tk.added, CAM_HOLDERS_ADDED_SELF | CAM_HOLDERS_ADDED_STARTER,
	   "added both");
	EQ(cam_holders_count(&t), 2, "two holders");
	{
		const struct cam_holder *l[2];
		unsigned char a[2];

		EQ(cam_holders_list(&t, l, a, 2), 2, "list");
		CHECK(l[0] == &manual && l[1] == &nn, "manual first");
		EQ(a[0], KEEP, "manual keeps");
		EQ(a[1], 0, "nn does not");
	}
	/* The start failed: both go, the table is as it was. */
	EQ(cam_holders_abort(&t), 2, "abort removes both");
	EQ(cam_holders_count(&t), 0, "empty again");
	EQ(cam_holders_claimed(&t), 0, "released");

	/* The start succeeded: both stay, and nn stopping leaves the stream to
	 * the operator. */
	EQ(cam_holders_add_or_start(&t, &nn, 0, &manual, KEEP, &tk),
	   CAM_HOLDERS_T_START, "start again");
	EQ(cam_holders_commit(&t), CAM_HOLDERS_T_NONE, "committed");
	EQ(cam_holders_remove(&t, &nn), CAM_HOLDERS_T_NONE,
	   "nn stops, camera does not");
	CHECK(cam_holders_has(&t, &manual), "manual left behind");

	/* Streaming by hand: only nn is added, so nn's own failure must undo
	 * only nn -- the operator's holder survives. */
	streaming_manual(&t);
	EQ(cam_holders_add_or_start(&t, &nn, 0, &manual, KEEP, &tk),
	   CAM_HOLDERS_T_NONE, "nn joins a running stream");
	EQ(tk.added, CAM_HOLDERS_ADDED_SELF, "added only nn");
	EQ(cam_holders_claimed(&t), 0, "a join does not claim");
	EQ(cam_holders_remove(&t, &nn), CAM_HOLDERS_T_NONE, "nn rolls back");
	CHECK(cam_holders_has(&t, &manual), "operator survives the rollback");
	EQ(cam_holders_count(&t), 1, "just the operator");

	/* nn already holding: nothing added, so nothing to undo. */
	EQ(cam_holders_add(&t, &nn, 0), CAM_HOLDERS_T_NONE, "nn in");
	EQ(cam_holders_add_or_start(&t, &nn, 0, &manual, KEEP, &tk),
	   CAM_HOLDERS_T_NONE, "nn again");
	EQ(tk.added, 0, "added nothing");

	/* A non-empty table never touches the starter, even if it is absent. */
	cam_holders_init(&t);
	EQ(cam_holders_add(&t, &other, KEEP), CAM_HOLDERS_T_START, "other");
	EQ(cam_holders_commit(&t), CAM_HOLDERS_T_NONE, "commit");
	EQ(cam_holders_add_or_start(&t, &nn, 0, &manual, KEEP, &tk),
	   CAM_HOLDERS_T_NONE, "join other's stream");
	EQ(tk.added, CAM_HOLDERS_ADDED_SELF, "starter not added");
	CHECK(!cam_holders_has(&t, &manual), "manual absent");

	/* Full: refused with nothing added. */
	EQ(cam_holders_add(&t, &fourth, 0), CAM_HOLDERS_T_NONE, "fill 3");
	EQ(cam_holders_add(&t, &fifth, 0), CAM_HOLDERS_T_NONE, "fill 4");
	EQ(cam_holders_add_or_start(&t, &nn_twin, 0, &manual, KEEP, &tk),
	   CAM_HOLDERS_E_FULL, "full");
	EQ(tk.added, 0, "nothing added when full");

	/* Arguments: refused before anything changes. */
	cam_holders_init(&t);
	tk.added = 99u;
	tk.self_gen = 7u;
	EQ(cam_holders_add_or_start(&t, &nn, 0, &nn, KEEP, &tk),
	   CAM_HOLDERS_E_INVAL, "self as starter");
	EQ(tk.added, 0, "added cleared on refusal");
	EQ(tk.self_gen, 0, "generation cleared on refusal");
	EQ(cam_holders_add_or_start(&t, &nn, 0, NULL, KEEP, &tk),
	   CAM_HOLDERS_E_INVAL, "no starter");
	EQ(cam_holders_add_or_start(&t, &nn, 0, &manual, KEEP, NULL),
	   CAM_HOLDERS_E_INVAL, "no out");
	EQ(cam_holders_count(&t), 0, "still empty");

	/* Attribute mismatch on the join path. */
	streaming_manual(&t);
	EQ(cam_holders_add(&t, &nn, 0), CAM_HOLDERS_T_NONE, "nn in");
	EQ(cam_holders_add_or_start(&t, &nn, KEEP, &manual, KEEP, &tk),
	   CAM_HOLDERS_E_INVAL, "nn re-registered with keeps");
	EQ(tk.added, 0, "nothing added");
}

/* ---- 5. the claim ------------------------------------------------------- */

static void test_claim(void)
{
	struct cam_holders t;
	struct cam_holders_ticket tk;

	cam_holders_init(&t);
	EQ(cam_holders_commit(&t), CAM_HOLDERS_E_NOT_CLAIMED, "commit, no claim");
	EQ(cam_holders_abort(&t), CAM_HOLDERS_E_NOT_CLAIMED, "abort, no claim");

	EQ(cam_holders_add(&t, &manual, KEEP), CAM_HOLDERS_T_START, "start");
	/* While the start is in flight, nobody else changes the table -- not
	 * even with a no-op. */
	EQ(cam_holders_add(&t, &nn, 0), CAM_HOLDERS_E_CLAIMED, "add");
	EQ(cam_holders_add(&t, &manual, KEEP), CAM_HOLDERS_E_CLAIMED, "dup add");
	EQ(cam_holders_add_or_start(&t, &nn, 0, &manual, KEEP, &tk),
	   CAM_HOLDERS_E_CLAIMED, "add_or_start");
	EQ(tk.added, 0, "nothing added");
	EQ(cam_holders_remove(&t, &manual), CAM_HOLDERS_E_CLAIMED, "remove");
	EQ(cam_holders_remove(&t, &nn), CAM_HOLDERS_E_CLAIMED, "remove absent");
	EQ(cam_holders_count(&t), 1, "unchanged");
	/* Reading is fine. */
	CHECK(cam_holders_has(&t, &manual), "has during claim");

	/* The start failed: what it added goes, and it is retryable. */
	EQ(cam_holders_abort(&t), 1, "abort");
	EQ(cam_holders_count(&t), 0, "empty");
	EQ(cam_holders_abort(&t), CAM_HOLDERS_E_NOT_CLAIMED, "abort twice");
	EQ(cam_holders_add(&t, &nn, 0), CAM_HOLDERS_T_START, "retry starts");
	EQ(cam_holders_commit(&t), CAM_HOLDERS_T_NONE, "commit");
	EQ(cam_holders_commit(&t), CAM_HOLDERS_E_NOT_CLAIMED, "commit twice");
	/* After the commit, an abort cannot remove what the start added. */
	EQ(cam_holders_abort(&t), CAM_HOLDERS_E_NOT_CLAIMED, "late abort");
	CHECK(cam_holders_has(&t, &nn), "nn survives a late abort");
	EQ(cam_holders_add(&t, &manual, KEEP), CAM_HOLDERS_T_NONE,
	   "claim released: joins");
}

/* ---- 6. the list -------------------------------------------------------- */

static void test_list(void)
{
	struct cam_holders t;
	const struct cam_holder *l[CAM_HOLDERS_MAX];
	unsigned char a[CAM_HOLDERS_MAX];

	cam_holders_init(&t);
	EQ(cam_holders_list(&t, l, a, CAM_HOLDERS_MAX), 0, "empty list");

	EQ(cam_holders_add(&t, &manual, KEEP), CAM_HOLDERS_T_START, "1");
	EQ(cam_holders_commit(&t), CAM_HOLDERS_T_NONE, "commit");
	EQ(cam_holders_add(&t, &nn, 0), CAM_HOLDERS_T_NONE, "2");
	EQ(cam_holders_add(&t, &other, 0), CAM_HOLDERS_T_NONE, "3");
	EQ(cam_holders_list(&t, l, a, CAM_HOLDERS_MAX), 3, "three");
	CHECK(l[0] == &manual && l[1] == &nn && l[2] == &other,
	      "registration order");
	CHECK(strcmp(l[1]->name, "nn") == 0, "names are the holders'");

	/* Removing from the middle keeps the order of the rest. */
	EQ(cam_holders_remove(&t, &nn), CAM_HOLDERS_T_NONE, "middle out");
	EQ(cam_holders_list(&t, l, a, CAM_HOLDERS_MAX), 2, "two");
	CHECK(l[0] == &manual && l[1] == &other, "order after a gap");
	EQ(a[0], KEEP, "attr follows the holder");
	EQ(a[1], 0, "attr follows the holder");

	/* The cap: never more than asked for, and nothing past it written. */
	l[1] = NULL;
	EQ(cam_holders_list(&t, l, NULL, 1), 1, "capped at 1");
	CHECK(l[0] == &manual && l[1] == NULL, "nothing past the cap");
	EQ(cam_holders_list(&t, l, NULL, 0), 0, "capped at 0");
	EQ(cam_holders_list(&t, NULL, NULL, 4), 0, "no buffer");
}

/* ---- 7. dispositions ---------------------------------------------------- */

static void test_disp(void)
{
	EQ(cam_holders_disp_of(CAM_HOLDERS_T_NONE), CAM_HOLDERS_DISP_DONE, "none");
	EQ(cam_holders_disp_of(CAM_HOLDERS_T_START), CAM_HOLDERS_DISP_DONE, "start");
	EQ(cam_holders_disp_of(CAM_HOLDERS_T_STOP), CAM_HOLDERS_DISP_DONE, "stop");
	EQ(cam_holders_disp_of(CAM_HOLDERS_E_CLAIMED), CAM_HOLDERS_DISP_RETRYABLE,
	   "claimed");
	EQ(cam_holders_disp_of(CAM_HOLDERS_E_INVAL), CAM_HOLDERS_DISP_TERMINAL,
	   "inval");
	EQ(cam_holders_disp_of(CAM_HOLDERS_E_FULL), CAM_HOLDERS_DISP_TERMINAL,
	   "full");
	EQ(cam_holders_disp_of(CAM_HOLDERS_E_IN_USE), CAM_HOLDERS_DISP_TERMINAL,
	   "in use");
	EQ(cam_holders_disp_of(CAM_HOLDERS_E_NOT_CLAIMED),
	   CAM_HOLDERS_DISP_TERMINAL, "not claimed");
	EQ(cam_holders_disp_of(-99), CAM_HOLDERS_DISP_TERMINAL,
	   "unknown fails closed");

	/* The refusals are distinct codes, never folded into one. */
	CHECK(CAM_HOLDERS_E_INVAL != CAM_HOLDERS_E_FULL &&
	      CAM_HOLDERS_E_FULL != CAM_HOLDERS_E_CLAIMED &&
	      CAM_HOLDERS_E_CLAIMED != CAM_HOLDERS_E_IN_USE &&
	      CAM_HOLDERS_E_IN_USE != CAM_HOLDERS_E_NOT_CLAIMED &&
	      CAM_HOLDERS_E_INVAL != CAM_HOLDERS_E_IN_USE &&
	      CAM_HOLDERS_E_INVAL != CAM_HOLDERS_E_CLAIMED &&
	      CAM_HOLDERS_E_INVAL != CAM_HOLDERS_E_NOT_CLAIMED &&
	      CAM_HOLDERS_E_FULL != CAM_HOLDERS_E_IN_USE &&
	      CAM_HOLDERS_E_FULL != CAM_HOLDERS_E_NOT_CLAIMED &&
	      CAM_HOLDERS_E_CLAIMED != CAM_HOLDERS_E_NOT_CLAIMED,
	      "distinct codes");
}

/* ---- 8. the late undo: generations (review F1) --------------------------- */

static void test_late_undo(void)
{
	struct cam_holders t;
	struct cam_holders_ticket tk, tk2;

	/* The exact sequence the review found.  nn joins a stopped camera (it
	 * takes the operator's place too) and the start commits; the stream then
	 * ends for good (a confirmed end clears the table); the operator starts
	 * it again by hand; only now does nn's start fail (its step refuses to
	 * open) and undo its join.  The undo must not take the operator's NEW
	 * registration -- that would stop the operator's stream. */
	cam_holders_init(&t);
	EQ(cam_holders_add_or_start(&t, &nn, 0, &manual, KEEP, &tk),
	   CAM_HOLDERS_T_START, "nn starts the camera");
	EQ(cam_holders_commit(&t), CAM_HOLDERS_T_NONE, "start committed");
	cam_holders_clear(&t);                       /* confirmed end      */
	EQ(cam_holders_count(&t), 0, "cleared");
	EQ(cam_holders_add(&t, &manual, KEEP), CAM_HOLDERS_T_START,
	   "operator starts it again");
	EQ(cam_holders_commit(&t), CAM_HOLDERS_T_NONE, "committed");
	EQ(cam_holders_undo(&t, &nn, &manual, &tk), CAM_HOLDERS_T_NONE,
	   "late undo: no transition");
	CHECK(cam_holders_has(&t, &manual), "the operator's stream survives");
	EQ(cam_holders_count(&t), 1, "only the operator");

	/* Same class: the operator starts by hand ON TOP of nn's join (manual
	 * already present, so the add is a re-registration), then nn's start
	 * fails and undoes.  nn goes, the operator's renewed hold stays. */
	cam_holders_init(&t);
	EQ(cam_holders_add_or_start(&t, &nn, 0, &manual, KEEP, &tk),
	   CAM_HOLDERS_T_START, "nn starts the camera");
	EQ(cam_holders_commit(&t), CAM_HOLDERS_T_NONE, "committed");
	EQ(cam_holders_add(&t, &manual, KEEP), CAM_HOLDERS_T_NONE,
	   "operator starts it by hand: already held");
	EQ(cam_holders_undo(&t, &nn, &manual, &tk), CAM_HOLDERS_T_NONE,
	   "nn undoes");
	CHECK(!cam_holders_has(&t, &nn), "nn gone");
	CHECK(cam_holders_has(&t, &manual), "the renewed operator stays");

	/* Nobody intervened: the undo takes back exactly both, and says STOP. */
	cam_holders_init(&t);
	EQ(cam_holders_add_or_start(&t, &nn, 0, &manual, KEEP, &tk),
	   CAM_HOLDERS_T_START, "start");
	EQ(cam_holders_commit(&t), CAM_HOLDERS_T_NONE, "committed");
	EQ(cam_holders_undo(&t, &nn, &manual, &tk), CAM_HOLDERS_T_STOP,
	   "undo empties: stop");
	EQ(cam_holders_count(&t), 0, "both gone");

	/* A join onto a running stream added only nn: the undo never touches
	 * the operator, whatever the starter argument says. */
	cam_holders_init(&t);
	EQ(cam_holders_add(&t, &manual, KEEP), CAM_HOLDERS_T_START, "operator");
	EQ(cam_holders_commit(&t), CAM_HOLDERS_T_NONE, "committed");
	EQ(cam_holders_add_or_start(&t, &nn, 0, &manual, KEEP, &tk),
	   CAM_HOLDERS_T_NONE, "nn joins");
	EQ(cam_holders_undo(&t, &nn, &manual, &tk), CAM_HOLDERS_T_NONE,
	   "undo nn");
	CHECK(cam_holders_has(&t, &manual), "operator untouched");

	/* nn re-joined after a clear: an undo with the OLD ticket leaves the new
	 * nn registration alone. */
	cam_holders_init(&t);
	EQ(cam_holders_add_or_start(&t, &nn, 0, &manual, KEEP, &tk),
	   CAM_HOLDERS_T_START, "first join");
	EQ(cam_holders_commit(&t), CAM_HOLDERS_T_NONE, "committed");
	cam_holders_clear(&t);
	EQ(cam_holders_add_or_start(&t, &nn, 0, &manual, KEEP, &tk2),
	   CAM_HOLDERS_T_START, "second join");
	EQ(cam_holders_commit(&t), CAM_HOLDERS_T_NONE, "committed");
	EQ(cam_holders_undo(&t, &nn, &manual, &tk), CAM_HOLDERS_T_NONE,
	   "stale undo");
	EQ(cam_holders_count(&t), 2, "second join intact");
	CHECK(tk2.self_gen != tk.self_gen && tk2.starter_gen != tk.starter_gen,
	      "clear does not restart the generations");

	/* An undo never stops a stream another holder needs: the starter it
	 * added stays while somebody without KEEPS is there. */
	cam_holders_init(&t);
	EQ(cam_holders_add_or_start(&t, &nn, 0, &manual, KEEP, &tk),
	   CAM_HOLDERS_T_START, "start");
	EQ(cam_holders_commit(&t), CAM_HOLDERS_T_NONE, "committed");
	EQ(cam_holders_add(&t, &other, 0), CAM_HOLDERS_T_NONE, "other joins");
	EQ(cam_holders_undo(&t, &nn, &manual, &tk), CAM_HOLDERS_T_NONE,
	   "undo with other present");
	CHECK(cam_holders_has(&t, &manual), "operator kept for other");
	CHECK(!cam_holders_has(&t, &nn), "nn gone");

	/* Refusals. */
	EQ(cam_holders_undo(&t, &nn, &manual, NULL), CAM_HOLDERS_E_INVAL,
	   "no ticket");
	cam_holders_init(&t);
	EQ(cam_holders_add(&t, &manual, KEEP), CAM_HOLDERS_T_START, "claim");
	EQ(cam_holders_undo(&t, &nn, &manual, &tk), CAM_HOLDERS_E_CLAIMED,
	   "undo during a claim");
	memset(&tk2, 0, sizeof tk2);
	tk2.added = CAM_HOLDERS_ADDED_STARTER;
	EQ(cam_holders_abort(&t), 1, "abort");
	EQ(cam_holders_add(&t, &manual, KEEP), CAM_HOLDERS_T_START, "again");
	EQ(cam_holders_commit(&t), CAM_HOLDERS_T_NONE, "committed");
	EQ(cam_holders_undo(&t, &nn, &manual, &tk2), CAM_HOLDERS_T_NONE,
	   "generation 0 matches nothing");
	CHECK(cam_holders_has(&t, &manual), "still there");
}

int main(void)
{
	test_registration();
	test_transitions();
	test_in_use();
	test_add_or_start();
	test_claim();
	test_list();
	test_disp();
	test_late_undo();

	if (fails) {
		printf("test_cam_holders: %d FAILED\n", fails);
		return 1;
	}
	printf("test_cam_holders: all passed\n");
	return 0;
}
