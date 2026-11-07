/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_svc_common.c
 * @brief   `nn info`'s model section and `nn thresh`, once (issue #174).  See
 *          nn_svc_common.h.
 *
 * [!] NO MUTABLE STORAGE: every hook is the board's, every result the caller's.
 */
#include "nn_svc_common.h"

#include <stddef.h>
#include <string.h>

#include "nn.h"               /* the inference API (svc/nn/nn.c)           */
#include "nn_active_core.h"   /* NN_ACTIVE_THRESH_*                        */
#include "nn_core_model.h"    /* nn_core_model_copy_stands()               */

/* ---- nn info ------------------------------------------------------------- */

/* A BUSY copy carries no value (issue #199): nn_svc_common_info() cleared
 * seq_known first and sets it only once the copy stands. */
static void nn_svc_common_busy(struct nn_svc_info *out)
{
	out->model_active   = 0u;
	out->model[0]       = '\0';
	out->arena_used     = 0u;
	out->avail_identity = (uint8_t)NN_AVAIL_BUSY;
	out->avail_runtime  = (uint8_t)NN_AVAIL_BUSY;
	out->avail_tensors  = (uint8_t)NN_AVAIL_BUSY;
}

void nn_svc_common_info(struct nn_svc_info *out, uint32_t (*life_seq)(void))
{
	const struct nn_backend_info *bi = nn_backend();
	struct nn_model *m = NULL;
	uint32_t seq0, seq1;
	int held, rc;

	memset(out, 0, sizeof *out);
	nn_svc_str(out->backend, sizeof out->backend, bi ? bi->name : NULL);
	nn_svc_str(out->version, sizeof out->version, bi ? bi->version : NULL);

	/* [!] THE RESERVATION, NOT THE USE (issue #131 P3): a fixed fact of the
	 * build, so it needs no claim and stands even when the rest is busy. */
	out->arena_bytes = nn_arena_reserved();

	/* [!] THE FIRST READ COMES BEFORE THE OPEN (issue #199): a failed open
	 * gives its initialisation back, so another console can open again and
	 * run a whole load before this call reads anything.  Only two reads
	 * around everything this copy rests on -- the open included -- can say
	 * nothing ran in between. */
	seq0 = life_seq();
	rc = nn_model_open(&m);
	if (rc == NN_ERR_SCRATCH_HELD) {
		/* Another command holds the backend's memory and nothing was opened
		 * (f746g-disco's `sdram test`, issue #152): a transient answer, not
		 * "nothing is loaded".  A board whose backend memory nobody else
		 * claims never gets this. */
		nn_svc_common_busy(out);
		return;
	}
	if (rc != 0 || m == NULL) {
		/* The backend could not open: "nothing loaded", which is true and
		 * not BUSY.  The copy is of the state the counter showed on both
		 * sides of the open, when the two reads agree and are even (issue
		 * #199); a load that ran in between leaves it with no value, and one
		 * that lands later fails nn_svc_info_stands() like any other. */
		seq1 = life_seq();
		out->seq_known = (uint8_t)(seq0 == seq1 && (seq1 & 1u) == 0u);
		out->seq       = out->seq_known ? seq1 : 0u;
		return;
	}

	/*
	 * [!] THE NAME IS COPIED UNDER THE SESSION WHEN THE SESSION IS FREE, and
	 * copied anyway when it is not -- a deliberate middle (issue #122):
	 *
	 *   Taking the session unconditionally would make `nn info` REFUSE while a
	 *   stream runs -- the worker holds it for the whole stream -- and that is
	 *   exactly when the report is worth asking for.
	 *
	 *   Not taking it at all leaves the copy able to race a reload, which
	 *   rewrites the backend's name buffer byte by byte.
	 *
	 * So: try, and when the session is held, ask the transition counter
	 * whether the copy describes one model (below).  Copied, not borrowed: the
	 * backend owns the name and a reload replaces it.
	 */
	held = (nn_session_try_acquire() == 0);
	/* [!] "A MODEL IS THERE", NOT "THE SINGLETON OPENED" (issue #122 P2,
	 * #131 P16): the singleton stays open with nothing in it after boot and
	 * after `nn model unload`. */
	out->model_active = nn_model_present(m) ? 1u : 0u;
	nn_svc_str(out->model, sizeof out->model, nn_model_name(m));
	out->arena_used  = out->model_active ? nn_activations_bytes(m) : 0u;
	/*
	 * [!] THE SECOND READ IS TAKEN BEFORE THE SESSION IS GIVEN BACK (issue
	 * #199): it is the value the copy carries (below), and a load claims this
	 * same session, so under it the counter cannot move.  Read after the
	 * release, a load that ran whole in between would lend the copy its own
	 * value, and the next model's tensors would pass as this one's.  Not
	 * held, the two reads either side of the copy decide it, as before.
	 */
	seq1 = life_seq();
	if (held)
		nn_session_release();

	/*
	 * [!] A LOAD IN FLIGHT IS BUSY, NOT "NO MODEL" OR A NAME AND A SIZE FROM
	 * TWO MODELS (issue #122; #131 step 7d).  The backend rewrites the name and
	 * the tensors while it swaps -- the new model, or the previous one on a
	 * rollback.  When the session was free no load can have run; when it was
	 * not, the copy stands only if no load or unload was between its two steps
	 * at either end of it, nor ran whole in between.  A stream holds the
	 * session and never moves the counter, so its report is what it was.
	 */
	if (!nn_core_model_copy_stands(seq0, seq1, held)) {
		nn_svc_common_busy(out);
		return;
	}
	/* Which state the copy is of (issue #199): seq1, read under the session
	 * or equal to seq0.  An odd value cannot come from either -- a load in
	 * flight holds the session, and an odd seq0 does not stand -- but one
	 * is never carried: it would belong to no one state. */
	out->seq_known = (uint8_t)((seq1 & 1u) == 0u);
	out->seq       = seq1;

	/* [!] Say plainly when nothing is being inferred, so a latency from
	 * `nn bench` is never mistaken for a model's. */
	if (bi && strcmp(bi->name, "null") == 0)
		nn_svc_str(out->source, sizeof out->source,
		           "synthetic workload, not inference");

	/* Every section is answered here, streaming or not -- which is the point of
	   the copy above (issue #99). */
	out->avail_identity = (uint8_t)NN_AVAIL_OK;
	out->avail_runtime  = (uint8_t)NN_AVAIL_OK;
	out->avail_tensors  = out->model_active ? (uint8_t)NN_AVAIL_OK
	                                        : (uint8_t)NN_AVAIL_NA;
}

