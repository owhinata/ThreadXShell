/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_info_hold.c
 * @brief   Where `nn info`'s model section comes from on this board, and which
 *          state it belongs to (issue #199).  See nn_info_hold.h.
 */
#include "nn_info_hold.h"

#include "nn_param_calls.h"   /* enum nn_owner */

void nn_info_hold_of(int gate_taken, unsigned owner, uint32_t seq,
                     struct nn_info_hold *out)
{
	int keeps_still;   /* the holder keeps a load out */

	if (gate_taken) {
		out->from   = (uint8_t)NN_INFO_FROM_GATE;
		keeps_still = 1;
	} else if (owner == (unsigned)NN_OWNER_STREAM) {
		out->from   = (uint8_t)NN_INFO_FROM_STREAM;
		keeps_still = 1;
	} else {
		out->from   = (uint8_t)NN_INFO_FROM_BUSY;
		/* [!] Only an ordinary operation: a SWAP holder is a load or an
		 * unload, which moves the counter inside its hold, and any other
		 * owner is not one this board writes with the gate refused. */
		keeps_still = (owner == (unsigned)NN_OWNER_OP);
	}
	out->seq_known = (uint8_t)(keeps_still && (seq & 1u) == 0u);
	out->seq       = out->seq_known ? seq : 0u;
}
