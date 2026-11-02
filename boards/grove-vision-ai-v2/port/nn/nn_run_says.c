/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_run_says.c
 * @brief   A NO_RESULT `nn run`'s status and sentence on this board (issue
 *          #174).  See nn_run_says.h.
 */
#include "nn_run_says.h"

#include "nn_camera.h"       /* NN_OV_SHOT_*                              */
#include "nn_shot.h"         /* nn_shot_says(): the shared sentences      */
#include "nn_svc_adapter.h"  /* nn_detail_set                             */

int nn_run_no_result_says(int shot, unsigned out_index,
                          struct nn_op_result *res)
{
	const struct nn_shot_says *say;

	switch (shot) {
	case NN_OV_SHOT_PREP_FAILED:
		say = nn_shot_says(NN_SHOT_PREP);
		break;
	case NN_OV_SHOT_INVOKE_FAILED:
		say = nn_shot_says(NN_SHOT_INVOKE);
		break;
	case NN_OV_SHOT_LEASE_TIMEOUT:
		say = nn_shot_says(NN_SHOT_LEASE_TIMEOUT);
		break;
	case NN_OV_SHOT_NOT_HELD:
		say = nn_shot_says(NN_SHOT_NOT_HELD);
		break;
	case NN_OV_SHOT_NO_OUTPUTS:
		/* This board knows which output; the shared sentence does not. */
		nn_detail_set("output %u of the model is unreadable", out_index);
		return NN_SVC_ERR_HW;
	case NN_OV_SHOT_PUBLISHED:
		/* Finished and published, but the record did not take it (a
		 * generation that moved): nothing of this run to report. */
		nn_detail_set("the result was not accepted by the record");
		return NN_SVC_ERR_HW;
	case NN_OV_SHOT_STOPPED:
	default:
		nn_detail_set("the inference worker ended the run without a "
		              "result (%d)", shot);
		return NN_SVC_ERR_HW;
	}
	nn_detail_set("%s", say->text);
	return say->status;
}
