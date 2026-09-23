/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_detail.h
 * @brief   The bound on a board's `nn` explanation, and its build-time check
 *          (issue #122 P15).
 *
 * Split out of nn_svc.h so that the tables a board's words come from -- a
 * backend's status names, a bring-up's refusals, an asset store's verdicts --
 * can check their sentences against the bound without including the whole `nn`
 * contract.  Includes nothing but <stddef.h>, so any C or C++ file may use it.
 *
 * [!] svc/ SHARED TABLES DO NOT INCLUDE THIS.  A shared layer that names the
 * `nn` field's size would be depending upward; their sentences are enumerated
 * against NN_SVC_DETAIL_MAX by a host test instead (test_nn_detail_check.c).
 */
#ifndef NN_DETAIL_H
#define NN_DETAIL_H

#include <stddef.h>

/** Longest board-written explanation carried back with a result. */
#define NN_SVC_DETAIL_MAX 159

/*
 * [!] A BOARD'S LITERAL WORDING IS CHECKED AGAINST THAT BOUND AT BUILD TIME
 * (issue #122 P15).  The copy into @ref nn_op_result::detail truncates, and a
 * truncated sentence does not look truncated -- one board's refusal ended
 * "... or see `d (-76)" on hardware, the half it cut being the advice.  Nothing
 * noticed, because nothing compared the sentence with the field.
 *
 * NN_SVC_DETAIL_CHECK(s) is a compile-time zero that fails the build when the
 * string LITERAL @p s (a format included) is longer than NN_SVC_DETAIL_MAX;
 * NN_SVC_DETAIL_LIT(s) is @p s itself, checked.  `"" s` refuses anything that
 * is not a literal, so a pointer cannot slip past as "checked".
 *
 * WHAT IT CANNOT SEE: text a conversion expands at run time -- a name, a
 * number, a sentence picked from a table.  A table of whole sentences is
 * checkable, so a board wraps each of its entries in NN_SVC_DETAIL_LIT; a
 * composition of a literal with an argument is not, and remains the author's
 * arithmetic.
 */
#ifndef __cplusplus
#define NN_SVC_DETAIL_CHECK(s)                                                 \
	(0u * sizeof(struct {                                                  \
		_Static_assert(sizeof("" s) <= (size_t)NN_SVC_DETAIL_MAX + 1u, \
		               "detail literal is longer than NN_SVC_DETAIL_MAX"); \
		char nn_detail_check_;                                         \
	}))
#define NN_SVC_DETAIL_LIT(s)  (NN_SVC_DETAIL_CHECK(s) + ("" s))
/** The same check on the FORMAT of a printf-shaped call: its first argument.
 *  A board's `nn_detail_set(fmt, ...)` puts this in front of the formatter. */
#define NN_SVC_DETAIL_FIRST_(fmt, ...) fmt
#define NN_SVC_DETAIL_CHECK_FMT(...) \
	NN_SVC_DETAIL_CHECK(NN_SVC_DETAIL_FIRST_(__VA_ARGS__, 0))
#endif /* !__cplusplus */

#ifdef __cplusplus
/*
 * The same check for a C++ table.  A struct cannot be defined inside sizeof in
 * C++, so the literal's length is taken as a template argument and asserted
 * there; `"" s` still refuses a pointer.
 */
template <size_t N>
constexpr const char *nn_svc_detail_lit_(const char (&s)[N])
{
	static_assert(N <= (size_t)NN_SVC_DETAIL_MAX + 1u,
	              "detail literal is longer than NN_SVC_DETAIL_MAX");
	return s;
}
#define NN_SVC_DETAIL_LIT(s)  (nn_svc_detail_lit_("" s))
#endif

#endif /* NN_DETAIL_H */