int nn_svc_common_info_stands(const struct nn_svc_info *info,
                              uint32_t (*life_seq)(void))
{
	return nn_svc_info_seq_stands(info, life_seq());
}

/* ---- nn thresh ----------------------------------------------------------- */

/*
 * [!] COUNTED IN, THEN THE LEASE; GIVEN BACK IN THE REVERSE ORDER, ON EVERY
 * EXIT (issues #110, #122, #127).  The entries reach a plugin's param_get /
 * param_set, which read and write plugin state a decode may be rewriting and a
 * concurrent `nn model load` may be REPLACING, and `nn thresh` takes no NN
 * session.  The lease keeps the decode and the draw out; on a board whose
 * console calls are counted from outside its gate, the count keeps a load or an
 * unload from replacing the plugin under the call.  Not had, the call is BUSY
 * -- "could not be asked", never NN_SVC_THRESH_NONE, which an operator reads as
 * "this decoder has no threshold" (issue #122 P6).
 */
static int nn_svc_common_in(const struct nn_svc_thresh_ops *o)
{
	/* fail-closed: a count with only one end would leave a call counted in
	 * for good (or count out one that never came in) */
	if ((o->enter == NULL) != (o->leave == NULL))
		return 0;
	if (o->lease_take == NULL || o->lease_give == NULL)
		return 0;   /* fail-closed: never enter a plugin unleased */
	if (o->enter != NULL && !o->enter())
		return 0;
	if (!o->lease_take()) {
		if (o->leave != NULL)
			o->leave();
		return 0;
	}
	return 1;
}

static void nn_svc_common_out(const struct nn_svc_thresh_ops *o)
{
	o->lease_give();
	if (o->leave != NULL)
		o->leave();
}

int nn_svc_common_thresh_get(const struct nn_svc_thresh_ops *o,
                             unsigned *milli)
{
	int r;

	if (milli == NULL)
		return NN_SVC_ERR_ARG;
	*milli = NN_SVC_THRESH_NONE;
	/* [!] NO DECODER IN THIS BUILD (issue #116): nothing holds a threshold --
	 * the answer a build with a decoder gives with none loaded. */
	if (o == NULL || o->get == NULL)
		return NN_SVC_OK;
	if (!nn_svc_common_in(o))
		return NN_SVC_ERR_BUSY;
	r = o->get(milli);
	nn_svc_common_out(o);
	/* NN_ACTIVE_NOT_HELD cannot come back from under the lease; if it ever
	 * does, it is BUSY like any other call that did not get in (issue #130). */
	return r == NN_ACTIVE_THRESH_OK ? NN_SVC_OK : NN_SVC_ERR_BUSY;
}

int nn_svc_common_thresh_set(const struct nn_svc_thresh_ops *o,
                             unsigned milli)
{
	int r;

	/* Nothing holds one in this build -- and "there is nothing to set" is not
	 * "that value is out of range" (issue #116). */
	if (o == NULL || o->set == NULL)
		return NN_SVC_ERR_STATE;
	if (!nn_svc_common_in(o))
		return NN_SVC_ERR_BUSY;
	r = o->set(milli);
	nn_svc_common_out(o);
	if (r == NN_ACTIVE_THRESH_OK)
		return NN_SVC_OK;
	/* The loaded decoder holds no threshold: a state, not a bad value. */
	if (r == NN_ACTIVE_THRESH_NO_DECODER)
		return NN_SVC_ERR_STATE;
	if (r == o->not_held)
		return NN_SVC_ERR_BUSY;   /* nothing was entered */
	return NN_SVC_ERR_ARG;
}
