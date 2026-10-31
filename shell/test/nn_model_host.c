/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host stand-in for a board's port/nn/nn_model_board.c (issue #161); see
 * nn_model_host.h.
 */
#include <assert.h>

#include "nn_dispatch_state.h"
#include "nn_model_host.h"

struct nn_dispatch_state nn_model_board_state;

uint32_t nn_host_mask;
int      nn_host_depth;
int      nn_host_enters;
int      nn_host_cycles_start_rc = -1;   /* like a part without CYCCNT */
uint32_t nn_host_cycles_step = 0u;

static uint32_t nn_host_cyccnt;

uint32_t nn_model_board_enter(void)
{
	uint32_t saved = nn_host_mask;

	nn_host_mask = 1u;
	nn_host_depth++;
	nn_host_enters++;
	return saved;
}

void nn_model_board_exit(uint32_t saved)
{
	assert(nn_host_depth > 0);
	assert(nn_host_mask == 1u);      /* nothing opened it inside the section */
	nn_host_depth--;
	nn_host_mask = saved;
}

void nn_model_board_wait_tick(void)
{
	assert(!"the open latch was contended in a single-threaded test");
}

int nn_model_board_cycles_start(void)
{
	return nn_host_cycles_start_rc;
}

uint32_t nn_model_board_cycles(void)
{
	nn_host_cyccnt += nn_host_cycles_step;
	return nn_host_cyccnt;
}
