/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    main.c
 * @brief   Grove Vision AI V2 (HX6538 / CM55M, Secure) ThreadX shell app.
 *
 * Boot path: the Himax 2nd bootloader loads this ELF into ITCM/DTCM and jumps
 * to Reset_Handler (SDK startup).  SystemInit() sets VTOR/FPU/fault enables
 * and leaves the clock tree alone -- the app INHERITS the bootloader's clocks
 * and only reads the real CM55M frequency back through the SCU driver
 * (platform_driver_init -> SystemCoreClockUpdate).
 *
 * IRQ hygiene (reviewed M-G1 plan): platform_driver_init() runs the SDK's
 * TrustZone SEC_ONLY configuration and the prebuilt drivers' init, which
 * install NVIC vectors and can enable peripheral IRQs (MPC/PPC irq_enable in
 * TZ_Set_ALL_Secure, driver-internal NVIC_EnableIRQ).  All of that runs under
 * PRIMASK here, and every external IRQ is then disabled + pending-cleared
 * before PRIMASK is released -- so no ISR can run before the ThreadX objects
 * exist ("interrupts only after TX object creation", project invariant).  The
 * console UART IRQ is (re-)enabled by the backend's enable(), which runs on
 * the shell thread after the scheduler starts.
 */
#include "WE2_device.h"
#include "pinmux_init.h"
#include "platform_driver_init.h"

#include "tx_api.h"
#include "tx_glue.h"

#include "cli.h"
#include "cli_instance.h"
#include "cli_backend_uart.h"

#define LOG_TAG "main"
#include "log.h"
#include "timebase.h"

#include <stdio.h>

void fault_init(void);          /* src/fault.c */

/* Number of external interrupt lines to sweep in the pre-kernel hygiene pass.
 * The SDK's own SystemInit() sweeps 0..200 as well; real device IRQs top out
 * well below (UART0=90, U55<=194 per WE2_ARMCM55.h). */
#define IRQ_SWEEP_MAX 200

/* --- interactive shell over UART0 (CH343P bridge -> USB-C) --------------- */
CLI_BACKEND_UART_DEFINE(uart_tr);
CLI_INSTANCE_DEFINE(uart_sh, &uart_tr, "grove> ");

/* Gate probe (cmake/check_image_coherence.py): the command-registry span in
 * .rodata must be a whole multiple of this value.  Exported as data so the
 * gate verifies against the sizeof THIS firmware was compiled with, not a
 * constant that could drift from the struct. */
__attribute__((used)) const uint32_t cli_cmd_sizeof_probe =
	(uint32_t)sizeof(struct cli_cmd);

int main(void)
{
	uint32_t pm;
	int irq;

	/* Logging + fault capture first, so everything after leaves a trace. */
	log_init();
	fault_init();
	timebase_init();

	LOG_INF("platform init: begin");

	/* SDK board bring-up under PRIMASK (see the file header).  board.c itself
	 * is deliberately NOT used -- it would wire the SDK clib console; this
	 * calls the two weak init hooks it would have called. */
	pm = __get_PRIMASK();
	__disable_irq();

	pinmux_init();                  /* PB0/PB1 -> UART0 RX/TX (SDK default) */
	platform_driver_init();         /* TZ cfg, SCU init + freq read-back,
	                                 * timers/WDT/RTC/DMA/UART/GPIO/I2C/SPI/PWM
	                                 * driver install (prebuilt libdriver.a) */

	/* Sweep every external IRQ the init above may have enabled or pended. */
	for (irq = 0; irq <= IRQ_SWEEP_MAX; irq++) {
		NVIC_DisableIRQ((IRQn_Type)irq);
		NVIC_ClearPendingIRQ((IRQn_Type)irq);
	}
	__DSB();
	__ISB();
	__set_PRIMASK(pm);

	LOG_INF("platform init: done, core=%lu Hz",
	        (unsigned long)SystemCoreClock);

	/* ThreadX: _tx_initialize_low_level (port/threadx/tx_glue.c) programs
	 * SysTick from the read-back SystemCoreClock; tx_application_define below
	 * creates the shell; then the scheduler starts.  Does not return. */
	tx_kernel_enter();

	for (;;)
		;                           /* unreachable */
}

void tx_application_define(void *first_unused_memory)
{
	(void)first_unused_memory;

	/* Shell instance: create its ThreadX objects + backend, then spawn its
	 * thread.  Fail-soft: a failed cli_init just skips the shell.  The
	 * backend's enable() -- which opens the UART and its IRQ -- runs later,
	 * on the shell thread, after the scheduler starts. */
	if (cli_init(&uart_sh) == 0)
		cli_start(&uart_sh);
	cli_job_pool_init();            /* background-job worker pool (`cmd &`) */

	/* Boot banner via printf -> _write -> the UART TX ring.  Pre-scheduler
	 * _write only enqueues (never waits); the backend flushes the ring once
	 * enable() has armed TX. */
	printf("\r\n%s\r\n", "Grove Vision AI V2 ThreadX Shell");
	printf("core: %lu Hz (SCU read-back)\r\n",
	       (unsigned long)SystemCoreClock);
	if (!tx_glue_systick_ok()) {
		printf("[!] SysTick REJECTED: core clock read-back implausible; "
		       "no ThreadX tick is running\r\n");
		LOG_ERR("systick rejected: core=%lu Hz",
		        (unsigned long)SystemCoreClock);
	}
	if (SystemCoreClock != (uint32_t)CLI_CPU_CYCLES_PER_US * 1000000u) {
		/* Standing guard: CLI_CPU_CYCLES_PER_US (400) matched the SCU
		 * read-back exactly on hardware, and a future bootloader that
		 * changes the clock tree must not silently skew the shell's
		 * cycle-based timing. */
		printf("[!] CLI_CPU_CYCLES_PER_US=%u disagrees with core clock %lu Hz\r\n",
		       (unsigned)CLI_CPU_CYCLES_PER_US,
		       (unsigned long)SystemCoreClock);
		LOG_WRN("cycles/us %u vs core %lu Hz",
		        (unsigned)CLI_CPU_CYCLES_PER_US,
		        (unsigned long)SystemCoreClock);
	}

	/* Open the SysTick -> ThreadX gate LAST (timer lists exist by now). */
	tx_glue_timer_enable();
}
