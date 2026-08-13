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

#endif /* TX_GLUE_H */
