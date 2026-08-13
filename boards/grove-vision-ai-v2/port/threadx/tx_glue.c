/*
 * ThreadX low-level glue for Grove Vision AI V2 (HX6538 / Cortex-M55 / GNU).
 *
 * Adapted from the wio-lite-ai reference; the shape differs where the SDK
 * differs:
 *  - There is no HAL tick here.  The SDK's own SysTick use (a free-running
 *    profiling counter + strong handler) is compiled out by ENABLE_OS, so this
 *    file OWNS SysTick outright: it programs the 1 ms reload itself from the
 *    runtime-read SystemCoreClock (the app inherits the bootloader's clock
 *    tree and never configures PLLs; platform_driver_init() reads the real
 *    CM55M frequency back through the SCU driver before tx_kernel_enter()).
 *  - PendSV (context switch) runs at the lowest priority; SysTick one step
 *    higher.  SysTick MUST outrank PendSV: when no thread is ready ThreadX
 *    idles by spinning inside PendSV with interrupts enabled, and SysTick must
 *    be able to preempt that spin to advance the tick -- else sleeping threads
 *    never wake (deadlock; proven on the F746).  __NVIC_PRIO_BITS is 3 on this
 *    device, so "lowest" is 7, not the M7 boards' 15.
 *  - Critical sections use PRIMASK (TX_PORT_USE_BASEPRI undefined), so a
 *    priority-0 driver ISR calling tx_event_flags_set can never preempt a
 *    ThreadX critical section.
 *  - ThreadX's PendSV_Handler comes from the port asm; with
 *    TX_SINGLE_MODE_SECURE the port emits no SVC_Handler and the startup's
 *    weak Default_Handler keeps that slot (nothing in this app uses SVC).
 *  - No EPK on this board (see tx_user.h) -- so no profile gates and no
 *    dedicated timebase peripheral here.
 */
#include "tx_api.h"
#include "WE2_device.h"

#include "tx_glue.h"

extern VOID  _tx_timer_interrupt(VOID);
extern VOID *_tx_initialize_unused_memory;

/* The threads in this app own their stacks statically, so ThreadX never needs
   the "first unused memory" region; point it at a tiny valid buffer. */
static UCHAR tx_unused_memory[4];

/* Gate so the SysTick ISR does not poke ThreadX timer lists before they exist.
   SysTick is armed below in _tx_initialize_low_level -- which tx_kernel_enter
   calls BEFORE _tx_initialize_high_level builds those lists -- so the first
   ticks can fire during initialization if anything runs with interrupts
   enabled there.  The gate makes that window safe regardless. */
static volatile UINT tx_timer_active = 0u;

/* Runtime-validated SysTick state (tx_glue_systick_ok / tx_glue_core_hz). */
static uint32_t systick_hz = 0u;

/* Sanity bounds for the runtime-read core clock.  The compile-time SDK config
   is a 24 MHz placeholder and the datasheet caps the CM55M at 400 MHz + DVFS
   margin; anything outside [1 MHz, 500 MHz] means the SCU read-back went wrong
   and a tick derived from it would be garbage -- better no tick + a loud
   banner than a plausible-looking one. */
#define TX_GLUE_HZ_MIN 1000000u
#define TX_GLUE_HZ_MAX 500000000u

void _tx_initialize_low_level(void)
{
    uint32_t hz = SystemCoreClock;
    uint32_t reload;

    /* PendSV lowest, SysTick one step above (3-bit NVIC priorities). */
    NVIC_SetPriority(PendSV_IRQn, 7);
    NVIC_SetPriority(SysTick_IRQn, 6);

    _tx_initialize_unused_memory = (VOID *)tx_unused_memory;

    /* 1 ms SysTick from the inherited clock -- only if the value is sane. */
    reload = hz / (uint32_t)TX_TIMER_TICKS_PER_SECOND;
    if (hz >= TX_GLUE_HZ_MIN && hz <= TX_GLUE_HZ_MAX &&
        reload >= 2u && (reload - 1u) <= SysTick_LOAD_RELOAD_Msk)
    {
        SysTick->CTRL = 0u;
        SysTick->LOAD = reload - 1u;
        SysTick->VAL  = 0u;
        SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk |   /* processor clock */
                        SysTick_CTRL_TICKINT_Msk |
                        SysTick_CTRL_ENABLE_Msk;
        systick_hz = hz;
    }
    /* else: systick_hz stays 0; main() reports it on the banner and the
       tx_timer_active gate keeps the (never-firing) handler harmless. */
}

/* Called at the end of tx_application_define(), once the timer lists are set
   up by _tx_initialize_high_level(), to let the SysTick ISR drive ThreadX. */
void tx_glue_timer_enable(void)
{
    tx_timer_active = 1u;
}

int tx_glue_systick_ok(void)
{
    return (systick_hz != 0u) ? 1 : 0;
}

uint32_t tx_glue_core_hz(void)
{
    return systick_hz;
}

/* ENABLE_OS removed the SDK's strong SysTick_Handler; the startup slot is a
   weak Default_Handler alias, so this definition takes the vector. */
void SysTick_Handler(void)
{
    if (tx_timer_active != 0u)
    {
        _tx_timer_interrupt();
    }
}
