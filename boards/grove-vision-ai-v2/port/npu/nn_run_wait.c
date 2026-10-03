/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_run_wait.c
 * @brief   How `nn run`'s wait ends (issue #129).  See nn_run_wait.h.
 */
#include "nn_run_wait.h"

enum nn_run_end nn_run_wait_step(int published, int finished, int lost,
                                 int cancelled, int expired)
{
	if (published)
		return NN_RUN_INFERRED;
	if (finished)
		return NN_RUN_NO_RESULT;
	if (lost)
		return NN_RUN_LOST;
	if (cancelled)
		return NN_RUN_CANCELLED;
	if (expired)
		return NN_RUN_TIMEOUT;
	return NN_RUN_WAITING;
}

enum nn_run_end nn_run_wait_final(enum nn_run_end end, int published)
{
	if (end == NN_RUN_TIMEOUT && published)
		return NN_RUN_INFERRED;
	return end;
}
