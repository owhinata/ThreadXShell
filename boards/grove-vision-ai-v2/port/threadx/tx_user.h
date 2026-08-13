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

/*
 * Execution Profile Kit -- the `thread` command's cpu% column (M-G2, #25).
 *
 * Defined here (not via CMake -D) so every translation unit that sees
 * TX_THREAD agrees on its layout: the kit adds
 * tx_thread_execution_time_total / _last_start to TX_THREAD, and tx_api.h
 * auto-includes tx_execution_profile.h -- both only when this is defined.
 * tx_user.h is included by the port asm (tx_thread_schedule.S,
 * tx_thread_context_{save,restore}.S) and by the C core + shell, so all stay
 * ABI-consistent and the port asm emits the _tx_execution_thread_enter/exit
 * hooks.
 *
 * [!] This is a COMPILE-TIME switch with no runtime off-ramp.  M-G1 deferred
 * the kit because the outermost console ISR lives inside the prebuilt
 * libdriver.a; M-G2 solves that by wrapping the vendor-installed vector at
 * runtime (backend/cli_backend_uart.c).  When that wrap -- or the TIMER2
 * bring-up below -- fails, the kit still runs; what the firmware can do is
 * SAY SO, which is why port/threadx/tx_glue.c exports an availability hook
 * that the shared `thread` command reads (cli_thread_cpu_source_ok).
 */
#define TX_EXECUTION_PROFILE_ENABLE

/* Use the Cortex-M execution-profile path (nest counter) for ISR accounting.
 * Mandatory here: this port's TX_THREAD_GET_SYSTEM_STATE() ORs in the IPSR
 * (tx_port.h), so inside an ISR it is never == 1; the non-EPK "== 1" guard in
 * tx_execution_profile.c would drop all ISR time.  The EPK path guards on
 * "truthy && nest_counter == 1", which works once our plain-C ISRs (SysTick +
 * the wrapped UART0 vector) call _tx_execution_isr_enter/exit (via
 * tx_glue_isr_enter/exit). */
#define TX_CORTEX_M_EPK

/*
 * Execution-profile time source = Himax TIMER2, free-running.
 *
 * TIMER2 is a CMSDK-style DOWN counter (CTRL/VALUE/RELOAD/INTSTATUS at +0/+4/
 * +8/+0xC; WE2_S.svd), while the kit assumes a monotonically INCREASING
 * source -- so the source is the bitwise complement of VALUE.  That identity
 * only holds because port/threadx/tx_glue.c programs RELOAD to all-ones: with
 * RELOAD == 0xFFFFFFFF the counter sweeps the whole 32-bit range, so ~VALUE
 * is an exact mod-2^32 up-counter.  Any other RELOAD would make ~VALUE jump
 * at each reload (see the small-RELOAD self-test in tx_glue.c, which is
 * deliberately NOT written as a continuity check for that reason).
 *
 * Chosen over the kit default DWT->CYCCNT because DWT freezes when the core
 * clock is gated by WFI (TX_ENABLE_WFI below): TIMER2 lives on the SB APB1
 * clock and keeps counting while the core sleeps, so cpu%/idle stay correct.
 * DWT stays the udelay/membench time base (both busy-wait in the foreground
 * and never run while the core is in WFI).  Each EPK delta is bounded to
 * <= 1 ms by the SysTick isr hook, far below the 32-bit wrap.
 * TX_EXECUTION_MAX_TIME_SOURCE keeps its 0xFFFFFFFF default (full 32-bit).
 *
 * The address is a literal because this header is also preprocessed into the
 * port assembly and cannot include the SDK's C headers; tx_glue.c carries a
 * _Static_assert tying it back to HX_TIMER2_BASE.
 */
#define TX_GLUE_EPK_TIMER_VALUE_ADDR 0x5500C004UL
#define TX_EXECUTION_TIME_SOURCE \
    ((EXECUTION_TIME_SOURCE_TYPE)(~(*(volatile ULONG *)TX_GLUE_EPK_TIMER_VALUE_ADDR)))

/* Idle power saving: when no thread is ready the Cortex-M55 port inserts
 * DSB;WFI;ISB (tx_thread_schedule.S __tx_ts_wait) instead of busy-spinning, so
 * the core sleeps until an interrupt.  Build-gated by BSP_ENABLE_WFI (CMake,
 * default ON) so an SWD-debug build can be made with -DBSP_ENABLE_WFI=OFF (a
 * WFI-sleeping core is hard to attach without connect-under-reset).
 *
 * [!] Also a compile-time switch: the firmware cannot "detect a problem and
 * fall back to spinning".  So its two preconditions are ENFORCED before the
 * scheduler starts rather than merely checked -- tx_glue.c clears
 * SCB->SCR.SLEEPDEEP and .SLEEPONEXIT, reads them back, and fail-stops if
 * they did not take.  With those clear, WFI is a plain CPU-clock gate: the SB
 * APB1 clock (TIMER2), SysTick and the UART0 IRQ all keep running, and WFI
 * wakes on any enabled IRQ regardless of PRIMASK.  TX_LOW_POWER is NOT used
 * (it would call vendor PM entry/exit hooks around the WFI). */
#if defined(BSP_ENABLE_WFI) && (BSP_ENABLE_WFI)
#define TX_ENABLE_WFI
#endif

/* TX_PORT_USE_BASEPRI is left undefined so the Cortex-M55 GNU port uses
 * PRIMASK critical sections: a driver ISR calling tx_event_flags_set can then
 * never preempt a ThreadX critical section, whatever its NVIC priority (the
 * SDK's prebuilt drivers register their ISRs at priority 0). */

#endif /* TX_USER_H */
