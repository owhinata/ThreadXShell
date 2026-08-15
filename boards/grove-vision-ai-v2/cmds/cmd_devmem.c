/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_devmem.c
 * @brief   `devmem` built-in shell command: peek / poke / dump.
 *
 * Grove Vision AI V2 port of the wio-lite-ai cmd_devmem.c (same subcommands,
 * same gating discipline, new region allow-list):
 *
 *   devmem peek <addr> [8|16|32]        read  one 8/16/32-bit word (default 32)
 *   devmem poke <addr> <val> [8|16|32]  write one word, then read it back
 *   devmem dump <addr> [len]            canonical hex+ASCII over [addr, addr+len)
 *
 * devmem is a *dangerous* command: the whole file compiles in only when
 * CLI_ENABLE_DANGEROUS_CMDS is set.
 *
 * Address-range gate: accesses are checked against a compile-time region
 * allow-list.  The map lists the secure aliases of the real on-chip RAMs and
 * the PPB (word-only); everything else -- reserved holes, the peripheral
 * windows, and notably the FLASH XIP alias 0x3A000000 -- is absent.  The XIP
 * window is deliberately NOT listed in M-G1: the app is loaded to SRAM/TCM and
 * whether the QSPI XIP path is left readable behind it after boot is
 * unverified (no public TRM); a stalled AXI access there would hang the shell.
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"

#include <stdint.h>
#include <string.h>

#if CLI_ENABLE_DANGEROUS_CMDS

/* Allowed access widths, as a bitmask carried per region. */
#define W8   0x1u
#define W16  0x2u
#define W32  0x4u
#define WALL (W8 | W16 | W32)

struct devmem_region {
	uint32_t    base;       /* region start */
	uint32_t    size;       /* region length in bytes */
	uint8_t     read;       /* 1 = peek/dump allowed */
	uint8_t     write;      /* 1 = poke allowed */
	uint8_t     widths;     /* bitmask of permitted access widths */
	const char *name;       /* shown in range/width error messages */
};

/*
 * Region allow-list for the HX6538 CM55M secure memory map (WE2_device_addr.h
 * secure aliases).  Only real on-chip RAM and the PPB are listed; a typo lands
 * on the gate's error message instead of a fault.  The PPB is word-only (many
 * registers misbehave on sub-word access), so dump -- byte-granular -- is
 * allowed on RAM only.
 */
static const struct devmem_region devmem_map[] = {
	{ 0x10000000u, 0x00040000u, 1, 1, WALL, "ITCM"  }, /* 256 KB (code!)     */
	{ 0x30000000u, 0x00040000u, 1, 1, WALL, "DTCM"  }, /* 256 KB             */
	{ 0x34000000u, 0x00200000u, 1, 1, WALL, "SRAM01"}, /* SRAM0+SRAM1 2 MB   */
	{ 0x36000000u, 0x00060000u, 1, 1, WALL, "SRAM2" }, /* 384 KB             */
	{ 0xE0000000u, 0x00100000u, 1, 1, W32,  "PPB"   }, /* SCB/NVIC/SysTick/DWT */
	/* Peripheral registers, secure aliases (issue #30).  READ ONLY and
	 * 32-bit only: this window covers the SCU, the timers, every GPIO group
	 * and the SPI/DMA controllers, and a stray write there could reprogram
	 * the clock tree this app is contractually forbidden to touch.  Reading
	 * is how a bring-up gets diagnosed without a debugger -- and this board
	 * has no public TRM, so a raw dump is often the only honest answer. */
	{ 0x50000000u, 0x07000000u, 1, 0, W32,  "PERIPH"}, /* AHB/APB aliases    */
	/* External QSPI NOR, memory-mapped READ alias (issue #44).  Read only --
	 * the write alias is a different window on purpose and is deliberately NOT
	 * listed, because that flash holds the bootloader.  Byte-granular so a
	 * model header can actually be looked at: `nn` parses the flatbuffer in
	 * place here, and when it refuses, the only way to tell "nothing was
	 * flashed" from "flashed at the wrong offset" is to dump the bytes. */
	{ 0x3A000000u, 0x01000000u, 1, 0, WALL, "FLASH-R"}, /* 16 MB, read alias  */
};

