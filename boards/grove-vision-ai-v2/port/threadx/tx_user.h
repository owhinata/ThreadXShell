/*
 * tx_user.h - ThreadX build-time configuration for Grove Vision AI V2 (HX6538).
 *
 * Included by the Cortex-M55 GNU port assembly (unconditionally) and by the C
 * core + shell when TX_INCLUDE_USER_DEFINE_FILE is defined (set in CMake), so
 * every translation unit that sees TX_THREAD agrees on its layout (ABI match).
 */
#ifndef TX_USER_H
#define TX_USER_H

/* ThreadX tick rate.  The SysTick handler (tx_glue.c) calls
   _tx_timer_interrupt() once per SysTick; the reload is derived at runtime
   from SystemCoreClock (the bootloader-configured CM55M clock, read back via
   the SCU driver) for a 1 ms period.  1 tick = 1 ms. */
#ifndef TX_TIMER_TICKS_PER_SECOND
#define TX_TIMER_TICKS_PER_SECOND  1000
#endif

/*
 * [!] Whole app runs in the TrustZone SECURE state (SDK SEC_ONLY: SAU
 * disabled, every address secure).  TX_SINGLE_MODE_SECURE makes the port
 * match that reality:
 *  - the initial thread EXC_RETURN becomes 0xFFFFFFFD (same-state, PSP) --
 *    without this define the port builds 0xFFFFFFBC, which returns into the
 *    NON-SECURE world on the first PendSV, and no such world is configured
 *  - the six secure-stack sources compile to empty objects and the port emits
 *    no SVC_Handler (dual-world secure-stack management is not needed)
 *  - TX_THREAD loses the tx_thread_secure_stack_context extension word
 * ABI-affecting, therefore defined HERE and not as a -D on one target.
 */
#define TX_SINGLE_MODE_SECURE

/* The Execution Profile Kit is deliberately NOT enabled on this board (M-G1):
 * the outermost UART ISR lives inside the prebuilt libdriver.a where the EPK
 * isr_enter/exit hooks cannot be placed, so its ISR accounting would be
 * silently wrong.  The `thread` command's cpu%% columns are #ifdef-guarded and
 * simply absent.  Re-enabling it later means wrapping the vendor-installed
 * vectors (see the board README, "future work"). */

/* TX_PORT_USE_BASEPRI is left undefined so the Cortex-M55 GNU port uses
 * PRIMASK critical sections: a driver ISR calling tx_event_flags_set can then
 * never preempt a ThreadX critical section, whatever its NVIC priority (the
 * SDK's prebuilt drivers register their ISRs at priority 0). */

#endif /* TX_USER_H */
