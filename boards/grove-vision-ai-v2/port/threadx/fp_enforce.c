/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * The floating-point context precondition's verdict (issue #42).  See
 * fp_enforce.h for why it is a pure function and not inline in the glue: the
 * failing boots cannot be produced on hardware, so this is the only part of the
 * check that can ever be seen to fail.
 */
#include "fp_enforce.h"

enum fp_enforce_verdict fp_enforce_judge(uint32_t before, uint32_t after)
{
	/* [!] The inherited state first.  A lazy save that was already
	 * outstanding when this port took over describes a frame belonging to
	 * whoever ran before it -- and the ASPEN write neither fixes that nor
	 * makes it safe to ignore, so asking about ASPEN first would report a
	 * success that was never earned. */
	if ((before & FP_FPCCR_LSPACT) != 0u)
		return FP_ENFORCE_LSPACT;

	if ((after & FP_FPCCR_ASPEN) == 0u)
		return FP_ENFORCE_ASPEN_REFUSED;

	/* And once more on the read-back: nothing here should have started a
	 * lazy save (no exception has been taken between the two reads), so one
	 * appearing means the register is not behaving as this port models it. */
	if ((after & FP_FPCCR_LSPACT) != 0u)
		return FP_ENFORCE_LSPACT;

	/* TS on the READ-BACK, not on `before`: the caller clears it, and what
	 * matters is whether the clear took.  A TS that survives the write means
	 * every exception from Secure state stacks 64 B more than the reserve this
	 * port computes for a plugin callback -- see fp_enforce.h. */
	if ((after & FP_FPCCR_TS) != 0u)
		return FP_ENFORCE_TS_REFUSED;

	return FP_ENFORCE_OK;
}

const char *fp_enforce_strerror(enum fp_enforce_verdict v)
{
	switch (v) {
	case FP_ENFORCE_OK:
		return "ok";
	case FP_ENFORCE_LSPACT:
		return "a lazy FP save was already outstanding";
	case FP_ENFORCE_ASPEN_REFUSED:
		return "FPCCR.ASPEN did not take";
	case FP_ENFORCE_TS_REFUSED:
		return "FPCCR.TS would not clear";
	}
	return "unknown";
}
