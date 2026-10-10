/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_core_model.c
 * @brief   The order of a model load and unload, shared by every board (issue
 *          #131).  See nn_core_model.h.
 */
#include "nn_core_model.h"

#include <stddef.h>   /* NULL */

#include "nn_swap.h"

/* Load and unload never hand the caller cleanup authority: what they hold is
 * given back before they return. */
static void nn_core_model_result(struct nn_core_model_job *j, int status)
{
	j->res->status = status;
	j->res->claim  = (uint8_t)NN_CLAIM_NONE;
}

static void nn_core_model_bump(struct nn_core_model *m,
                               const struct nn_core_model_board *b)
{
	unsigned posture = b->cs_enter();

	m->seq++;
	b->cs_exit(posture);
}

uint32_t nn_core_model_seq(const struct nn_core_model *m,
                           const struct nn_core_model_board *b)
{
	unsigned posture = b->cs_enter();
	uint32_t v       = m->seq;

	b->cs_exit(posture);
	return v;
}

int nn_core_model_copy_stands(uint32_t seq0, uint32_t seq1, int claim_free)
{
	if (claim_free)
		return 1;
	return (seq0 & 1u) == 0u && seq0 == seq1;
}

/* What is open when nothing has been changed: the refusals before the claim,
 * and a refused claim, report it as it stands. */
static enum nn_model_state nn_core_model_as_is(const struct nn_core_model_board *b)
{
	return b->backend->has_model() ? NN_MODEL_PREVIOUS : NN_MODEL_EMPTY;
}

/* The backend's swap, then the plugin -- the steps under which the plugin's
 * entry() runs on the console's stack.  Small locals only. */
static enum nn_swap_end
nn_core_model_swap(const struct nn_core_model_board *b,
                   struct nn_core_model_job *j, int *status)
{
	int model_after = 0, plugin_refused = 0, rc;

	rc = b->backend->swap(j, &model_after);
	if (rc != NN_SVC_OK) {
		/* [!] BUSY IS ANSWERED ONLY BEFORE ANYTHING CHANGED (nn_core_model.h).
		 * Past this point something may have, so a BUSY from the backend is
		 * reported as the hardware's refusal it is. */
		*status = (rc == NN_SVC_ERR_BUSY) ? NN_SVC_ERR_HW : rc;
	} else if (!model_after) {
		/* [!] A "SUCCESS" THAT LEFT NO MODEL IS A FAILURE.  The table calls
		 * it LOST and takes everything down; the status must not say
		 * "loaded" beside that. */
		*status = NN_SVC_ERR_HW;
	} else if (nn_swap_swaps_plugin(0, model_after)) {
		/* [!] ONLY NOW, AFTER THE BACKEND TOOK THE NEW MODEL.  The reservation
		 * is one region: a plugin copied in before, or after a rollback, would
		 * destroy the previous model's decoder for nothing.  A bare model
		 * loads nothing, so the previous decoder goes -- it must not read the
		 * new model's outputs. */
		if (b->lease_take == NULL) {
			/* a board with no lease has no plugin to touch */
		} else if (j->bare || b->plugin_start == NULL) {
			if (b->plugin_unload != NULL)
				b->plugin_unload();
		} else {
			int pr = b->plugin_start(j);

			if (pr != NN_SVC_OK) {
				plugin_refused = 1;
				*status = (pr == NN_SVC_ERR_BUSY) ? NN_SVC_ERR_HW : pr;
			}
		}
	}
	return nn_swap_end_of(rc == NN_SVC_OK ? 0 : 1, model_after,
	                      plugin_refused);
}

void nn_core_model_load(struct nn_core_model *m,
                        const struct nn_core_model_board *b,
                        struct nn_core_model_job *j,
                        enum nn_model_state *state)
{
	struct nn_swap_verdict v;
	enum nn_swap_end end;
	int status = NN_SVC_OK, leased = 0;

	*state      = NN_MODEL_EMPTY;
	j->had_open = 0;
	j->bare     = 0;

	/* ---- before the claim: nothing is acquired, nothing changes ---------- */

	/* [!] THE TAG IS REFUSED BEFORE ANYTHING IS ACQUIRED.  A source this board
	 * does not have is not a hardware failure and must not cost a bring-up. */
	if (j->spec == NULL || (unsigned)j->spec->tag >= 32u ||
	    (b->tags & NN_CORE_MODEL_TAG(j->spec->tag)) == 0u) {
		if (b->spec_refused != NULL)
			b->spec_refused(j);
		*state = nn_core_model_as_is(b);
		nn_core_model_result(j, NN_SVC_ERR_SPEC);
		return;
	}
	if (b->check_spec != NULL && (status = b->check_spec(j)) != NN_SVC_OK) {
		*state = nn_core_model_as_is(b);
		nn_core_model_result(j, status);
		return;
	}
	if (b->admit != NULL && (status = b->admit(j)) != NN_SVC_OK) {
		*state = nn_core_model_as_is(b);
		nn_core_model_result(j, status);
		return;
	}
	if (!b->claim_take()) {
		*state = nn_core_model_as_is(b);
		nn_core_model_result(j, NN_SVC_ERR_BUSY);
		return;
	}

	/* ---- under the claim ------------------------------------------------- */

	/* Read under the claim: no unload, and no other load, can move it now. */
	j->had_open = b->backend->has_model() ? 1 : 0;

	/* A preparation that fails gave back what it took; nothing else moved. */
	if (b->prepare != NULL && (status = b->prepare(j)) != NN_SVC_OK) {
		*state = j->had_open ? NN_MODEL_PREVIOUS : NN_MODEL_EMPTY;
		nn_core_model_result(j, status);
		b->claim_give();
		return;
	}

	status = b->fetch(j);
	if (status != NN_SVC_OK) {
		end = NN_SWAP_REFUSED;
		goto settle;
	}

	/*
	 * [!] THE PLUGIN LEASE, BEFORE THE FIRST THING THAT CHANGES WHAT IS OPEN
	 * (issue #127).  Everything this load replaces -- the plugin, the last
	 * result, the geometry -- follows the backend, whose first step closes the
	 * old model; so this is the last moment at which a refusal still leaves
	 * everything as it was.  The lookup above stays outside it.
	 */
	leased = (b->lease_take == NULL) ? 1 : b->lease_take(j);
	if (!leased) {
		status = NN_SVC_ERR_BUSY;
		end    = NN_SWAP_REFUSED;
		goto settle;
	}

	nn_core_model_bump(m, b);   /* odd: what is open is about to change */
	end = nn_core_model_swap(b, j, &status);

settle:
	nn_swap_decide(j->had_open, end, &v);
	/* Unload's order: plugin -> model -> hardware.
	 * [!] THE PLUGIN ONLY UNDER THE LEASE (issue #127): a load refused before
	 * it held the lease swapped nothing in, and a plugin that is published
	 * belongs to whoever holds the lease. */
	if (v.unload && leased && b->lease_take != NULL && b->plugin_unload != NULL)
		b->plugin_unload();
	if (v.hw_down) {
		if (b->backend->release != NULL)
			b->backend->release();
		if (b->hw_down != NULL)
			b->hw_down();
	}
	/* [!] The last result goes BEFORE any new identity is visible (issue
	 * #118) -- and only when what is open changed. */
	if (v.invalidate && b->invalidate != NULL)
		b->invalidate();
	if (v.forget)
		b->forget();
	/* [!] COMMITTED EVEN WHEN THE PLUGIN WAS REFUSED (issue #122 D6): the new
	 * model IS open. */
	if (v.commit)
		b->commit(j);
	/* A capture's geometry belongs to the model it was taken for. */
	if (v.state != (unsigned char)NN_MODEL_PREVIOUS && b->geom_clear != NULL)
		b->geom_clear(leased && b->lease_take != NULL);
	if (leased) {
		nn_core_model_bump(m, b);   /* even: the last change is behind */
		if (b->lease_give != NULL)
			b->lease_give();
	}

	*state = (enum nn_model_state)v.state;
	nn_core_model_result(j, v.ok ? NN_SVC_OK : status);
	b->claim_give();
}

void nn_core_model_unload(struct nn_core_model *m,
                          const struct nn_core_model_board *b,
                          struct nn_core_model_job *j)
{
	int status;

	j->had_open = 0;
	j->bare     = 0;

	if (b->admit != NULL && (status = b->admit(j)) != NN_SVC_OK) {
		nn_core_model_result(j, status);
		return;
	}
	if (!b->claim_take()) {
		nn_core_model_result(j, NN_SVC_ERR_BUSY);
		return;
	}

	/* [!] UNDER THE PLUGIN LEASE, taken before anything changes (issue #127):
	 * refused, nothing is unloaded and the counter does not move. */
	if (b->lease_take != NULL && !b->lease_take(j)) {
		nn_core_model_result(j, NN_SVC_ERR_BUSY);
		b->claim_give();
		return;
	}
	nn_core_model_bump(m, b);   /* odd */

	/* [!] THE PLUGIN GOES FIRST, and what its result is made of with it, under
	 * the lease and before the backend (issues #122, #127): no window exists
	 * in which a plugin is published for a model that is already gone. */
	if (b->lease_take != NULL && b->plugin_unload != NULL)
		b->plugin_unload();
	if (b->invalidate != NULL)
		b->invalidate();
	if (b->geom_clear != NULL)
		b->geom_clear(b->lease_take != NULL);
	if (b->lease_take != NULL && b->lease_give != NULL)
		b->lease_give();

	if (b->backend->release != NULL)
		b->backend->release();
	if (b->hw_down != NULL)
		b->hw_down();
	b->forget();

	nn_core_model_bump(m, b);   /* even */
	nn_core_model_result(j, NN_SVC_OK);
	b->claim_give();
}
