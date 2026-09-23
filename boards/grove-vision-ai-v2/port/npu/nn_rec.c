/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * This board's decode record (issue #118).  See nn_rec.h.
 */
#include "nn_rec.h"

#include <stddef.h>

#include "tx_api.h"

/* Static and never freed, like everything the producer touches: a producer
 * that never acknowledged a stop may still publish into it (camera.h). */
static struct nn_det_record nn_rec;

void nn_rec_boundary(void)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	nn_det_record_boundary(&nn_rec);
	TX_RESTORE
}

uint32_t nn_rec_boundary_base(void)
{
	struct nn_det_snapshot s;
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	nn_det_record_boundary(&nn_rec);
	nn_det_record_snapshot(&nn_rec, &s, NULL, 0);
	TX_RESTORE
	return s.accepted;
}

void nn_rec_invalidate(void)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	nn_det_record_invalidate(&nn_rec);
	TX_RESTORE
}

uint32_t nn_rec_gen(void)
{
	uint32_t g;
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	g = nn_det_record_gen(&nn_rec);
	TX_RESTORE
	return g;
}

int nn_rec_publish_external(int n, uint32_t gen)
{
	int took;
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	took = nn_det_record_publish_external(&nn_rec, n, gen);
	TX_RESTORE
	return took;
}

int nn_rec_publish_raw(uint32_t gen, const struct nn_raw_outputs *raw)
{
	int took;
	TX_INTERRUPT_SAVE_AREA

	/* Only `nn run` publishes this kind (a stream needs a plugin), so the
	 * ~300 B copy is never on the producer's path. */
	TX_DISABLE
	took = nn_det_record_publish_raw(&nn_rec, gen, raw);
	TX_RESTORE
	return took;
}

void nn_rec_snapshot(struct nn_det_snapshot *out, struct nn_result_extra *ext)
{
	TX_INTERRUPT_SAVE_AREA

	if (out == NULL)
		return;
	TX_DISABLE
	nn_det_record_snapshot(&nn_rec, out, NULL, 0);
	if (ext != NULL)
		nn_det_record_extra(&nn_rec, ext);
	TX_RESTORE
}
