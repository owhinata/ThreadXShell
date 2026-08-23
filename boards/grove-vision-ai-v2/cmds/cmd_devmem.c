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
 * allow-list.  The map lists the secure aliases of the real on-chip RAMs, the
 * PPB (word-only) and the FLASH XIP read alias; everything else -- reserved
 * holes and the peripheral windows -- is absent.
 *
 * [!] THE XIP ALIAS NEEDS A LEASE, NOT JUST AN ALLOW-LIST ENTRY (issue #90).
 * Being in the map says the address is legal to read.  It says nothing about
 * whether there is anything mapped there, and this window has an owner:
 * port/nor/ brings it up and issue #88's writer will drop it.  Reading it
 * without holding a lease was wrong in two directions:
 *
 *   - before any bring-up the window is DEAD, and a dead window does not fault
 *     and does not read 0xFF -- one register block aliases across all 16 MB, so
 *     `devmem dump 0x3a000000` printed plausible nonsense as flash contents;
 *   - CLI_MAX_BG_JOBS is 2, so a devmem access can be in flight beside another
 *     command, and a writer that sampled "no readers" would drop XIP out from
 *     under it.  [!] This half is BOUNDED and hard to hit: a dump of the alias
 *     is at most CLI_DEVMEM_DUMP_MAX_LEN bytes, and background jobs run below
 *     the foreground one under TX_NO_TIME_SLICE, so `cmd &; cmd2` does not
 *     actually interleave them.  It is the first half above that was observed
 *     lying on hardware.
 *
 * So every access that touches the alias takes NOR_LEASE_DEVMEM first and gives
 * it back on every exit.  Acquiring is also what brings the window up, which is
 * why one answer closes both problems.
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"

#include <stdint.h>
#include <string.h>

#include "nor_flash.h"

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
	/* [!] From nor_flash.h, not restated.  port/nor/ owns this window; a
	 * second copy of its base or size here would be a second declaration of
	 * somebody else's fact, free to drift. */
	{ NOR_XIP_BASE, NOR_SIZE,    1, 0, WALL, "FLASH-R"}, /* 16 MB, read alias  */
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

/*
 * Take the XIP lease if this access touches the flash alias, and only then
 * (issue #90).  `*token` is 0 when no lease was needed, which devmem_leave()
 * treats as "nothing to give back" -- so every caller can pair the two
 * unconditionally and there is no path that returns while still holding one.
 *
 * [!] INTERSECTION, NOT CONTAINMENT.  devmem_check() has already refused
 * anything that straddles a region boundary, so today an access either lies
 * wholly inside the alias or wholly outside it.  Asking the weaker question
 * anyway means this stays correct if the map ever gains an adjacent region:
 * erring towards taking a lease costs a refusal, erring away costs a read of a
 * window somebody is entitled to pull down.
 */
static int devmem_enter(struct cli_instance *sh, uint32_t addr, uint32_t span,
                        uint32_t *token)
{
	uint64_t alo = addr;
	uint64_t ahi = (uint64_t)addr + span;
	uint64_t xlo = NOR_XIP_BASE;
	uint64_t xhi = (uint64_t)NOR_XIP_BASE + NOR_SIZE;

	*token = 0u;
	if (ahi <= xlo || alo >= xhi)
		return 0;                       /* nowhere near the flash window */

	if (nor_acquire(NOR_LEASE_DEVMEM, token) == 0)
		return 0;

	/* Refusals: the port is faulted, a bring-up is in flight, or this
	 * single-instance slot is already held.
	 *
	 * [!] THE SLOT-ALREADY-HELD CASE CANNOT BE PRODUCED FROM A CONSOLE, so do
	 * not go looking for it on hardware.  Background jobs run below the
	 * foreground one under TX_NO_TIME_SLICE, so two devmem commands never
	 * overlap by typing.  test/test_nor_state.c walks it instead -- the same
	 * reason nor_state.h gives for keeping the decisions as pure functions. */
	if (nor_lifecycle_state() == NOR_ST_FAULTED)
		cli_error(sh, "devmem: flash window unusable: %s\r\n",
		          nor_fail_reason() ? nor_fail_reason() : "faulted");
	else
		cli_error(sh, "devmem: flash window busy\r\n");
	return -1;
}

static void devmem_leave(uint32_t token)
{
	(void)nor_release(token);       /* 0 is "nothing held" and is not an error */
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
	uint32_t addr, width, token;

	if (parse_addr_width(sh, argv[1], argc >= 3 ? argv[2] : NULL,
	                     &addr, &width) != 0)
		return 1;
	if (devmem_check(sh, addr, width, width, 0) != 0)
		return 1;
	if (devmem_enter(sh, addr, width, &token) != 0)
		return 1;

	print_cell(sh, addr, width, mem_read(addr, width));
	devmem_leave(token);
	return 0;
}

static int cmd_devmem_poke(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t addr, width, value, token;
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
	/* Unreachable for the alias today -- the map marks it read-only, so
	 * devmem_check() refuses first -- but paired here anyway so that making it
	 * writable would be a decision about writing, not an accidental one about
	 * ownership. */
	if (devmem_enter(sh, addr, width, &token) != 0)
		return 1;

	a = (uintptr_t)addr;
	switch (width) {
	case 1:  *(volatile uint8_t  *)a = (uint8_t)value;  break;
	case 2:  *(volatile uint16_t *)a = (uint16_t)value; break;
	default: *(volatile uint32_t *)a = value;           break;
	}

	print_cell(sh, addr, width, mem_read(addr, width));     /* read-back */
	devmem_leave(token);
	return 0;
}

static int cmd_devmem_dump(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t addr, len = 64, token;                 /* default 64 bytes */
	int rc;

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
	/* dump is byte-granular, so it needs an 8-bit-capable region. */
	if (devmem_check(sh, addr, len, 1, 0) != 0)
		return 1;
	if (devmem_enter(sh, addr, len, &token) != 0)
		return 1;

	/* [!] The lease spans the WHOLE walk, not each read.  A dump is capped at
	 * CLI_DEVMEM_DUMP_MAX_LEN so this is not a long window today, but the rule
	 * is about where the transaction ends rather than how big it happens to
	 * be: releasing between lines would mean the bytes after the first could
	 * come from a window somebody was entitled to pull down mid-dump. */
	rc = cli_hexdump_base(sh, (const void *)(uintptr_t)addr, len, addr);
	devmem_leave(token);
	return rc == 0 ? 0 : 1;
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
