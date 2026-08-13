/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    fault.c
 * @brief   Cortex-M55 fault handlers + crash record (Grove Vision AI V2).
 *
 * Strong HardFault/MemManage/BusFault/UsageFault/SecureFault handlers override
 * the SDK startup's weak Default_Handler aliases.  On a fault the crash is
 * recorded to the RAM log (svc/log.c) -- readable with `dmesg` after the next
 * boot when DTCM content survives the reset -- and the board resets so the
 * bootloader re-runs and the console comes back.  If a debugger owns the core
 * it spin-halts instead so SWD post-mortem stays possible.
 *
 * v8-M additions over the M7 siblings:
 *  - SecureFault (vector 7): SFSR/SFAR are dumped alongside CFSR/HFSR.  The
 *    whole app runs Secure (SEC_ONLY), so a SecureFault here means a
 *    configuration bug, not an NS boundary violation.
 *  - PSPLIM: the ThreadX M55 port sets the stack-limit register per thread; an
 *    overflow raises UsageFault with CFSR.STKOF -- it lands here and the CFSR
 *    dump identifies it.
 *
 * The exception entry is captured by a small naked stub shared by all vectors
 * (the type is read back from SCB->ICSR), which hands the C handler the
 * stacked frame and EXC_RETURN.  Clean-room: concept from NuttX armv8-m fault
 * handlers / Zephyr log_panic; no code reused.
 */
#define LOG_TAG "fault"
#include "log.h"

#include <stdint.h>

#include "WE2_device.h"

/* ---- init -------------------------------------------------------------- */

void fault_init(void)
{
	/* SystemInit() already enabled MemManage/Bus/Usage/SecureFault in SHCSR
	 * (SDK device/system_WE2_ARMCM55.c); repeat defensively so this file does
	 * not silently depend on that ordering. */
	SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk | SCB_SHCSR_BUSFAULTENA_Msk |
	              SCB_SHCSR_USGFAULTENA_Msk | SCB_SHCSR_SECUREFAULTENA_Msk;
	/* Trap integer divide-by-zero as a UsageFault (cheap, catches a real bug).
	 * UNALIGN_TRP is left off: memcpy does intentional unaligned accesses. */
	SCB->CCR |= SCB_CCR_DIV_0_TRP_Msk;
	__DSB();
	__ISB();
}

/* ---- helpers ----------------------------------------------------------- */

/* True for addresses in on-chip RAM where a live stack can legitimately sit
 * (secure aliases): DTCM (all stacks in this port), SRAM0/1, SRAM2.  Used to
 * validate the stacked frame before dereferencing it. */
static int addr_in_ram(uint32_t a)
{
	return (a >= 0x30000000u && a < 0x30040000u) ||     /* DTCM 256 KB */
	       (a >= 0x34000000u && a < 0x34200000u) ||     /* SRAM0+SRAM1 2 MB */
	       (a >= 0x36000000u && a < 0x36060000u);       /* SRAM2 384 KB */
}

/* Final resting state: spin only while a debugger owns the core (DHCSR
 * C_DEBUGEN), so SWD post-mortem is possible; otherwise reset the board so the
 * bootloader re-runs and `dmesg` can replay the crash record. */
static void fault_rest(void)
{
	if (CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) {
		for (;;)
			;                       /* busy loop: keep the core attachable */
	}
	NVIC_SystemReset();             /* does not return */
	for (;;)
		;                           /* belt-and-suspenders */
}

/* ---- C fault handler --------------------------------------------------- */

/* `used`: the only reference is the `b fault_handler_c` inside the naked
 * stub's inline asm, which reachability analysis does not see. */
