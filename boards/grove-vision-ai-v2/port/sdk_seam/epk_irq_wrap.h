/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    epk_irq_wrap.h
 * @brief   Wrap vendor-installed ISRs so the execution profile kit can account
 *          for them (issue #30).
 *
 * The console backend does this by hand for UART0, because that is the one
 * interrupt whose number is known in advance.  A peripheral brought up through
 * the prebuilt driver is different: which lines it enables (the peripheral's
 * own, one or more DMA controller lines, error lines) is a property of the
 * binary, not of any header.  Guessing the set and wrapping the guess is the
 * fail-open this port spent issue #25 closing -- an unwrapped-but-enabled line
 * bills its runtime to whichever thread it interrupted while `thread` keeps
 * printing confident cpu% numbers.
 *
 * So the set is MEASURED instead: snapshot NVIC->ISER, run the vendor bring-up
 * with interrupts masked, and wrap + register every line that appeared.  The
 * result is that "enabled" and "accounted" are the same set by construction,
 * which is exactly what tx_glue_profile_ok() re-checks on every query.
 *
 * Usage, on a thread, with the peripheral idle:
 *
 *      struct epk_irq_snapshot snap;
 *      TX_INTERRUPT_SAVE_AREA
 *      TX_DISABLE
 *      grove_epk_irq_snapshot(&snap);
 *      ... vendor bring-up that enables interrupts ...
 *      ok = grove_epk_irq_wrap_new(&snap);
 *      TX_RESTORE
 *      if (!ok) { ...tear the peripheral back down... }
 */
#ifndef GROVE_EPK_IRQ_WRAP_H
#define GROVE_EPK_IRQ_WRAP_H

#include <stdint.h>

/**
 * How many vendor ISRs can be wrapped at once (one static trampoline each).
 *
 * Sized for the camera (issue #35), which is by far the largest consumer.  Its
 * candidate set is at most 26 lines -- sensor control 20/84/85, Timer0 34, and
 * INP/DP/EDM/WDMA 136..157 -- read off WE2_ARMCM55.h and the disassembly of
 * libsensordp.a.  Which of them the vendor bring-up actually enables is not
 * knowable in advance, which is the whole reason the wrap works off a measured
 * ISER diff; 26 is the ceiling, not the expectation.  Add the console UART and
 * the LCD's SPI/DMA lines and the high-water mark is under 32.
 *
 * Raising this costs one trampoline (a few words of ITCM) and one registry slot
 * (TX_GLUE_EPK_MAX_IRQ, which must be raised WITH it) per line.  Running out is
 * not a soft failure: grove_epk_irq_wrap_new() refuses the whole bring-up
 * rather than leave a line enabled but unaccounted, so a peripheral simply does
 * not come up.
 */
#define GROVE_EPK_WRAP_MAX 32

struct epk_irq_snapshot {
	uint32_t iser[16];              /* NVIC->ISER as it was before bring-up */
};

/**
 * What one wrap attempt actually installed.
 *
 * The wrap is a TRANSACTION and this is its undo log.  Without it, a bring-up
 * that wrapped two lines and then failed on the third -- or succeeded entirely
 * and then failed at a later step, like the panel's init table -- would leave
 * those wrappers installed and registered while the caller tore the peripheral
 * down underneath them.  The registry would then point at vectors the vendor's
 * close() had changed, and the accounting would read "not trustworthy" until
 * the next reboot.
 */
struct epk_irq_wrapset {
	int      irqn[GROVE_EPK_WRAP_MAX];
	uint32_t count;
};

/**
 * @brief  Re-take any line in @p set whose vector the vendor has replaced.
 *
 * The measure-then-wrap protocol assumes a bring-up only ADDS enabled lines.
 * A driver may instead re-install an ISR on a line it already had running --
 * after which the vector is the vendor's and the accounting registry still
 * expects the trampoline, so the line stops being accounted and `thread`
 * reports the whole cpu% column as untrustworthy.
 *
 * This adopts the NEW vendor vector as the handler the trampoline calls and
 * puts the trampoline back.  Thread context only.
 *
 * @return 1 if every line in @p set is wrapped afterwards, 0 otherwise.
 */
int grove_epk_irq_reassert(const struct epk_irq_wrapset *set);

/**
 * @brief  How many times a line has had to be re-taken.
 *
 * Non-zero means a vendor driver re-registers ISRs on lines it already has
 * running.  A count that climbs with the frame rate means it does so on every
 * frame, which is worth knowing: it is the difference between "once, at
 * start-up" and "the accounting has to be defended continuously".
 */
uint32_t grove_epk_irq_reasserts(void);

/** Record which interrupts are enabled right now. */
void grove_epk_irq_snapshot(struct epk_irq_snapshot *snap);

/**
 * @brief  Wrap and register every interrupt enabled since @p snap was taken.
 *
 * For each newly enabled line: disable it, read the vector the vendor
 * installed, install a trampoline that brackets it with the profile kit's
 * enter/exit hooks, verify the swap, register it with the accounting registry,
 * and re-enable it.
 *
 * @param out  filled with every line this call DID wrap, success or not, so
 *             the caller can roll the attempt back.  Always written.
 * @return 1 when every new line ended up wrapped AND registered; 0 otherwise,
 *         with the lines that FAILED left DISABLED and the reason latched for
 *         `thread` / `epk`.  A caller that gets 0 must abandon the bring-up --
 *         and must roll @p out back, because the lines that succeeded are
 *         enabled and registered.
 */
int grove_epk_irq_wrap_new(const struct epk_irq_snapshot *snap,
                           struct epk_irq_wrapset *out);

/**
 * @brief  Undo a wrap: disable the line, restore the vendor vector, drop it
 *         from the accounting registry.  Thread context.
 */
void grove_epk_irq_unwrap(int irqn);

/**
 * @brief  Roll a whole wrap attempt back.  Safe to call twice: @p set is
 *         emptied, so a teardown after a failure path already unwound does
 *         nothing.  Thread context.
 */
void grove_epk_irq_unwrap_set(struct epk_irq_wrapset *set);

#endif /* GROVE_EPK_IRQ_WRAP_H */
