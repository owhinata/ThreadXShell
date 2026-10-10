/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_sess_release.c
 * @brief   See nn_sess_release.h.  No storage.
 */
#include "nn_sess_release.h"

int nn_sess_may_release(int sink_drained, int holds, enum nn_sess_who who)
{
	/* Nothing to give back, or a producer that may still be writing the
	 * input tensor: keep it. */
	if (!holds || !sink_drained)
		return 0;
	switch (who) {
	case NN_SESS_WORKER_OUT:
	case NN_SESS_STOP_PARKED:
		return 1;
	case NN_SESS_STOP_BUSY:
		return 0;   /* the worker is still in the model */
	}
	return 0;           /* a caller nobody can explain: keep the hold */
}
