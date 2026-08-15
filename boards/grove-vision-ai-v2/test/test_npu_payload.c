/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/*
 * Host test for the Ethos-U command-stream payload check (issue #46).
 *
 * Compiles the REAL port/npu/npu_payload.c -- the walk has no TFLite and no
 * hardware dependency, which is why it was split out of npu_tflm.cc in the
 * first place.
 *
 * WHAT THIS IS PROTECTING.  The driver launches the NPU from inside its action
 * walk and keeps walking.  A malformed later action makes it return without
 * waiting, so the arena is never handed back and TFLM writes it while the NPU
 * may still be running.  npu_open() refuses such a model.  The interesting
 * cases below are therefore the ACCEPTS that must stay accepted (a real Vela
 * payload) and the REJECTS that must stay rejected (anything after the stream).
 *
 * The word encoding is asserted here as well as in the implementation, because
 * the two have to agree with a THIRD party -- ethosu_driver.c -- and a test
 * that only mirrored the implementation would agree with a wrong one.  The
 * expected byte layouts below were read off the driver's own reassembly:
 *   action word = command | reserved<<8 | length<<16
 *   cms_length  = (reserved << 16) | length
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "npu_payload.h"

#define FOURCC 0x31504F43u   /* "COP1" */

#define ACT_OPT 1u
#define ACT_CMS 2u
#define ACT_NOP 5u

static uint32_t buf[64];

/* Build an action word the way the driver decodes one. */
static uint32_t action(uint32_t cmd, uint32_t reserved, uint32_t length)
{
	return (cmd & 0xFFu) | ((reserved & 0xFFu) << 8) | ((length & 0xFFFFu) << 16);
}

static bool run(unsigned words)
{
	return npu_payload_is_single_final_command_stream((const uint8_t *)buf,
	                                                  (size_t)words * 4u);
}

static void expect(const char *what, unsigned words, bool want)
{
	bool got = run(words);

	if (got != want) {
		printf("  %-52s FAIL (got %s)\n", what, got ? "accept" : "reject");
		exit(1);
	}
	printf("  %-52s %s\n", what, want ? "accept" : "reject");
}

int main(void)
{
	unsigned n;

	printf("test_npu_payload:\n");

	/* --- the shape Vela emits, which must keep working ------------------- */

	/* fourcc, OPTIMIZER_CONFIG (1 + 2 words), COMMAND_STREAM of 3 words. */
	memset(buf, 0, sizeof(buf));
	buf[0] = FOURCC;
	buf[1] = action(ACT_OPT, 0u, 0u);
	buf[2] = 0u;                            /* cfg  */
	buf[3] = 0u;                            /* id   */
	buf[4] = action(ACT_CMS, 0u, 3u);
	/* 3 words of command stream */
	n = 8u;
	expect("optimizer config then a final command stream", n, true);

	/* A command stream with no optimizer config in front of it. */
	memset(buf, 0, sizeof(buf));
	buf[0] = FOURCC;
	buf[1] = action(ACT_CMS, 0u, 2u);
	n = 4u;
	expect("bare final command stream", n, true);

	/* NOPs before the stream are legal; the driver skips them. */
	memset(buf, 0, sizeof(buf));
	buf[0] = FOURCC;
	buf[1] = action(ACT_NOP, 0u, 0u);
	buf[2] = action(ACT_CMS, 0u, 1u);
	n = 4u;
	expect("NOP then a final command stream", n, true);

	/* The length is 24 bits and `reserved` carries the top 8.  If that byte
	 * were ignored the declared stream would be 0 words, the walk would end
	 * neatly at the payload end, and this would be ACCEPTED -- so the reject
	 * below is what proves the byte is being read. */
	memset(buf, 0, sizeof(buf));
	buf[0] = FOURCC;
	buf[1] = action(ACT_CMS, 1u, 0u);       /* cms_length = 1 << 16 = 65536 */
	buf[2] = 0u;
	expect("24-bit length: the reserved byte is the top 8 bits", 3u, false);

	/* --- everything that must be refused --------------------------------- */

	/* THE case this check exists for: an action after the command stream. */
	memset(buf, 0, sizeof(buf));
	buf[0] = FOURCC;
	buf[1] = action(ACT_CMS, 0u, 1u);
	buf[2] = 0u;                            /* the stream word */
	buf[3] = action(ACT_NOP, 0u, 0u);       /* ... and then more */
	n = 4u;
	expect("[!] a NOP AFTER the command stream", n, false);

	memset(buf, 0, sizeof(buf));
	buf[0] = FOURCC;
	buf[1] = action(ACT_CMS, 0u, 1u);
	buf[2] = 0u;
	buf[3] = action(ACT_OPT, 0u, 0u);
	buf[4] = 0u;
	buf[5] = 0u;
	n = 6u;
	expect("[!] an optimizer config AFTER the command stream", n, false);

	/* Two streams: the second launch would be parsed after the first ran. */
	memset(buf, 0, sizeof(buf));
	buf[0] = FOURCC;
	buf[1] = action(ACT_CMS, 0u, 1u);
	buf[2] = 0u;
	buf[3] = action(ACT_CMS, 0u, 1u);
	buf[4] = 0u;
	n = 5u;
	expect("[!] two command streams", n, false);

	/* No stream at all: nothing would run, but nothing would be handed back
	 * either -- and a model that cannot infer is not one to open. */
	memset(buf, 0, sizeof(buf));
	buf[0] = FOURCC;
	buf[1] = action(ACT_NOP, 0u, 0u);
	n = 2u;
	expect("no command stream", n, false);

	/* A length field that runs past the end -- the walk must not read there. */
	memset(buf, 0, sizeof(buf));
	buf[0] = FOURCC;
	buf[1] = action(ACT_CMS, 0u, 0xFFFFu);
	n = 4u;
	expect("command stream length overruns the payload", n, false);

	/* An unknown action: the driver rejects it, so this must too -- and it
	 * must reject rather than guess a length and walk off. */
	memset(buf, 0, sizeof(buf));
	buf[0] = FOURCC;
	buf[1] = action(3u, 0u, 0u);
	n = 2u;
	expect("unknown driver action", n, false);

	/* RESERVED (0) is an action value the driver does not handle. */
	memset(buf, 0, sizeof(buf));
	buf[0] = FOURCC;
	buf[1] = action(0u, 0u, 0u);
	n = 2u;
	expect("action 0 (RESERVED)", n, false);

	memset(buf, 0, sizeof(buf));
	buf[0] = 0xDEADBEEFu;
	buf[1] = action(ACT_CMS, 0u, 0u);
	n = 2u;
	expect("wrong fourcc", n, false);

	/* Sizes the driver itself refuses. */
	memset(buf, 0, sizeof(buf));
	buf[0] = FOURCC;
	expect("fourcc only, no action", 1u, false);
	assert(!npu_payload_is_single_final_command_stream((const uint8_t *)buf, 0u));
	assert(!npu_payload_is_single_final_command_stream((const uint8_t *)buf, 6u));
	assert(!npu_payload_is_single_final_command_stream(NULL, 8u));
	printf("  %-52s reject\n", "empty / not a multiple of 4 / NULL");

	/* A zero-length command stream still consumes its action word, and the
	 * payload ends there -- accepted, because the driver would launch an empty
	 * stream and still reach the wait.  Recorded so the boundary is a decision
	 * and not an accident. */
	memset(buf, 0, sizeof(buf));
	buf[0] = FOURCC;
	buf[1] = action(ACT_CMS, 0u, 0u);
	n = 2u;
	expect("zero-length command stream, still final", n, true);

	printf("test_npu_payload: all passed\n");
	return 0;
}
