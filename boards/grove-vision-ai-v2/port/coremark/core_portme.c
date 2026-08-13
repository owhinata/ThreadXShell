/*
 * CoreMark port for Grove Vision AI V2 (Himax HX6538 / CM55M) -- run as the
 * shell `coremark` command.
 *  - Timing : the ThreadX tick, tx_time_get() (EE_TICKS_PER_SEC == 1000).
 *             There is no HAL tick on this board and the SDK's own SysTick use
 *             is compiled out by ENABLE_OS, so ThreadX IS the millisecond
 *             timebase (port/threadx/tx_glue.c programs the reload from the
 *             SCU-read core clock).  The other two candidates are deliberately
 *             not used here: DWT CYCCNT is shared with udelay()/membench and
 *             stops in WFI, and TIMER2 is owned by the execution profile kit.
 *             cmds/bench_gate.c refuses to start a run when that tick is not
 *             running -- CoreMark's auto-calibration loop would never exit.
 *  - Output : printf, retargeted by the shell backend's _write to the console
 *             of the thread that ran it.
 * Derived from EEMBC's barebones core_portme.c (Apache-2.0).
 */
#include "coremark.h"

#include "tx_api.h"

#ifndef ITERATIONS
#define ITERATIONS 0   /* 0 -> CoreMark auto-calibrates to run 10..100 s */
#endif

/* tx_time_get() counts ThreadX ticks; tx_user.h fixes that at 1 kHz. */
#define EE_TICKS_PER_SEC 1000

#if (TX_TIMER_TICKS_PER_SECOND != EE_TICKS_PER_SEC)
#error "CoreMark timing assumes a 1 kHz ThreadX tick"
#endif

#if (SEED_METHOD == SEED_VOLATILE)
/* Performance run seeds (0, 0, 0x66) -> canonical CoreMark score. */
volatile ee_s32 seed1_volatile = 0x0;
volatile ee_s32 seed2_volatile = 0x0;
volatile ee_s32 seed3_volatile = 0x66;
volatile ee_s32 seed4_volatile = ITERATIONS;
volatile ee_s32 seed5_volatile = 0;
#endif

static CORETIMETYPE start_time_val, stop_time_val;

void start_time(void)
{
    start_time_val = (CORETIMETYPE)tx_time_get();
}

void stop_time(void)
{
    stop_time_val = (CORETIMETYPE)tx_time_get();
}

CORE_TICKS get_time(void)
{
    return (CORE_TICKS)(stop_time_val - start_time_val);
}

secs_ret time_in_secs(CORE_TICKS ticks)
{
    return ((secs_ret)ticks) / (secs_ret)EE_TICKS_PER_SEC;
}

ee_u32 default_num_contexts = 1;

void portable_init(core_portable *p, int *argc, char *argv[])
{
    (void)argc;
    (void)argv;

    /* The board is already up: main() ran the SDK platform init and the
     * scheduler is running the shell thread that called us.  This port must
     * NOT touch hardware -- re-initialising anything here would disturb the
     * live console, and the clock tree is inherited from the bootloader and
     * never reconfigured by this app at all. */

    if (sizeof(ee_ptr_int) != sizeof(ee_u8 *))
    {
        ee_printf(
            "ERROR! Please define ee_ptr_int to a type that holds a "
            "pointer!\n");
    }
    if (sizeof(ee_u32) != 4)
    {
        ee_printf("ERROR! Please define ee_u32 to a 32b unsigned type!\n");
    }
    p->portable_id = 1;
}

void portable_fini(core_portable *p)
{
    p->portable_id = 0;
}

/* MEM_METHOD == MEM_STATIC: core_main.c owns a TOTAL_DATA_SIZE .bss array and
 * never calls portable_malloc/portable_free, so this port supplies neither. */
