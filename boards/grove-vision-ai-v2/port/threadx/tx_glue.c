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
 *  - The Execution Profile Kit (`thread` cpu%, issue #25) lives here: this
 *    file OWNS Himax TIMER2 as the kit's free-running time source.
 *  - The WFI preconditions are enforced here (issue #25); see
 *    epk_wfi_enforce_preconditions().
 *
 * [!] TIMER2 OWNERSHIP INVARIANT.  This file is the ONLY place in the firmware
 * that touches TIMER2 -- its SCU clock gate/divider/owner and its four MMIO
 * registers.  The vendor's own TIMER2 entry points (hx_drv_timer_cm55m_start/
 * _stop/_clkdiv, which drive TIMER_ID_2) would overwrite RELOAD from a period
 * in milliseconds and set the timer's interrupt-enable bit, destroying both the
 * free-running sweep the kit's time source assumes and the "no TIMER2 IRQ"
 * property.  cmake/check_placement_budget.py fails the build if any of them
 * survives into the image; a generic hx_drv_timer_hw_start(TIMER_ID_2, ...)
 * cannot be caught by name, so the rule is stated here and in the board README.
 */
#include "tx_api.h"
#include "WE2_device.h"
#include "hx_drv_scu.h"          /* SCU timer clock gate / divider / owner */

#include "tx_glue.h"

#define LOG_TAG "txglue"
#include "log.h"

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

/* ==========================================================================
 *  Execution Profile Kit time source: Himax TIMER2
 * ========================================================================== */

/* CMSDK-style timer register block (WE2_S.svd: CTRL/VALUE/RELOAD/INTSTATUS at
 * +0x00/+0x04/+0x08/+0x0C).  The CTRL bit meanings are the ones documented on
 * hx_drv_timer_GetCtrl() in the SDK's hx_drv_timer.h: bit0 = counter enable,
 * bit3 = interrupt enable (bits 1..2 are external-trigger modes we leave off).
 *
 * The base is spelled as a literal so it can be tied to the address tx_user.h
 * hands the kit, which must be a literal because that header is preprocessed
 * into the port assembly.  Both are checked against the SDK's own macro here,
 * so a future SDK bump that moves TIMER2 breaks the build instead of silently
 * profiling an unmapped address. */
#define EPK_T2_BASE       0x5500C000UL
#define EPK_T2_CTRL       (*(volatile uint32_t *)(EPK_T2_BASE + 0x00u))
#define EPK_T2_VALUE      (*(volatile uint32_t *)(EPK_T2_BASE + 0x04u))
#define EPK_T2_RELOAD     (*(volatile uint32_t *)(EPK_T2_BASE + 0x08u))
#define EPK_T2_INTSTATUS  (*(volatile uint32_t *)(EPK_T2_BASE + 0x0Cu))

#define EPK_T2_CTRL_EN     (1u << 0)
#define EPK_T2_CTRL_IRQEN  (1u << 3)

_Static_assert(EPK_T2_BASE == (uint32_t)HX_TIMER2_BASE,
               "TIMER2 base moved: EPK_T2_BASE no longer matches the SDK's "
               "HX_TIMER2_BASE");
_Static_assert(TX_GLUE_EPK_TIMER_VALUE_ADDR == (EPK_T2_BASE + 0x04u),
               "tx_user.h TX_EXECUTION_TIME_SOURCE does not read TIMER2->VALUE");

/* SCU divider applied to the timer reference clock.  1 = full resolution.  The
 * "reference / divider" relation is the SDK driver's own model of this
 * register (hx_drv_timer_update_clk() computes exactly that), not a
 * TRM-confirmed encoding -- there is no public TRM for this part.  It only
 * affects the ABSOLUTE tick rate reported by the `epk` command; cpu% is a
 * ratio of two deltas over the same window and is independent of it. */
#define EPK_T2_CLKDIV 1u

/* Small-RELOAD self-test.  The reload behaviour at zero (does the counter
 * reload to RELOAD, and does it keep running?) is inferred from CMSDK, not
 * specified in the SVD, so it is measured before the kit is allowed to trust
 * the source.
 *
 * [!] The test is NOT "the inverted value increases monotonically".  The
 * complement of a down-counter is only an up-counter when RELOAD is all-ones
 * (see tx_user.h); with a small RELOAD, ~VALUE jumps by 2^32-(RELOAD+1) at
 * every reload.  So progress is measured modulo RELOAD+1 instead, which is why
 * RELOAD is chosen one less than a power of two: the modulo is a mask.
 *
 * Sized so a reload happens many times inside the loop while one sample-to-
 * sample gap (an APB read from a 400 MHz core) stays far below one cycle, so
 * the modular deltas cannot alias. */
#define EPK_SELFTEST_RELOAD 0x00000FFFu                      /* 4096 states */
#define EPK_SELFTEST_TARGET (4u * (EPK_SELFTEST_RELOAD + 1u))
#define EPK_SELFTEST_SPINS  1000000u
#define EPK_SELFTEST_WRAPS  3u

/* A reload must be RARE compared with the samples that observed it: one per
 * cycle, against the hundreds of samples it takes to walk a cycle.  Without
 * this, an UP-counter would pass the test -- every sample would look like a
 * reload and each modular delta would come out near RELOAD, piling up "elapsed"
 * and "wraps" quickly.  Since the whole point of the self-test is to check the
 * direction and reload behaviour rather than assume them, that would be exactly
 * the wrong thing to pass.  (epk_timer_arm() checks the direction again on the
 * production configuration; this makes the self-test mean what it says.) */
#define EPK_SELFTEST_MIN_SAMPLES_PER_WRAP 16u

/* Bounded spin used to prove the armed counter is moving, and the largest
 * elapsed count that can plausibly be seen across it.  A DOWN counter makes
 * (previous - current) a small positive number; an up-counter (or a source
 * running backwards from what we assume) makes the same unsigned subtraction
 * come out near 2^32, which this bound rejects. */
#define EPK_MOVE_SPINS  100000u
#define EPK_MOVE_MAX    0x0000FFFFu

/* Same check on the re-validation path (tx_glue_profile_ok), which runs
 * whenever somebody types `thread`.  A much smaller cap: a healthy counter
 * moves within one or two APB reads, and a stopped one must not turn a shell
 * command into a visible stall. */
#define EPK_RECHECK_SPINS 4096u

/* EPK state.  epk_timer_ok covers the time source; profile_active additionally
 * covers the ISR hooks, which the console backend arms once it has wrapped the
 * vendor-installed UART0 vector.  Both must hold for cpu% to be believable. */
static uint32_t epk_timer_ok;
static uint32_t epk_ref_hz;      /* SCU reference feeding TIMER2 (Hz)        */
static uint32_t epk_div;         /* divider read back from the SCU           */
static uint32_t epk_hz;          /* epk_ref_hz / epk_div: the TIMER2 rate    */

/* First reason the cpu% numbers stopped being trustworthy.  Stored by pointer:
 * callers pass string literals only. */
static const char *epk_why;

/* What tx_glue_profile_arm() was told to keep re-checking: the one external
 * interrupt this port accounts for, and the wrapper installed for it. */
static int      epk_irqn = -1;
static uint32_t epk_vector;

/* EPK ISR nesting depth (tx_execution_profile.c, TX_CORTEX_M_EPK).  Read from
 * thread context it must be zero -- see tx_glue_profile_ok(). */
extern ULONG _tx_execution_isr_nest_counter;

/*
 * Gate for the kit's ISR hooks.  Separate from tx_timer_active: the isr
 * enter/exit calls must not run before the kit's state is set up by
 * _tx_execution_initialize() (called after tx_application_define(), just
 * before the scheduler starts) NOR before the vendor UART0 vector has been
 * wrapped.  Armed by tx_glue_profile_enable() from the console backend's
 * enable(), i.e. on a thread -- an ISR runs to completion without the thread
 * resuming, so a single ISR invocation always sees one value and its
 * enter/exit stay balanced.
 */
static volatile UINT profile_active = 0u;

void tx_glue_profile_fail(const char *why)
{
    if (epk_why == NULL)
        epk_why = why;
}

/* SCU side of the bring-up: gate the clock on, pin the divider, put the timer
 * under CPU (not PMU) control, and learn the reference frequency.  Everything
 * that can be read back is read back -- an SCU write that silently did not
 * take would produce a plausible-looking but wrong tick rate. */
static int epk_scu_setup(void)
{
    uint32_t div_rb = 0u;
    uint32_t ref    = 0u;
    uint8_t  en_rb  = 0u;

    if (hx_drv_scu_set_timer_clk_en(TIMER_ID_2, 1u) != SCU_NO_ERROR ||
        hx_drv_scu_get_timer_clk_en(TIMER_ID_2, &en_rb) != SCU_NO_ERROR ||
        en_rb != 1u) {
        tx_glue_profile_fail("TIMER2 SCU clock gate did not take");
        return 0;
    }
    if (hx_drv_scu_set_timer_clkdiv(TIMER_ID_2, EPK_T2_CLKDIV) != SCU_NO_ERROR ||
        hx_drv_scu_get_timer_clkdiv(TIMER_ID_2, &div_rb) != SCU_NO_ERROR ||
        div_rb != EPK_T2_CLKDIV || div_rb == 0u) {
        tx_glue_profile_fail("TIMER2 SCU divider did not read back");
        return 0;
    }
    /* CPU-owned, not PMU-triggered: the counter must run because we enabled
     * it, not because a power-management event started it.  The SCU exposes no
     * read-back for this field, so it is the one write here that is not
     * verified; the self-test below covers the observable consequence (the
     * counter actually advances after CTRL.EN is set). */
    if (hx_drv_scu_set_timer_ctrl((uint32_t)TIMER_ID_2, SCU_TIMERCTRL_CPU)
        != SCU_NO_ERROR) {
        tx_glue_profile_fail("TIMER2 SCU owner set failed");
        return 0;
    }
    /* The reference the SDK's own driver model uses for a TIMER_STATE_DC timer
     * (hx_drv_timer_update_clk() -> SB APB1 clock, then / divider). */
    if (hx_drv_scu_get_freq(SCU_CLK_FREQ_TYPE_SB_APB_1_CLK, &ref)
        != SCU_NO_ERROR || ref == 0u) {
        tx_glue_profile_fail("TIMER2 reference clock read failed");
        return 0;
    }

    epk_ref_hz = ref;
    epk_div    = div_rb;
    epk_hz     = ref / div_rb;
    return 1;
}

/* Prove the down-counter reloads the way the time source assumes.  See the
 * EPK_SELFTEST_* comment above for why progress is measured modulo. */
static int epk_selftest(void)
{
    uint32_t prev, cur, elapsed = 0u, wraps = 0u, i;

    EPK_T2_CTRL      = 0u;
    EPK_T2_INTSTATUS = 1u;                    /* w1c, in case boot left one */
    EPK_T2_RELOAD    = EPK_SELFTEST_RELOAD;
    EPK_T2_VALUE     = EPK_SELFTEST_RELOAD;
    __DSB();
    if (EPK_T2_RELOAD != EPK_SELFTEST_RELOAD)
        return 0;

    EPK_T2_CTRL = EPK_T2_CTRL_EN;             /* count; interrupt stays OFF */
    __DSB();
    if (EPK_T2_CTRL != EPK_T2_CTRL_EN)
        return 0;

    prev = EPK_T2_VALUE;
    for (i = 0u; i < EPK_SELFTEST_SPINS && elapsed < EPK_SELFTEST_TARGET; i++) {
        cur = EPK_T2_VALUE;
        if (cur > EPK_SELFTEST_RELOAD)
            return 0;                         /* RELOAD is not being honoured */
        if (cur > prev)
            wraps++;                          /* ..., 1, 0 -> RELOAD */
        elapsed += (prev - cur) & EPK_SELFTEST_RELOAD;   /* mod RELOAD + 1 */
        prev = cur;
    }

    EPK_T2_CTRL = 0u;
    __DSB();
    return (elapsed >= EPK_SELFTEST_TARGET &&
            wraps  >= EPK_SELFTEST_WRAPS &&
            wraps * EPK_SELFTEST_MIN_SAMPLES_PER_WRAP <= i)
           ? 1 : 0;
}

/* Move to the production configuration: RELOAD all-ones, so the counter sweeps
 * the full 32-bit range and the complement tx_user.h hands the kit is an exact
 * mod-2^32 up-counter.  (With RELOAD == 0xFFFFFFFF, ~VALUE == RELOAD - VALUE
 * identically -- the identity the plan calls for is exactly the statement that
 * RELOAD is all-ones, which is what the read-back below verifies.) */
static int epk_timer_arm(void)
{
    uint32_t v0, cur, delta = 0u, i;

    EPK_T2_CTRL      = 0u;
    EPK_T2_INTSTATUS = 1u;
    EPK_T2_RELOAD    = 0xFFFFFFFFu;
    EPK_T2_VALUE     = 0xFFFFFFFFu;
    __DSB();
    if (EPK_T2_RELOAD != 0xFFFFFFFFu)
        return 0;

    EPK_T2_CTRL = EPK_T2_CTRL_EN;
    __DSB();
    if (EPK_T2_CTRL != EPK_T2_CTRL_EN)   /* enabled, and IRQEN still clear */
        return 0;

    v0 = EPK_T2_VALUE;
    for (i = 0u; i < EPK_MOVE_SPINS; i++) {
        cur   = EPK_T2_VALUE;
        delta = (uint32_t)(v0 - cur);
        if (delta != 0u)
            break;
    }
    return (delta != 0u && delta <= EPK_MOVE_MAX) ? 1 : 0;
}

/*
 * Bring TIMER2 up as the kit's time source.  Called from
 * _tx_initialize_low_level(), i.e. before _tx_execution_initialize() samples
 * the source for the first time.  Never fails the boot: on any problem the
 * shell still comes up, cpu% reads "--", and `thread` prints why.
 */
static void epk_timer_init(void)
{
    if (!epk_scu_setup())
        goto fail;

    /* Nothing in this firmware handles a TIMER2 interrupt, and the production
     * CTRL below leaves IRQEN clear -- but the vector is installed by the
     * prebuilt driver at init time and boot may have left the line pending, so
     * shut it at the NVIC as well.  (main()'s pre-kernel sweep does the same
     * for every line; this keeps the property local to the owner.) */
    NVIC_DisableIRQ(TIMER2INT_IRQn);
    NVIC_ClearPendingIRQ(TIMER2INT_IRQn);

    if (!epk_selftest()) {
        tx_glue_profile_fail("TIMER2 down-counter self-test failed");
        goto fail;
    }
    if (!epk_timer_arm()) {
        tx_glue_profile_fail("TIMER2 free-run arm failed");
        goto fail;
    }

    epk_timer_ok = 1u;
    LOG_INF("epk: TIMER2 %lu Hz (ref %lu / %lu)",
            (unsigned long)epk_hz, (unsigned long)epk_ref_hz,
            (unsigned long)epk_div);
    return;

fail:
    /* Leave the timer stopped rather than half-configured: a stuck source
     * makes every EPK delta zero, which the `thread` command renders as "--"
     * even independently of the availability hook. */
    EPK_T2_CTRL = 0u;
    __DSB();
    epk_timer_ok = 0u;
    epk_hz = 0u;
    LOG_ERR("epk: %s", (epk_why != NULL) ? epk_why : "TIMER2 bring-up failed");
}

#ifdef TX_ENABLE_WFI
/*
 * WFI preconditions.  TX_ENABLE_WFI is compiled into the port's idle loop, so
 * there is no runtime path back to busy-spinning -- the preconditions are
 * ENFORCED, not tested.  Both bits must be clear before the scheduler starts:
 *
 *   SCR.SLEEPDEEP    - a deep-sleep WFI can gate clocks this port depends on
 *                      (SysTick and, on this part, the SB APB1 clock that
 *                      TIMER2 -- the cpu% time source -- runs from).  Plain
 *                      sleep gates the CPU clock only.
 *   SCR.SLEEPONEXIT  - would put the core back to sleep on every exception
 *                      return, including the PendSV that is trying to schedule
 *                      a newly-ready thread.
 *
 * Neither is written by this port, but the bootloader's leftover state is not
 * guaranteed, so they are cleared, barrier'd and read back.  If the read-back
 * still shows them set, the sleep behaviour would be something other than what
 * the rest of this file reasons about -- stop rather than run a shell whose
 * timekeeping and wakeups are unanalysed.  log_init() ran in main() well
 * before this, and the DTCM log ring survives a reset, so `dmesg` shows the
 * record after the next boot.
 */
static void epk_wfi_enforce_preconditions(void)
{
    uint32_t scr;

    SCB->SCR &= ~(SCB_SCR_SLEEPDEEP_Msk | SCB_SCR_SLEEPONEXIT_Msk);
    __DSB();
    __ISB();
    scr = SCB->SCR;
    if ((scr & (SCB_SCR_SLEEPDEEP_Msk | SCB_SCR_SLEEPONEXIT_Msk)) != 0u) {
        LOG_ERR("wfi: SCR %08lx keeps SLEEPDEEP/SLEEPONEXIT -- halting",
                (unsigned long)scr);
        __disable_irq();
        for (;;)
            ;
    }
}
#endif /* TX_ENABLE_WFI */

void _tx_initialize_low_level(void)
{
    uint32_t hz = SystemCoreClock;
    uint32_t reload;

    /* PendSV lowest, SysTick one step above (3-bit NVIC priorities). */
    NVIC_SetPriority(PendSV_IRQn, 7);
    NVIC_SetPriority(SysTick_IRQn, 6);

    _tx_initialize_unused_memory = (VOID *)tx_unused_memory;

#ifdef TX_ENABLE_WFI
    epk_wfi_enforce_preconditions();
#endif

    /* Time source for the profile kit, before _tx_execution_initialize()
       samples it (tx_initialize_kernel_enter.c calls us first). */
    epk_timer_init();

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

/* ---- EPK ISR accounting ------------------------------------------------- */

/*
 * The kit's nest counter is a plain read-modify-write and the UART0 ISR
 * (priority 2) can preempt SysTick (priority 6), so both calls run inside a
 * PRIMASK critical section.  No-ops until tx_glue_profile_enable().
 */
void tx_glue_isr_enter(void)
{
    if (profile_active != 0u)
    {
        TX_INTERRUPT_SAVE_AREA
        TX_DISABLE
        _tx_execution_isr_enter();
        TX_RESTORE
    }
}

void tx_glue_isr_exit(void)
{
    if (profile_active != 0u)
    {
        TX_INTERRUPT_SAVE_AREA
        TX_DISABLE
        _tx_execution_isr_exit();
        TX_RESTORE
    }
}

void tx_glue_profile_arm(int irqn, uint32_t vector)
{
    if (epk_timer_ok == 0u) {
        tx_glue_profile_fail("TIMER2 time source unavailable");
        return;
    }
    epk_irqn   = irqn;
    epk_vector = vector;
    profile_active = 1u;
}

/*
 * ---- runtime re-validation ------------------------------------------------
 *
 * The boot-time checks establish that the accounting was correct WHEN IT WAS
 * ARMED.  That is a different claim from "it is correct now", and the gap
 * between them is exactly where a plausible wrong number comes from: a vendor
 * call that reinstalls its own UART handler, a second interrupt source enabled
 * by some later feature and never wrapped, a TIMER2 that stopped.  Nothing in
 * the firmware as it stands does any of those -- the placement gate keeps the
 * vendor timer API out of the image, and only dw_uart_open() installs a vector
 * (checked by disassembling the prebuilt archive) -- but "I checked the
 * binary once" is not a property the running firmware can rely on.
 *
 * So the state is re-read on every query.  It costs a handful of MMIO reads on
 * a path that runs when a human types `thread`.
 */

/* TIMER2 is configured as the production free-running source AND advancing. */
static int epk_timer_still_ok(const char **why)
{
    uint32_t v0, cur, delta = 0u, i;

    if (EPK_T2_CTRL != EPK_T2_CTRL_EN) {
        *why = "TIMER2 control register changed (stopped, or its IRQ enabled)";
        return 0;
    }
    if (EPK_T2_RELOAD != 0xFFFFFFFFu) {
        /* Not pedantry: the kit's time source is the complement of VALUE, and
           that is only an up-counter while RELOAD spans the full range. */
        *why = "TIMER2 reload changed; the time source is no longer monotonic";
        return 0;
    }

    v0 = EPK_T2_VALUE;
    for (i = 0u; i < EPK_RECHECK_SPINS; i++) {
        cur   = EPK_T2_VALUE;
        delta = (uint32_t)(v0 - cur);
        if (delta != 0u)
            break;
    }
    if (delta == 0u || delta > EPK_MOVE_MAX) {
        *why = "TIMER2 is not counting down";
        return 0;
    }
    return 1;
}

/* The wrapped vector is still ours, and it is still the ONLY interrupt that
   can reach the CPU.  An unaccounted enabled IRQ does not corrupt anything --
   it just bills its own runtime to whichever thread it interrupted, which is
   the misattribution this whole mechanism exists to avoid. */
static int epk_vectors_still_ok(const char **why)
{
    uint32_t i;

    if (NVIC_GetVector((IRQn_Type)epk_irqn) != epk_vector) {
        *why = "the accounted interrupt vector was replaced";
        return 0;
    }

    for (i = 0u; i < (sizeof NVIC->ISER / sizeof NVIC->ISER[0]); i++) {
        uint32_t enabled = NVIC->ISER[i];

        if (i == ((uint32_t)epk_irqn >> 5))
            enabled &= ~(1UL << ((uint32_t)epk_irqn & 0x1Fu));
        if (enabled != 0u) {
            *why = "an interrupt with no accounting wrapper is enabled";
            return 0;
        }
    }
    return 1;
}

int tx_glue_profile_ok(const char **why)
{
    const char *reason = NULL;

    if (epk_timer_ok == 0u || profile_active == 0u) {
        reason = (epk_why != NULL) ? epk_why : "ISR accounting not armed";
    } else if (!epk_timer_still_ok(&reason) ||
               !epk_vectors_still_ok(&reason)) {
        /* reason set by the callee */
    } else if (_tx_execution_isr_nest_counter != 0u) {
        /* Read from thread context, so every ISR that entered has left: a
           residual count means an enter without its exit somewhere. */
        reason = "EPK isr nesting is unbalanced";
    } else {
        return 1;
    }

    /* Latch the first failure so a transient one still shows up afterwards. */
    tx_glue_profile_fail(reason);
    if (why != NULL)
        *why = reason;
    return 0;
}

uint32_t tx_glue_epk_timer_hz(void)   { return epk_timer_ok ? epk_hz : 0u; }
uint32_t tx_glue_epk_ref_hz(void)     { return epk_timer_ok ? epk_ref_hz : 0u; }
uint32_t tx_glue_epk_clkdiv(void)     { return epk_timer_ok ? epk_div : 0u; }

/* The count in the same direction the kit sees it (TX_EXECUTION_TIME_SOURCE is
   the complement of the down-counter), so a delta taken here is directly
   comparable with an EPK delta. */
uint32_t tx_glue_epk_timer_ticks(void)
{
    return ~EPK_T2_VALUE;
}

/*
 * Strong override of the shared `thread` command's weak availability hook
 * (shell/cmds/cmd_thread.c).  Lives here rather than in the command because
 * the answer is entirely a property of this port's EPK plumbing.
 */
int cli_thread_cpu_source_ok(const char **why)
{
    return tx_glue_profile_ok(why);
}

/* ENABLE_OS removed the SDK's strong SysTick_Handler; the startup slot is a
   weak Default_Handler alias, so this definition takes the vector. */
void SysTick_Handler(void)
{
    tx_glue_isr_enter();   /* EPK: charge this ISR to (isr), not to the thread */

    if (tx_timer_active != 0u)
    {
        _tx_timer_interrupt();
    }

    tx_glue_isr_exit();
}
