/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    npu_payload.c
 * @brief   Ethos-U command-stream payload check (issue #46).  See npu_payload.h.
 *
 * [!] THE CONSTANTS BELOW ARE RESTATED FROM THE DRIVER.
 *
 * ethosu_driver.c keeps the fourcc, the DRIVER_ACTION_e values and the action
 * lengths in the .c file rather than a header, so they cannot be included.
 * They have to match: this walk exists to prove a property of a payload that
 * the DRIVER's walk then relies on, and a disagreement about the encoding
 * would make the proof vacuous rather than merely wrong -- it would accept a
 * payload the driver parses into a different sequence of actions.
 *
 * Every offset here is in 32-bit WORDS, as the driver's are, and every advance
 * is checked against the words REMAINING rather than by forming a pointer and
 * comparing it.  The length fields come out of flash, so the walk has to
 * terminate and stay in bounds for any value they hold.
 */
#include "npu_payload.h"

/* "COP1" as the driver's ETHOSU_FOURCC builds it: '1'<<24|'P'<<16|'O'<<8|'C'. */
#define COP_FOURCC            0x31504F43u

/* DRIVER_ACTION_e.  RESERVED (0) is deliberately absent: unknown means reject. */
#define ACTION_OPTIMIZER_CFG  1u
#define ACTION_COMMAND_STREAM 2u
#define ACTION_NOP            5u

/* DRIVER_ACTION_LENGTH_32_BIT_WORD / OPTIMIZER_CONFIG_LENGTH_32_BIT_WORD. */
#define ACTION_WORDS          1u
#define OPT_CFG_WORDS         2u

bool npu_payload_is_single_final_command_stream(const uint8_t *data,
                                               size_t bytes)
{
	uint32_t total;
	uint32_t i;
	uint32_t streams = 0u;

	if (data == NULL)
		return false;
	/* The fourcc plus at least one action word, and a whole number of words --
	 * the driver refuses a size that is not a multiple of 4 as well. */
	if (bytes < 8u || (bytes % 4u) != 0u)
		return false;
	/* Alignment is the caller's promise; a flatbuffer buffer is 4-byte aligned
	 * and this is read as words. */
	if (((uintptr_t)data & 3u) != 0u)
		return false;

	total = (uint32_t)(bytes / 4u);

	{
		const uint32_t *w = (const uint32_t *)(const void *)data;

		if (w[0] != COP_FOURCC)
			return false;

		i = 1u;                       /* the fourcc word is consumed */
		while (i < total) {
			uint32_t step;

			switch (w[i] & 0xFFu) {   /* driver_action_command */
			case ACTION_OPTIMIZER_CFG:
				step = ACTION_WORDS + OPT_CFG_WORDS;
				break;
			case ACTION_COMMAND_STREAM: {
				/* cms_length is (reserved << 16) | length, exactly as the
				 * driver reassembles it: reserved is the second byte of the
				 * word and length the upper halfword. */
				uint32_t len = (((w[i] >> 8) & 0xFFu) << 16) |
				               ((w[i] >> 16) & 0xFFFFu);

				if (len > total - i - ACTION_WORDS)
					return false;     /* the stream runs off the end */
				step = ACTION_WORDS + len;
				streams++;
				break;
			}
			case ACTION_NOP:
				step = ACTION_WORDS;
				break;
			default:
				return false;         /* the driver rejects it too */
			}

			if (step == 0u || step > total - i)
				return false;
			i += step;

			/* The launch happens inside the command-stream action, so the
			 * moment one has been seen this must already be the end. */
			if (streams > 0u && i != total)
				return false;
		}
	}
	return streams == 1u && i == total;
}