/* "8"/"16"/"32" -> access width in bytes (1/2/4). */
static int parse_width(const char *s, uint32_t *bytes)
{
	if (strcmp(s, "8") == 0)  { *bytes = 1; return 0; }
	if (strcmp(s, "16") == 0) { *bytes = 2; return 0; }
	if (strcmp(s, "32") == 0) { *bytes = 4; return 0; }
	return -1;
}

/*
 * Gate one access of `span` bytes starting at `addr`, made up of `elem_bytes`
 * (1/2/4) wide elements, against the region table.  The access must lie
 * wholly inside a single region (64-bit arithmetic avoids wrap at the 4 GiB
 * top), be permitted for its direction, and use a width that region allows.
 * Returns 0 if allowed; otherwise prints the reason and returns -1.
 */
static int devmem_check(struct cli_instance *sh, uint32_t addr, uint32_t span,
                        uint32_t elem_bytes, int want_write)
{
	uint8_t want_w = (elem_bytes == 1) ? W8 : (elem_bytes == 2) ? W16 : W32;
	uint64_t alo = addr;
	uint64_t ahi = (uint64_t)addr + span;           /* exclusive end */
	size_t i;

	for (i = 0; i < sizeof devmem_map / sizeof devmem_map[0]; i++) {
		const struct devmem_region *r = &devmem_map[i];
		uint64_t rlo = r->base;
		uint64_t rhi = (uint64_t)r->base + r->size; /* exclusive end */

		if (alo < rlo || ahi > rhi)
			continue;                       /* not wholly within this region */

		if (want_write ? !r->write : !r->read) {
			cli_error(sh, "devmem: %s not allowed in %s\r\n",
			          want_write ? "write" : "read", r->name);
			return -1;
		}
		if (!(r->widths & want_w)) {
			cli_error(sh, "devmem: %lu-bit access not allowed in %s\r\n",
			          (unsigned long)(elem_bytes * 8u), r->name);
			return -1;
		}
		return 0;
	}
	cli_error(sh, "devmem: 0x%08lx (%lu bytes) not in an allowed region\r\n",
	          (unsigned long)addr, (unsigned long)span);
	return -1;
}

/* Read `width` bytes at `addr` (already gated and aligned). */
static uint32_t mem_read(uint32_t addr, uint32_t width)
{
	uintptr_t a = (uintptr_t)addr;

	switch (width) {
	case 1:  return *(const volatile uint8_t  *)a;
	case 2:  return *(const volatile uint16_t *)a;
	default: return *(const volatile uint32_t *)a;
	}
}

/* Print "0x<addr>: 0x<value>" with the value zero-padded to the access width. */
static void print_cell(struct cli_instance *sh, uint32_t addr, uint32_t width,
                       uint32_t val)
{
	switch (width) {
	case 1:
		cli_print(sh, "0x%08lx: 0x%02lx\r\n",
		          (unsigned long)addr, (unsigned long)val);
		break;
	case 2:
		cli_print(sh, "0x%08lx: 0x%04lx\r\n",
		          (unsigned long)addr, (unsigned long)val);
		break;
	default:
		cli_print(sh, "0x%08lx: 0x%08lx\r\n",
		          (unsigned long)addr, (unsigned long)val);
		break;
	}
}

