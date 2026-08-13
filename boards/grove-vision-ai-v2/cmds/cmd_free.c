/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_free.c
 * @brief   `free` shell command: per-region memory usage at runtime.
 *
 * Grove Vision AI V2 port.  The app is loaded (not XIP) into:
 *   ITCM  256 KB @0x10000000  vectors + code + rodata (+ copy/zero tables)
 *   DTCM  256 KB @0x30000000  data + bss + noinit + heap, MSP stack on top
 *   SRAM0 window @0x3401F000  explicit-placement only (empty in M-G1)
 *
 * Pure introspection: reads linker boundary symbols and newlib's malloc
 * accounting; changes no state.  Heap via mallinfo() (not sbrk(0)) for the
 * same reason as the sibling boards: from a ThreadX thread the stock
 * heap-vs-SP comparison is meaningless.
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"

#include <malloc.h>   /* mallinfo / struct mallinfo */
#include <stdint.h>

/* Region geometry -- mirrors the MEMORY block of ldscript/HX6538_CM55M_S.ld. */
#define ITCM_ORIGIN   0x10000000u
#define ITCM_LENGTH   (256u * 1024u)
#define DTCM_ORIGIN   0x30000000u
#define DTCM_LENGTH   (256u * 1024u)
#define SRAM_ORIGIN   0x3401F000u
#define SRAM_LENGTH   0x001E1000u

/*
 * Linker boundary symbols.  Their *addresses* carry the values -- declared as
 * arrays so a bare reference already yields the address without &.
 */
extern uint8_t __unprivileged_flash_end__[];  /* end of ITCM contents (-1) */
extern uint8_t __data_start__[], __data_end__[];
extern uint8_t __bss_end__[];
extern uint8_t _end_noinit[];                 /* end of .noinit (log ring) */
extern uint8_t __HeapBase[], __HeapLimit[];
extern uint8_t __StackLimit[], __StackTop[];  /* MSP stack (top of DTCM) */

static uint32_t sym(const uint8_t s[])
{
	return (uint32_t)(uintptr_t)s;
}

static void row(struct cli_instance *sh, const char *name, uint32_t origin,
                uint32_t length, uint32_t used)
{
	cli_print(sh, "%-6s 0x%08lx %7lu KB  used %7lu B  free %7lu B\r\n",
	          name, (unsigned long)origin, (unsigned long)(length / 1024u),
	          (unsigned long)used, (unsigned long)(length - used));
}

static int cmd_free(struct cli_instance *sh, int argc, char **argv)
{
	struct mallinfo mi = mallinfo();

	/* ITCM contents end at __unprivileged_flash_end__ (defined as `. - 1`
	 * after the zero table; +1 restores the exclusive end). */
	uint32_t itcm_used = sym(__unprivileged_flash_end__) + 1u - ITCM_ORIGIN;

	/* DTCM statics: .data + .bss + .noinit, bump-placed from ORIGIN.  The
	 * heap span and the MSP stack live above and are reported separately. */
	uint32_t dtcm_static = sym(_end_noinit) - DTCM_ORIGIN;
	uint32_t heap_len    = sym(__HeapLimit) - sym(__HeapBase);
	uint32_t stack_len   = sym(__StackTop) - sym(__StackLimit);
	uint32_t dtcm_used   = dtcm_static + (uint32_t)mi.uordblks + stack_len;

	(void)argc;
	(void)argv;

	row(sh, "ITCM", ITCM_ORIGIN, ITCM_LENGTH, itcm_used);
	row(sh, "DTCM", DTCM_ORIGIN, DTCM_LENGTH, dtcm_used);
	row(sh, "SRAM", SRAM_ORIGIN, SRAM_LENGTH, 0u);   /* empty in M-G1 */

	cli_print(sh, "data:   %lu B  bss+noinit: %lu B\r\n",
	          (unsigned long)(sym(__data_end__) - sym(__data_start__)),
	          (unsigned long)(sym(_end_noinit) - sym(__data_end__)));
	cli_print(sh, "heap:   %lu B used / %lu B reserved (mallinfo arena %lu)\r\n",
	          (unsigned long)mi.uordblks, (unsigned long)heap_len,
	          (unsigned long)mi.arena);
	cli_print(sh, "msp:    %lu B reserved @0x%08lx (no fill pattern; "
	              "usage not tracked)\r\n",
	          (unsigned long)stack_len, (unsigned long)sym(__StackLimit));
	return 0;
}

CLI_CMD_REGISTER(free, NULL, "show per-region memory usage", cmd_free, 1, 0);
