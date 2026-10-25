/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_svc_adapter.h
 * @brief   How a board's `nn` adapter builds a result and an `nn info` line
 *          (issue #130).
 *
 * The three adapters (boards/<board>/.../nn_svc_<board>.c) each carried the
 * same few helpers word for word.  They live here once, as static inline, so
 * each adapter TU still gets its own private copy and nothing crosses a TU
 * boundary that did not before.
 *
 * Kept apart from nn_detail.h on purpose: that header includes nothing but
 * <stddef.h> so any C or C++ table may check its sentences against the bound.
 * This one needs the formatter and the whole `nn` contract.
 *
 * [!] NO STORAGE.  Like every shared TU, nothing here may hold mutable state --
 * no static variable, not even a function-local one.  The words a command
 * prints go straight into that command's own result (see below).
 *
 * [!] svc/ DOES NOT INCLUDE A BOARD HEADER OR tx_api.h, and this file is no
 * exception.
 */
#ifndef NN_SVC_ADAPTER_H
#define NN_SVC_ADAPTER_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "fmt.h"
#include "nn_svc.h"

/*
 * [!] THERE IS NO SHARED DIAGNOSTIC BUFFER, and that is the fix for a hazard
 * the first version of the adapters had.  A port adapter cannot print -- it
 * holds no shell instance -- so it writes WHY something failed, and the shared
 * command prints that.  Keeping those words in one static meant two consoles
 * building results at once would overwrite each other's explanation: the second
 * console's sentence would appear under the first console's command.  Writing
 * straight into the caller's result removes the sharing instead of locking it,
 * so the words a command prints are the words that command produced.
 */
static inline void nn_detail_to(char *dst, size_t cap, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	(void)fmt_vsnformat(dst, cap, fmt, ap);
	va_end(ap);
}

/* Every failure path writes into the result it is about to return; both macros
 * name the enclosing function's `res` (a struct nn_op_result *). */
/* [!] Its format is checked against NN_SVC_DETAIL_MAX at build time (issue
 * #122 P15): the copy truncates, and a truncated sentence does not look it. */
#define nn_detail_set(...)                                                  \
	((void)NN_SVC_DETAIL_CHECK_FMT(__VA_ARGS__),                        \
	 nn_detail_to(res->detail, sizeof res->detail, __VA_ARGS__))
#define nn_detail_clear()  (res->detail[0] = '\0')

/* Fill a result in one place, so no path can set a status and forget the
   disposition -- they are two answers and both are always given. */
static inline void nn_result(struct nn_op_result *res, int status,
                             enum nn_claim claim)
{
	res->status = status;
	res->claim  = (uint8_t)claim;
}

/*
 * One `nn info` line through a bounded line builder.  The writer is
 * length-bearing, so the adapter formats its own text and hands over the
 * length -- no formatter crosses the boundary, which is what keeps a %f out of
 * three firmwares.  fmt_vsnformat() is svc/fmt.c's bounded formatter, the same
 * one cli_print uses underneath, so what appears here and what the shell prints
 * elsewhere are formatted by one implementation.
 *
 * [!] 128 AND NOT 80: TRUNCATION ATE THE LINE ENDING.  Grove's image line once
 * ran past 80 and came out as
 *   "... (reservation 131072 B at 0x  code 2400 B ..."
 * -- cut mid-number AND missing its CRLF, so the next line ran on (issue #103).
 * A bounded formatter drops what does not fit, and what did not fit was the
 * terminator that separates this line from the next.
 */
static inline int nn_info_line(nn_svc_write_fn write, void *ctx,
                               const char *f, ...)
{
	char line[128];
	va_list ap;
	int n;

	va_start(ap, f);
	n = fmt_vsnformat(line, sizeof line, f, ap);
	va_end(ap);
	if (n < 0)
		return -1;
	if ((size_t)n >= sizeof line)
		n = (int)sizeof line - 1;
	return write(ctx, line, (size_t)n);
}

#endif /* NN_SVC_ADAPTER_H */