static int parse_addr_width(struct cli_instance *sh, const char *addr_s,
                            const char *width_s, uint32_t *addr, uint32_t *width)
{
	*width = 4;                                      /* default 32-bit */
	if (cli_parse_u32(addr_s, addr) != 0) {
		cli_error(sh, "devmem: bad address '%s'\r\n", addr_s);
		return -1;
	}
	if (width_s != NULL && parse_width(width_s, width) != 0) {
		cli_error(sh, "devmem: bad width '%s' (use 8/16/32)\r\n", width_s);
		return -1;
	}
	if (*addr % *width != 0) {
		cli_error(sh, "devmem: 0x%08lx not %lu-bit aligned\r\n",
		          (unsigned long)*addr, (unsigned long)(*width * 8u));
		return -1;
	}
	return 0;
}

static int cmd_devmem_peek(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t addr, width;

	if (parse_addr_width(sh, argv[1], argc >= 3 ? argv[2] : NULL,
	                     &addr, &width) != 0)
		return 1;
	if (devmem_check(sh, addr, width, width, 0) != 0)
		return 1;

	print_cell(sh, addr, width, mem_read(addr, width));
	return 0;
}

static int cmd_devmem_poke(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t addr, width, value;
	uintptr_t a;

	if (cli_parse_u32(argv[2], &value) != 0) {
		cli_error(sh, "devmem: bad value '%s'\r\n", argv[2]);
		return 1;
	}
	if (parse_addr_width(sh, argv[1], argc >= 4 ? argv[3] : NULL,
	                     &addr, &width) != 0)
		return 1;
	if (width < 4 && value > ((1u << (8u * width)) - 1u)) {
		cli_error(sh, "devmem: value 0x%lx does not fit %lu-bit\r\n",
		          (unsigned long)value, (unsigned long)(width * 8u));
		return 1;
	}
	if (devmem_check(sh, addr, width, width, 1) != 0)
		return 1;

	a = (uintptr_t)addr;
	switch (width) {
	case 1:  *(volatile uint8_t  *)a = (uint8_t)value;  break;
	case 2:  *(volatile uint16_t *)a = (uint16_t)value; break;
	default: *(volatile uint32_t *)a = value;           break;
	}

	print_cell(sh, addr, width, mem_read(addr, width));     /* read-back */
	return 0;
}

static int cmd_devmem_dump(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t addr, len = 64;                        /* default 64 bytes */

	if (cli_parse_u32(argv[1], &addr) != 0) {
		cli_error(sh, "devmem: bad address '%s'\r\n", argv[1]);
		return 1;
	}
	if (argc >= 3 && cli_parse_u32(argv[2], &len) != 0) {
		cli_error(sh, "devmem: bad length '%s'\r\n", argv[2]);
		return 1;
	}
	if (len == 0)
		return 0;                               /* nothing to dump */
	if (len > CLI_DEVMEM_DUMP_MAX_LEN) {
		cli_error(sh, "devmem: length %lu exceeds max %u\r\n",
		          (unsigned long)len, (unsigned)CLI_DEVMEM_DUMP_MAX_LEN);
		return 1;
	}
	/* dump is byte-granular, so it needs an 8-bit-capable region (RAM). */
	if (devmem_check(sh, addr, len, 1, 0) != 0)
		return 1;

	return cli_hexdump_base(sh, (const void *)(uintptr_t)addr, len, addr)
	       == 0 ? 0 : 1;
}

CLI_SUBCMD_SET_CREATE(devmem_subcmds,
	CLI_CMD_ARG_USAGE(peek, NULL, "read  <addr> [8|16|32]",
	                  "<addr> [8|16|32]", cmd_devmem_peek, 2, 1),
	CLI_CMD_ARG_USAGE(poke, NULL, "write <addr> <val> [8|16|32]",
	                  "<addr> <val> [8|16|32]", cmd_devmem_poke, 3, 1),
	CLI_CMD_ARG_USAGE(dump, NULL, "hexdump <addr> [len]",
	                  "<addr> [len]", cmd_devmem_dump, 2, 1),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(devmem, devmem_subcmds,
                 "read/write memory (peek/poke/dump)", NULL, 1, 0);

#endif /* CLI_ENABLE_DANGEROUS_CMDS */
