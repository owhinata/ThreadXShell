/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the floating-point precondition's verdict (issue #42,
 * port/threadx/fp_enforce.c).
 *
 * WHY THIS EXISTS.  The check it belongs to halts the board before the
 * scheduler starts, and the boots that would make it halt cannot be produced:
 * there is no way to ask this part to refuse an FPCCR write, and a `devmem
 * poke` after boot is far too late -- the check runs once, before any thread
 * exists.  So the decision was split out from the halt for exactly this file,
 * because the alternative is shipping a gate nobody has ever seen fail.  This
 * repository has one of those on record already (issue #66: a scan that could
 * not detect a single instruction it named, and passed for months).
 *
 * What this does NOT cover: whether the port actually calls the enforcement,
 * whether the write reaches the real register, and whether the halt halts.  The
 * first is covered by check_placement_budget.py requiring the symbol -- which
 * only works because --gc-sections drops an uncalled function -- and by a
 * fixture that removes the call and watches that gate fail.  The rest is not
 * coverable and is stated as such rather than implied away.
 */
#include <stdio.h>

#include "fp_enforce.h"

static int failures;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
			failures++;                                            \
		}                                                              \
	} while (0)

static const char *name(enum fp_enforce_verdict v)
{
	switch (v) {
	case FP_ENFORCE_OK:            return "ok";
	case FP_ENFORCE_LSPACT:        return "lspact";
	case FP_ENFORCE_ASPEN_REFUSED: return "aspen-refused";
	case FP_ENFORCE_TS_REFUSED:    return "ts-refused";
	}
	return "?";
}

static void expect(uint32_t before, uint32_t after, enum fp_enforce_verdict want)
{
	enum fp_enforce_verdict got = fp_enforce_judge(before, after);

	CHECK(got == want, "judge(%08x, %08x) = %s, want %s",
	      before, after, name(got), name(want));
}

/*
 * TS (issue #103).
 *
 * [!] JUDGED ON `after`, NOT ON `before`, and that is the whole point.  The
 * caller CLEARS this bit in the same write that sets ASPEN, so an inherited
 * TS == 1 is not a refusal -- it is the ordinary case the write exists to fix.
 * What must be refused is a TS that SURVIVED the write, because then every
 * exception taken from Secure state stacks s16-s31 on top of the extended
 * frame: 64 B more than the reserve the plugin admission policy is computed
 * from, and a reserve that is wrong by 64 B is not a reserve.
 */
static void test_ts(void)
{
	printf(" case: FPCCR.TS (issue #103)\n");

	/* The ordinary case: inherited set, cleared by the write. */
	expect(FP_FPCCR_TS, FP_FPCCR_ASPEN, FP_ENFORCE_OK);

	/* The one that must be caught. */
	expect(0u, FP_FPCCR_ASPEN | FP_FPCCR_TS, FP_ENFORCE_TS_REFUSED);

	/* Ordering.  An outstanding lazy save is the older and more serious fact,
	 * so it is reported even when TS also failed to clear: diagnosing a
	 * bootloader handover starts from the frame nobody owns.  And a refused
	 * ASPEN outranks TS too -- without automatic context creation the frame
	 * size question is moot. */
	expect(FP_FPCCR_LSPACT, FP_FPCCR_ASPEN | FP_FPCCR_TS, FP_ENFORCE_LSPACT);
	expect(0u, FP_FPCCR_TS, FP_ENFORCE_ASPEN_REFUSED);
}

int main(void)
{
	/* The board as measured on 2026-08-19: ASPEN and LSPEN set by the
	 * bootloader, no lazy save outstanding.  The write is a no-op and the
	 * read-back agrees. */
	expect(0xD00004FCu, 0xD00004FCu, FP_ENFORCE_OK);

	/* The case the enforcement exists for: a bootloader that left ASPEN
	 * clear.  The write sets it, the read-back confirms, and the verdict is
	 * OK -- enforcing is allowed to succeed, or it would be a check rather
	 * than an enforcement. */
	expect(0x400004FCu, 0xC00004FCu, FP_ENFORCE_OK);

	/* ...and the same boot on a part where the write does not take. */
	expect(0x400004FCu, 0x400004FCu, FP_ENFORCE_ASPEN_REFUSED);
	expect(0x00000000u, 0x00000000u, FP_ENFORCE_ASPEN_REFUSED);

	/*
	 * [!] LSPACT INHERITED, and it must win over everything else.  The
	 * dangerous shape is the one where ASPEN reads back perfectly well and
	 * the only wrong thing is the lazy save left over from whoever ran
	 * first -- if ASPEN were judged first, that boot would report OK.
	 */
	expect(0xD00004FDu, 0xD00004FDu, FP_ENFORCE_LSPACT);
	expect(0x400004FDu, 0xC00004FDu, FP_ENFORCE_LSPACT);
	/* Even when everything else is as broken as it can be. */
	expect(0x00000001u, 0x00000000u, FP_ENFORCE_LSPACT);

	/* And appearing only in the read-back, which nothing between the two
	 * reads can legitimately cause. */
	expect(0xD00004FCu, 0xD00004FDu, FP_ENFORCE_LSPACT);

	/* LSPEN is not judged: both stackings are correct, and the choice is
	 * the bootloader's.  Neither value may change the verdict. */
	expect(0x800004FCu, 0x800004FCu, FP_ENFORCE_OK);   /* eager */
	expect(0xC00004FCu, 0xC00004FCu, FP_ENFORCE_OK);   /* lazy  */

	CHECK(fp_enforce_strerror(FP_ENFORCE_OK) != NULL, "no string for ok");
	CHECK(fp_enforce_strerror((enum fp_enforce_verdict)99) != NULL,
	      "no string for an unknown verdict");

	if (failures != 0) {
		printf("test_fp_enforce: %d failure(s)\n", failures);
		return 1;
	}
	test_ts();
	printf("test_fp_enforce: ok\n");
	return 0;
}
