/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * The QSPI driver's two decisions after a receive and after a failed HAL call
 * (port/qspi/qspi_idle.h, issue #194): may the CCR be rewritten to put FMODE
 * back to indirect write, and does the controller read idle (the receive may
 * report success / the handle may go back to READY).  Neither can be steered
 * on the board -- the controller decides when it is busy -- so the register
 * combinations are walked here.
 *
 * The point that carries the test: a CCR write for a form with neither
 * address nor data sends that instruction again (RM0385 14.3.5 / 14.3.11), so
 * the restore must never be asked for such a form.  Besides the exact answer
 * for every combination, every write the rule asks for is run through a
 * model of the RM's start rule (written here, independently of the header):
 * the value written must not start a command.
 */
#include <stdint.h>
#include <stdio.h>

#include "qspi_idle.h"

static int g_fail;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "test_qspi_idle: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		g_fail = 1; \
	} \
} while (0)

/* CCR fields (RM0385 14.5.6). */
#define IMODE_1   (1u << 8)
#define ADMODE_1  (1u << 10)
#define ADSIZE_24 (2u << 12)
#define DMODE_1   (1u << 24)
#define DMODE_4   (3u << 24)
#define FMODE(m)  ((uint32_t)(m) << 26)
#define DCYC(n)   ((uint32_t)(n) << 18)

#define SR_BUSY   (1u << 5)
#define SR_FLEVEL_FULL (32u << 8)
#define CR_ABORT  (1u << 1)
#define CR_EN     (1u << 0)
#define CR_BASE   (CR_EN | (3u << 24) | (3u << 8) | (1u << 4)) /* prescaler 3,
                                       * FTHRES, SSHIFT: bits the rule ignores */

/*
 * RM0385 14.3.5 "Triggering the start of a command", as it applies to a CCR
 * write: the command starts on that write when no address is needed
 * (ADMODE = 00) and no data must come from the firmware (FMODE = 01 or
 * DMODE = 00).  With an address it waits for AR, with firmware data for DR;
 * the driver writes neither while settling.
 */
static int ccr_write_starts(uint32_t ccr)
{
	uint32_t admode = (ccr >> 10) & 3u;
	uint32_t dmode  = (ccr >> 24) & 3u;
	uint32_t fmode  = (ccr >> 26) & 3u;

	return admode == 0u && (fmode == 1u || dmode == 0u);
}

/* The command forms the driver issues (FMODE left 0; the walk sets it). */
struct form {
	const char *what;
	uint32_t ccr;
	int has_data;
};

static const struct form forms[] = {
	/* no address, no data */
	{ "wren 0x06 / clfsr 0x50", IMODE_1 | 0x06u, 0 },
	{ "chip erase 0xc7", IMODE_1 | 0xC7u, 0 },
	/* address, no data */
	{ "erase 4k 0x20 / 64k 0xd8", IMODE_1 | ADMODE_1 | ADSIZE_24 | 0xD8u, 0 },
	/* no address, data */
	{ "rdsr 0x05 / rdfsr 0x70 / rdid 0x9f / rdvcr 0x85 / wrvcr 0x81",
	  IMODE_1 | DMODE_1 | 0x05u, 1 },
	/* address, data */
	{ "read 0x0b / prog 0x02", IMODE_1 | ADMODE_1 | ADSIZE_24 | DMODE_1 |
	  DCYC(8) | 0x0Bu, 1 },
	{ "qread 0x6b", IMODE_1 | ADMODE_1 | ADSIZE_24 | DMODE_4 | DCYC(10) |
	  0x6Bu, 1 },
};

static void walk_ccr_step(void)
{
	unsigned i, fm, busy;

	for (i = 0; i < sizeof forms / sizeof forms[0]; i++) {
		for (fm = 0; fm < 4; fm++) {
			for (busy = 0; busy < 2; busy++) {
				uint32_t ccr = forms[i].ccr | FMODE(fm);
				uint32_t sr  = busy ? (SR_BUSY | SR_FLEVEL_FULL) : 0u;
				enum qspi_ccr_step got = qspi_ccr_step(sr, ccr);
				enum qspi_ccr_step want;

				if (busy)
					want = QSPI_CCR_REFUSE;   /* CCR not writable */
				else if (fm == 0)
					want = QSPI_CCR_KEEP;
				else if (!forms[i].has_data)
					want = QSPI_CCR_REFUSE;   /* never rewrite it */
				else
					want = QSPI_CCR_RESTORE;
				if (got != want) {
					fprintf(stderr, "test_qspi_idle: %s, fmode %u, busy %u: "
					        "step %d, want %d\n", forms[i].what, fm, busy,
					        (int)got, (int)want);
					g_fail = 1;
				}
				/* A write the rule asks for starts nothing, and the value
				 * it writes reads idle once BUSY is 0. */
				if (got == QSPI_CCR_RESTORE) {
					uint32_t written = ccr & ~QSPI_IDLE_CCR_FMODE;

					if (ccr_write_starts(written)) {
						fprintf(stderr, "test_qspi_idle: %s, fmode %u: the "
						        "restore write %08x starts a command\n",
						        forms[i].what, fm, written);
						g_fail = 1;
					}
					CHECK(qspi_idle_ok(CR_BASE, 0u, written));
				}
				/* The write the rule refuses for a form without data would
				 * indeed have started the instruction again (the model
				 * agrees with the reason for the rule). */
				if (!busy && fm != 0 && !forms[i].has_data &&
				    (forms[i].ccr & ADMODE_1) == 0u)
					CHECK(ccr_write_starts(ccr & ~QSPI_IDLE_CCR_FMODE));
			}
		}
	}
}

/* The four forms (address yes / no x data yes / no) by their fields alone,
 * so the rule is seen to look at DMODE and not at ADMODE or the opcode. */
static void walk_four_forms(void)
{
	unsigned ad, dm, fm;

	for (ad = 0; ad < 4; ad++)
		for (dm = 0; dm < 4; dm++)
			for (fm = 1; fm < 4; fm++) {
				uint32_t ccr = IMODE_1 | (ad << 10) | ADSIZE_24 |
				               ((uint32_t)dm << 24) | FMODE(fm) | 0x5Au;
				enum qspi_ccr_step got = qspi_ccr_step(0u, ccr);

				if (dm == 0)
					CHECK(got == QSPI_CCR_REFUSE);
				else
					CHECK(got == QSPI_CCR_RESTORE);
				if (got == QSPI_CCR_RESTORE)
					CHECK(!ccr_write_starts(ccr & ~QSPI_IDLE_CCR_FMODE));
			}
}

static void walk_idle_ok(void)
{
	unsigned abort, busy, fm;

	for (abort = 0; abort < 2; abort++)
		for (busy = 0; busy < 2; busy++)
			for (fm = 0; fm < 4; fm++) {
				uint32_t cr  = CR_BASE | (abort ? CR_ABORT : 0u);
				uint32_t sr  = busy ? SR_BUSY : 0u;
				uint32_t ccr = IMODE_1 | ADMODE_1 | DMODE_4 | FMODE(fm) | 0x6Bu;
				int want = !abort && !busy && fm == 0;
				int got  = qspi_idle_ok(cr, sr, ccr) != 0;

				if (got != want) {
					fprintf(stderr, "test_qspi_idle: abort %u busy %u fmode %u: "
					        "idle %d, want %d\n", abort, busy, fm, got, want);
					g_fail = 1;
				}
			}
	/* Bits the rule does not own do not change the answer. */
	CHECK(qspi_idle_ok(0xFFFFFFFFu & ~CR_ABORT, ~SR_BUSY,
	                   ~QSPI_IDLE_CCR_FMODE));
	CHECK(!qspi_idle_ok(0u, 0u, FMODE(1)));
	/* The state the board was found in after a successful receive (SR 0,
	 * FMODE 01) is not idle, and the one it later wedged in is not either. */
	CHECK(!qspi_idle_ok(CR_BASE, 0u, forms[5].ccr | FMODE(1)));
	CHECK(!qspi_idle_ok(CR_BASE, SR_BUSY | SR_FLEVEL_FULL,
	                    forms[5].ccr | FMODE(1)));
}

int main(void)
{
	walk_ccr_step();
	walk_four_forms();
	walk_idle_ok();
	if (g_fail)
		return 1;
	printf("test_qspi_idle: OK\n");
	return 0;
}
