/*
 * ThreadX low-level glue interface for Grove Vision AI V2 (HX6538 / CM55M).
 */
#ifndef TX_GLUE_H
#define TX_GLUE_H

#include <stdint.h>

/* Open the SysTick -> _tx_timer_interrupt() gate.  Called at the END of
   tx_application_define(), once the ThreadX timer lists exist. */
void tx_glue_timer_enable(void);

/* 1 if _tx_initialize_low_level accepted SystemCoreClock and started SysTick;
   0 if the runtime sanity checks failed (the tick is NOT running -- the boot
   banner must report it; sleeping ThreadX services will not advance). */
int tx_glue_systick_ok(void);

/* The SystemCoreClock value SysTick was programmed from (Hz); 0 if rejected. */
uint32_t tx_glue_core_hz(void);

/*
 * ---- Execution Profile Kit (`thread` cpu%, issue #25) ---------------------
 *
 * The kit is compiled in unconditionally (tx_user.h) and cannot be turned off
 * at runtime.  What CAN fail at runtime is its *accuracy*: the TIMER2 time
 * source has to come up, and the vendor-installed UART0 vector has to be
 * wrapped so ISR time is attributed to (isr) rather than to whichever thread
 * was interrupted.  These entry points let the console backend arm the ISR
 * accounting and let the shared `thread` command say when the numbers are not
 * trustworthy, instead of printing plausible-looking wrong ones.
 */

/* ISR-time accounting hooks, called from the FIRST and LAST statement of every
   plain-C ISR this port owns (SysTick, and the UART0 wrapper).  No-ops until
   tx_glue_profile_enable(). */
void tx_glue_isr_enter(void);
void tx_glue_isr_exit(void);

/* Arm the hooks above, and record what must STAY true for the numbers to mean
   anything: `irqn` is the one external interrupt whose vector this port has
   wrapped, and `vector` is the wrapper that was installed for it.
   tx_glue_profile_ok() re-checks both on every query, so a vendor call that
   reinstalls its own handler later, or a second interrupt source enabled
   without an accounting wrapper, downgrades cpu% to "--" instead of silently
   mis-billing ISR time.

   Called from the console backend's enable() -- i.e. on the shell thread,
   after the swap has been verified -- so the armed flag flips 0->1 only in
   thread context and no single ISR invocation can see it change between its
   enter and its exit. */
void tx_glue_profile_arm(int irqn, uint32_t vector);

/* Record the first reason the cpu% accounting became untrustworthy.  `why`
   must be a string literal (it is stored by pointer, not copied).  Later calls
   are ignored: the first failure is the interesting one. */
void tx_glue_profile_fail(const char *why);

/*
 * 1 when the cpu% numbers can be believed, 0 otherwise with *why (when
 * non-NULL) set to a one-line reason.  This is what the shared `thread`
 * command reads through its weak cli_thread_cpu_source_ok() hook.
 *
 * This RE-VALIDATES at every call rather than reporting a boot-time verdict --
 * the boot checks establish that the accounting was correct when it was armed,
 * which is not the same claim as "it is correct now".  Each call re-reads the
 * TIMER2 configuration and confirms it is still counting, re-reads the wrapped
 * vector, confirms the EPK nesting counter is balanced, and confirms no
 * interrupt other than the accounted one is enabled at the NVIC.  Call it from
 * thread context.
 */
int tx_glue_profile_ok(const char **why);

/* Raw EPK time-source state, for the `epk` diagnostic command.  hz is the
   frequency TIMER2 was brought up at (SCU reference / divider, 0 if the
   bring-up failed); ticks is the raw free-running count in the same direction
   the kit sees it (i.e. increasing). */
uint32_t tx_glue_epk_timer_hz(void);
uint32_t tx_glue_epk_timer_ticks(void);

/* The SCU reference clock and divider tx_glue_epk_timer_hz() was derived
   from, for the same diagnostic.  Both 0 when the bring-up failed. */
uint32_t tx_glue_epk_ref_hz(void);
uint32_t tx_glue_epk_clkdiv(void);

#endif /* TX_GLUE_H */