__attribute__((used)) void fault_handler_c(uint32_t *frame, uint32_t exc_return)
{
	static volatile uint32_t in_fault;

	__disable_irq();
	if (in_fault)
		fault_rest();                   /* secondary fault while recording */
	in_fault = 1u;

	/* The stacked frame may itself be unreadable on a stacking fault or a
	 * wild SP: validate the 8-word basic-frame span is in RAM before
	 * dereferencing it, else fall back to a registers-only record. */
	int frame_ok = addr_in_ram((uint32_t)frame) &&
	               addr_in_ram((uint32_t)frame + 31u);

	/* Basic exception frame R0-R3, R12, LR, PC, xPSR is always the lowest 8
	 * words -- on an FPU-extended frame the S0-S15/FPSCR context is stacked
	 * ABOVE it, so frame[0..7] are correct either way; only the frame SIZE
	 * differs, handled in the SP calc. */
	uint32_t lr = 0, pc = 0, xpsr = 0, sp = 0;
	if (frame_ok) {
		lr = frame[5]; pc = frame[6]; xpsr = frame[7];

		/* SP at the fault = frame + frame size (basic 8 words, or 26-word
		 * FPU-extended frame when EXC_RETURN bit4 is clear) + 4 if the stacked
		 * xPSR bit9 flags alignment padding. */
		sp = (uint32_t)frame + ((exc_return & 0x10u) ? 8u : 26u) * 4u;
		if (xpsr & (1u << 9))
			sp += 4u;
	}

	uint32_t cfsr  = SCB->CFSR;
	uint32_t hfsr  = SCB->HFSR;
	uint32_t mmfar = SCB->MMFAR;
	uint32_t bfar  = SCB->BFAR;
	uint32_t sfsr  = SAU->SFSR;         /* v8-M SecureFault status */
	uint32_t sfar  = SAU->SFAR;

	uint32_t vect = SCB->ICSR & SCB_ICSR_VECTACTIVE_Msk;
	const char *name = (vect == 3u) ? "HardFault"   :
	                   (vect == 4u) ? "MemManage"   :
	                   (vect == 5u) ? "BusFault"    :
	                   (vect == 6u) ? "UsageFault"  :
	                   (vect == 7u) ? "SecureFault" : "Fault";

	/* Record to the RAM log -- replayed by `dmesg`. */
	LOG_ERR("%s cfsr=%08lx hfsr=%08lx mmfar=%08lx bfar=%08lx",
	        name, (unsigned long)cfsr, (unsigned long)hfsr,
	        (unsigned long)mmfar, (unsigned long)bfar);
	LOG_ERR("sfsr=%08lx sfar=%08lx",
	        (unsigned long)sfsr, (unsigned long)sfar);
	if (frame_ok)
		LOG_ERR("pc=%08lx lr=%08lx psr=%08lx sp=%08lx exc=%08lx",
		        (unsigned long)pc, (unsigned long)lr, (unsigned long)xpsr,
		        (unsigned long)sp, (unsigned long)exc_return);
	else
		LOG_ERR("frame lost (stacking fault?) frame=%08lx exc=%08lx",
		        (unsigned long)(uintptr_t)frame, (unsigned long)exc_return);

	fault_rest();                       /* reset (or halt under a debugger) */
}

/* ---- naked entry stubs ------------------------------------------------- */

/* One stub for all fault vectors: select MSP/PSP from EXC_RETURN bit2 and
 * pass the frame (r0) + EXC_RETURN (r1) to the C handler.  The C handler
 * reads the precise fault type from SCB->ICSR, so the stubs need not differ. */
__attribute__((naked)) void HardFault_Handler(void)
{
	__asm volatile(
		"tst   lr, #4            \n"
		"ite   eq                \n"
		"mrseq r0, msp           \n"
		"mrsne r0, psp           \n"
		"mov   r1, lr            \n"
		"b     fault_handler_c   \n");
}
void MemManage_Handler(void)   __attribute__((alias("HardFault_Handler")));
void BusFault_Handler(void)    __attribute__((alias("HardFault_Handler")));
void UsageFault_Handler(void)  __attribute__((alias("HardFault_Handler")));
void SecureFault_Handler(void) __attribute__((alias("HardFault_Handler")));
