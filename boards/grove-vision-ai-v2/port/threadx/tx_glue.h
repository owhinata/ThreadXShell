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

/*
 * The accounted-interrupt registry (issue #30).
 *
 * EVERY external interrupt this port enables must appear here, wrapper and
 * all -- error and rare lines included.  There is no "enabled but unwrapped"
 * category by design: such a line bills its own runtime to whichever thread it
 * interrupted while tx_glue_profile_ok() still answers 1, which hollows out
 * the guarantee the check exists to give.  A peripheral line not worth
 * wrapping must be left DISABLED and polled instead.
 *
 * Register BEFORE enabling the line at the NVIC, and unregister AFTER
 * disabling it.  A 0 return from the register call means "do not enable this
 * interrupt": losing the feature is the cheap failure, enabling it anyway
 * costs the meaning of every cpu% number the shell prints.
 *
 * Both calls are thread-context only (they mutate the registry inside a
 * PRIMASK critical section; an ISR must never see a half-written entry).
 */
/* Keep this equal to GROVE_EPK_WRAP_MAX (port/sdk_seam/epk_irq_wrap.h), which
 * explains the number: every wrapped line takes one slot here.  A registry
 * smaller than the trampoline pool would make a wrap succeed and its
 * registration fail, which grove_epk_irq_wrap_new() correctly treats as a
 * failed bring-up -- correct, but a self-inflicted one. */
#define TX_GLUE_EPK_MAX_IRQ 32

int  tx_glue_profile_register_irq(int irqn, uint32_t wrapper_vector);
void tx_glue_profile_unregister_irq(int irqn);

/* Arm the hooks above once the TIMER2 time source is up and at least one
   accounted interrupt is registered.  Called from the console backend's
   enable() -- i.e. on the shell thread, after its vector swap has been
   verified -- so the armed flag flips 0->1 only in thread context and no
   single ISR invocation can see it change between its enter and its exit. */
void tx_glue_profile_arm(void);

/**
 * @brief  Details of a replaced accounted-interrupt vector, if one was seen.
 *
 * "A vector was replaced" is not actionable on its own: this port wraps a
 * couple of dozen camera lines, and what matters is WHICH one and whether the
 * value that displaced the wrapper looks like a vendor handler (the driver
 * re-registered) or like nothing at all (something scribbled).
 *
 * @return 0 if no mismatch has been observed.
 */
int tx_glue_profile_bad_vector(int *irqn, uint32_t *want, uint32_t *got);

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
