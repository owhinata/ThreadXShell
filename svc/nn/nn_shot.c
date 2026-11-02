/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_shot.c
 * @brief   How one `nn run` ended (issue #171).  See nn_shot.h.
 *
 * [!] NO MUTABLE STORAGE: the state is the board's, passed in.
 */
#include "nn_shot.h"

#include <stddef.h>   /* NULL */

#include "nn_detail.h"  /* NN_SVC_DETAIL_LIT */
#include "nn_svc.h"     /* NN_SVC_ERR_* */

/*
 * The status each failure reports, on all three boards (grove-vision-ai-v2 reads
 * these words too, issue #174): a lease not had in time is BUSY (the same
 * request may pass in a moment), everything else is HW.
 */
static const struct nn_shot_says nn_shot_table[] = {
	[NN_SHOT_PREP] = { NN_SVC_ERR_HW,
		NN_SVC_DETAIL_LIT("preprocessing refused the frame") },
	[NN_SHOT_INVOKE] = { NN_SVC_ERR_HW,
		NN_SVC_DETAIL_LIT("inference failed") },
	[NN_SHOT_LEASE_TIMEOUT] = { NN_SVC_ERR_BUSY,
		NN_SVC_DETAIL_LIT("the plugin lease was not released in time; "
		                  "nothing was decoded") },
	[NN_SHOT_NOT_HELD] = { NN_SVC_ERR_HW,
		NN_SVC_DETAIL_LIT("the decode was refused: the plugin lease is "
		                  "not held") },
	[NN_SHOT_NO_OUTPUTS] = { NN_SVC_ERR_HW,
		NN_SVC_DETAIL_LIT("the outputs of the model could not be handed "
		                  "to the decoder") },
	[NN_SHOT_UNKNOWN] = { NN_SVC_ERR_HW,
		NN_SVC_DETAIL_LIT("the inference worker ended the run without a "
		                  "result") },
};

#define NN_SHOT_TABLE_N (sizeof nn_shot_table / sizeof nn_shot_table[0])

uint32_t nn_shot_begin(struct nn_shot *s)
{
	uint32_t run = s->run + 1u;

	if (run == 0u)   /* 0 is "no run" */
		run = 1u;
	s->run    = run;
	s->ended  = 0u;
	s->reason = (uint8_t)NN_SHOT_NONE;
	return run;
}

uint32_t nn_shot_open(const struct nn_shot *s)
{
	if (s->run == 0u || s->ended == s->run)
		return 0u;
	return s->run;
}

enum nn_shot_reason nn_shot_of_done(enum nn_core_done what, int took)
{
	switch (what) {
	case NN_CORE_DONE_RAW:
	case NN_CORE_DONE_DECODED:
		/* Not taken: a generation that moved under it.  The next frame is
		 * still this run's. */
		return took ? NN_SHOT_PUBLISHED : NN_SHOT_NONE;
	case NN_CORE_DONE_RETIRED:
		return NN_SHOT_NONE;
	case NN_CORE_DONE_NOT_HELD:
		return NN_SHOT_NOT_HELD;
	case NN_CORE_DONE_NO_OUTPUTS:
		return NN_SHOT_NO_OUTPUTS;
	default:
		return NN_SHOT_UNKNOWN;
	}
}

int nn_shot_record(struct nn_shot *s, uint32_t run, enum nn_shot_reason reason,
                   int gen_current)
{
	if (run == 0u || run != s->run || s->ended == run)
		return 0;
	if (reason == NN_SHOT_NONE)
		return 1;
	/* [!] An accepted publish is the answer whatever the generation; a
	 * failure only while the frame's generation is still the record's. */
	if (reason != NN_SHOT_PUBLISHED && !gen_current)
		return 1;
	if ((unsigned)reason > (unsigned)NN_SHOT_UNKNOWN)
		reason = NN_SHOT_UNKNOWN;
	s->ended  = run;
	s->reason = (uint8_t)reason;
	return 0;
}

int nn_shot_after_job(struct nn_shot *s, uint32_t run, enum nn_core_done what,
                      int took, int gen_current)
{
	return nn_shot_record(s, run, nn_shot_of_done(what, took), gen_current);
}

int nn_shot_failed(const struct nn_shot *s, enum nn_shot_reason *reason)
{
	if (s->run == 0u || s->ended != s->run ||
	    s->reason == (uint8_t)NN_SHOT_NONE ||
	    s->reason == (uint8_t)NN_SHOT_PUBLISHED)
		return 0;
	if (reason != NULL)
		*reason = (enum nn_shot_reason)s->reason;
	return 1;
}

const struct nn_shot_says *nn_shot_says(enum nn_shot_reason reason)
{
	if ((unsigned)reason >= NN_SHOT_TABLE_N ||
	    nn_shot_table[reason].text == NULL)
		return &nn_shot_table[NN_SHOT_UNKNOWN];
	return &nn_shot_table[reason];
}
