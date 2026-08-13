/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_membench.c
 * @brief   `membench` shell command: memory bandwidth + latency at cycle
 *          precision (Grove Vision AI V2, issue #25).
 *
 * A self-contained micro-benchmark (ported from the wio-lite-ai `membench`,
 * itself borrowing only the two core ideas of lmbench -- bw_mem sequential
 * read/write/copy bandwidth and lat_mem_rd pointer-chase latency) that measures
 * this board's physical memories with the Cortex-M55 DWT cycle counter.
 *
 * Regions, in MEMORY-HIERARCHY order:
 *   ITCM  ( 4 KB, .itcm_bench) -- READ ONLY and no latency column.  ITCM is
 *         where the whole application executes from on this board (the app is
 *         not XIP; the 2nd bootloader loads code here), so the write/copy legs
 *         and the chase construction -- all of which write -- are deliberately
 *         not run next to live code.  The read rate is the number worth having
 *         beside DTCM's.
 *   DTCM  ( 4 KB, .dtcm_bench)
 *   SRAM  (64 KB, .sram_bench) -- the SRAM0 window at 0x3401F000, and the only
 *         working set here large enough to overflow an L1 D-cache.  Also
 *         measured at 4 KB so the pair reads as a cache-residency comparison
 *         rather than an address one.
 *
 * [!] THE BUFFERS ARE NOLOAD, SO NOTHING INITIALISES THEM.  They are outside
 * the startup's copy and zero tables by construction (that is what keeps them
 * out of the flashed image), which means their contents after reset are
 * whatever the memory happened to hold -- and on a Cortex-M55 whose TCM ECC
 * state is inherited from the bootloader, READING A LOCATION THAT WAS NEVER
 * WRITTEN CAN RAISE AN ECC FAULT.  So every run starts by writing every buffer
 * end to end (bench_bufs_init) before a single read, and only then builds the
 * chase chains.
 *
 * [!] AND THE CONDITIONS ARE PRINTED WITH THE NUMBERS.  There is no public TRM
 * for this part, and the MPU / cache / TCM / prefetcher state is INHERITED from
 * the bootloader -- this app never programs it.  Whether the SRAM rows are
 * cacheable, write-back or write-through is therefore not something the
 * firmware can honestly assert, and decoding it properly (overlapping regions,
 * inclusive limits, PRIVDEFENA, shareability) is more machinery than the answer
 * is worth.  So `membench` DUMPS THE RAW REGISTERS and lets the reader decide;
 * the SRAM rows are labelled "as the CPU sees it", not "cached".
 *
 * Timing: DWT CYCCNT, never reset -- svc/timebase.c's udelay() reads the same
 * free-running counter, and zeroing it here would cut short a concurrent
 * `usleep N &`.  Each timed run is sized to ~0.3 ms (under one 1 kHz tick) by a
 * calibration pass, run up to MEMBENCH_TRIALS times; runs during which the
 * ThreadX tick advanced (an interrupt fired) are rejected and the minimum over
 * the clean runs is reported.  Interrupts are never disabled.  Cancel with
 * Ctrl+C between cells.
 *
 * DCE/line-reuse defeat: reads go through `const volatile`, a volatile sink
 * ends each loop, and the latency walk is a dependent load chain `idx =
 * buf[idx]` in PSEUDO-RANDOM order (a rising stride is exactly what a hardware
 * prefetcher is built to follow, which would measure the prefetcher instead of
 * the memory).
 *
 * A singleton guard rejects a second concurrent run: the shared static buffers
 * and sink would otherwise be raced.  Linked into the shell firmware only.
 * Clean-room glue.
 */
#include "cli.h"
#include "bench_gate.h"

#include "WE2_device.h"      /* DWT/CoreDebug/SCB/MPU/MEMSYSCTL, barriers */
#include "tx_api.h"          /* tx_time_get: the tick-pollution detector   */

#include <stdint.h>
#include <stdio.h>           /* snprintf for table cells */
#include <string.h>          /* strcpy */

/* ---- working sets ----------------------------------------------------------
 *
 * Sizes: the TCMs have no cache cliff (they ARE the fast path), so a small set
 * measures the same raw rate as a large one and 4 KB keeps them cheap.  SRAM
 * gets both 4 KB and 64 KB; 64 KB is chosen to exceed any plausible L1 D-cache
 * on this core (the M55 tops out at 64 KB, and at the 64 B chase stride a 64 KB
 * set touches 1024 distinct lines).
 */
#define ITCM_BENCH_BYTES  ( 4u * 1024u)
#define DTCM_BENCH_BYTES  ( 4u * 1024u)
#define SRAM_BENCH_BYTES  (64u * 1024u)
#define SRAM_SMALL_BYTES  ( 4u * 1024u)

/* The `used` attribute stops the COMPILER from dropping these; the ldscript's
 * KEEP stops --gc-sections from doing the same.  Both are needed, and the
 * section names are what check_placement_budget.py asserts residency on. */
static uint32_t itcm_bench_buf[ITCM_BENCH_BYTES / 4]
	__attribute__((aligned(32), section(".itcm_bench"), used));
static uint32_t dtcm_bench_buf[DTCM_BENCH_BYTES / 4]
	__attribute__((aligned(32), section(".dtcm_bench"), used));
static uint32_t sram_bench_buf[SRAM_BENCH_BYTES / 4]
	__attribute__((aligned(32), section(".sram_bench"), used));

/* Volatile sink: every measured loop ends by storing into it, so -O2 cannot
 * eliminate the loop as dead code. */
static volatile uint32_t g_sink;

/* Harness tuning. */
#define MEMBENCH_TRIALS     16u   /* attempts to find tick-clean runs        */
#define MEMBENCH_CLEAN       3u   /* stop once this many clean runs are seen */
#define MEMBENCH_MAX_ITERS  100000u
#define MEMBENCH_TARGET_DIV 3333u /* clk / 3333 ~= 0.3 ms of cycles per run  */

typedef void (*work_fn)(void *ctx);

/* ---- reentrancy guard ------------------------------------------------------ */

static volatile uint8_t membench_busy;

static int membench_try_acquire(void)
{
	uint32_t pm = __get_PRIMASK();
	int ok;

	__disable_irq();
	ok = !membench_busy;
	if (ok)
		membench_busy = 1u;
	__set_PRIMASK(pm);
	return ok;
}

/* ---- DWT cycle counter ------------------------------------------------------ */

static int dwt_enable(void)
{
	int attempt;

	if (DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk)
		return -1;                  /* CYCCNT not implemented on this core */

	/* Enable, then self-test that the counter actually advances; retry a few
	 * times re-asserting trace-enable.  If it never advances, abort cleanly
	 * rather than let calibration see a zero delta -> a hang.
	 *
	 * Never write DWT->CYCCNT: it is the shared free-running timebase that
	 * svc/timebase.c's udelay() (the `usleep`/`sleep` busy-wait) reads, so
	 * zeroing it here would corrupt a concurrent `usleep N &`'s elapsed delta
	 * and end its wait early.  Test advancement with a wrap-safe delta from a
	 * sampled value instead (CYCCNTENA is idempotent; timebase_init already
	 * enabled it).  Armv8-M has no DWT->LAR software lock to unlock, unlike
	 * the M7 boards. */
	for (attempt = 0; attempt < 3; attempt++) {
		volatile uint32_t spin = 64u;
		uint32_t a;

		CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
		DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

		a = DWT->CYCCNT;
		while (spin--)
			__NOP();
		if ((uint32_t)(DWT->CYCCNT - a) != 0u)
			return 0;           /* counting */
	}
	return -1;
}

/* ---- NOLOAD buffer initialisation ------------------------------------------ */

/*
 * Write every word of every buffer before anything reads one.  See the file
 * header: these live in NOLOAD sections, so no startup code has ever touched
 * them, and a first-ever read of an ECC-protected TCM word is not guaranteed to
 * be benign.  Also gives the read legs a defined value instead of whatever
 * survived the last boot.
 */
static void bench_bufs_init(void)
{
	uint32_t i;

	for (i = 0u; i < ITCM_BENCH_BYTES / 4u; i++)
		itcm_bench_buf[i] = i;
	for (i = 0u; i < DTCM_BENCH_BYTES / 4u; i++)
		dtcm_bench_buf[i] = i;
	for (i = 0u; i < SRAM_BENCH_BYTES / 4u; i++)
		sram_bench_buf[i] = i;
	__DSB();
	__ISB();
}

/* ---- timed harness ---------------------------------------------------------- */

/* Pick an iteration count for `fn` so one run is ~target cycles (~0.3 ms). */
static void calibrate(work_fn fn, void *ctx, uint32_t *iter, uint32_t init,
                      uint32_t target)
{
	uint32_t c0, c1, total, per, it;

	*iter = init;
	__DSB(); __ISB();
	c0 = DWT->CYCCNT;
	fn(ctx);
	__DSB(); __ISB();
	c1 = DWT->CYCCNT;
	total = c1 - c0;
	if (total < init) {     /* counter not advancing -> keep reps at 1 (anti-hang) */
		*iter = 1u;
		return;
	}
	per = total / init;     /* now per >= 1 */
	it = target / per;
	if (it < 1u)
		it = 1u;
	if (it > MEMBENCH_MAX_ITERS)
		it = MEMBENCH_MAX_ITERS;
	*iter = it;
}

/* Run fn (warm-up discarded) and return the min cycle count over tick-clean
 * runs; falls back to min-of-all if no clean run is found.  bench_gate_check()
 * has already established that the tick is running, so "the tick did not
 * advance" really does mean "no SysTick fired here". */
static uint32_t timed_min(work_fn fn, void *ctx)
{
	uint32_t best_clean = 0xFFFFFFFFu, best_any = 0xFFFFFFFFu;
	uint32_t clean = 0u, i;

	fn(ctx);                            /* warm-up -> discard */
	for (i = 0u; i < MEMBENCH_TRIALS && clean < MEMBENCH_CLEAN; i++) {
		uint32_t t0 = (uint32_t)tx_time_get();
		uint32_t c0, c1, t1, dc;

		__DSB(); __ISB();
		c0 = DWT->CYCCNT;
		fn(ctx);
		__DSB(); __ISB();
		c1 = DWT->CYCCNT;
		t1 = (uint32_t)tx_time_get();

		dc = c1 - c0;
		if (dc < best_any)
			best_any = dc;
		if (t1 == t0) {                 /* no tick fired across the run */
			clean++;
			if (dc < best_clean)
				best_clean = dc;
		}
	}
	return clean ? best_clean : best_any;
}

/* ---- bandwidth (bw_mem) ----------------------------------------------------- */

struct bw_ctx {
	const volatile uint32_t *src;
	volatile uint32_t       *dst;
	uint32_t                 words;   /* words touched per scan */
	uint32_t                 reps;    /* scans per timed run    */
};

static void bw_read_work(void *vctx)
{
	struct bw_ctx *c = (struct bw_ctx *)vctx;
	const volatile uint32_t *p = c->src;
	uint32_t words = c->words, reps = c->reps, r, i, acc = 0u;

	for (r = 0u; r < reps; r++) {
		for (i = 0u; i + 8u <= words; i += 8u) {
			acc += p[i + 0] + p[i + 1] + p[i + 2] + p[i + 3];
			acc += p[i + 4] + p[i + 5] + p[i + 6] + p[i + 7];
		}
		for (; i < words; i++)
			acc += p[i];
	}
	g_sink = acc;
}

static void bw_write_work(void *vctx)
{
	struct bw_ctx *c = (struct bw_ctx *)vctx;
	volatile uint32_t *p = c->dst;
	uint32_t words = c->words, reps = c->reps, r, i;
	uint32_t v = g_sink + 0x9E3779B9u;   /* derive from volatile -> not const-folded */

	for (r = 0u; r < reps; r++) {
		for (i = 0u; i + 8u <= words; i += 8u) {
			p[i + 0] = v; p[i + 1] = v; p[i + 2] = v; p[i + 3] = v;
			p[i + 4] = v; p[i + 5] = v; p[i + 6] = v; p[i + 7] = v;
		}
		for (; i < words; i++)
			p[i] = v;
	}
}

static void bw_copy_work(void *vctx)
{
	struct bw_ctx *c = (struct bw_ctx *)vctx;
	const volatile uint32_t *s = c->src;
	volatile uint32_t *d = c->dst;
	uint32_t words = c->words, reps = c->reps, r, i;

	for (r = 0u; r < reps; r++)
		for (i = 0u; i < words; i++)
			d[i] = s[i];
}

static uint32_t bw_mbps(uint32_t cycles, uint64_t bytes, uint32_t clk)
{
	if (cycles == 0u)
		return 0u;
	return (uint32_t)((bytes * (uint64_t)clk) / ((uint64_t)cycles * 1000000ULL));
}

/* ---- latency (lat_mem_rd) --------------------------------------------------- */

struct lat_ctx {
	const volatile uint32_t *buf;
	uint32_t                 k;    /* chase accesses per timed run */
};

/*
 * Build a single-cycle chase over the working set as WORD INDICES: nodes are
 * 64 B = 16 words apart and buf[node*16] holds the next node's word index.
 *
 * The order is a PSEUDO-RANDOM permutation, not i -> i+1.  A rising stride is
 * the access pattern hardware prefetchers exist to recognise (MEMSYSCTL->PFCR
 * on this core configures one), so a sequential chain would measure how well
 * the prefetcher hides the latency rather than the latency.  The permutation is
 * a full-period LCG modulo n: with n a power of two, a multiplier that is
 * 1 mod 4 and an odd increment, x -> a*x + c visits every node exactly once
 * before returning to the start (Hull-Dobell), so the chain is one cycle over
 * the whole set and needs no scratch array to build -- which matters, because
 * there is no heap to spare here.  Deterministic: same layout every run.
 */
static void build_chase(uint32_t *buf, uint32_t wss_bytes)
{
	const uint32_t a = 1103515245u;   /* == 1 (mod 4) */
	const uint32_t c = 12345u;        /* odd          */
	uint32_t n = wss_bytes / 64u;
	uint32_t mask, cur, i;

	if (n < 2u) {
		buf[0] = 0u;                  /* degenerate: chase itself */
		return;
	}
	mask = n - 1u;                    /* n is a power of two by construction */
	cur  = 0u;
	for (i = 0u; i < n; i++) {
		uint32_t next = (a * cur + c) & mask;
		buf[cur * 16u] = next * 16u;
		cur = next;
	}
	__DSB();
}

static void lat_work(void *vctx)
{
	struct lat_ctx *c = (struct lat_ctx *)vctx;
	const volatile uint32_t *b = c->buf;
	uint32_t idx = 0u, k = c->k;

	while (k--)                          /* each load address depends on the */
		idx = b[idx];                /* previous result -> serialized chain */
	g_sink = idx;
}

/* Tenths of a nanosecond per access: cycles/k accesses at clk. */
static uint32_t lat_ns_tenths(uint32_t cycles, uint32_t k, uint32_t clk)
{
	if (k == 0u || clk == 0u)
		return 0u;
	return (uint32_t)(((uint64_t)cycles * 10000000000ULL) / ((uint64_t)clk * k));
}

/* ---- formatting / rows ------------------------------------------------------ */

static void fmt_u(char *buf, size_t n, uint32_t v)
{
	snprintf(buf, n, "%lu", (unsigned long)v);
}

static void fmt_ns(char *buf, size_t n, uint32_t tenths)
{
	snprintf(buf, n, "%lu.%lu", (unsigned long)(tenths / 10u),
	         (unsigned long)(tenths % 10u));
}

/* One bandwidth row: read always; write/copy only when `writable` (ITCM is
 * read-only here).  read/write span `words`; copy moves the first half into the
 * second half. */
static void bw_row(struct cli_instance *sh, const char *label, uint32_t *base,
                   uint32_t words, uint32_t clk, int writable)
{
	struct bw_ctx c;
	char rds[12], wrs[12], cps[12];
	uint32_t target = clk / MEMBENCH_TARGET_DIV;

	c.src = base; c.dst = base; c.words = words; c.reps = 1u;
	calibrate(bw_read_work, &c, &c.reps, 1u, target);
	fmt_u(rds, sizeof rds,
	      bw_mbps(timed_min(bw_read_work, &c), (uint64_t)words * 4u * c.reps, clk));

	if (writable) {
		uint32_t half = words / 2u;

		c.src = base; c.dst = base; c.words = words; c.reps = 1u;
		calibrate(bw_write_work, &c, &c.reps, 1u, target);
		fmt_u(wrs, sizeof wrs,
		      bw_mbps(timed_min(bw_write_work, &c), (uint64_t)words * 4u * c.reps, clk));

		c.src = base; c.dst = base + half; c.words = half; c.reps = 1u;
		calibrate(bw_copy_work, &c, &c.reps, 1u, target);
		fmt_u(cps, sizeof cps,
		      bw_mbps(timed_min(bw_copy_work, &c), (uint64_t)half * 4u * c.reps, clk));
	} else {
		strcpy(wrs, "--");
		strcpy(cps, "--");
	}

	cli_print(sh, "  %-22s %8s %8s %8s\r\n", label, rds, wrs, cps);
}

/*
 * One latency cell over the requested working set.  The chain is rebuilt each
 * time (the set size changes), and the lines it touches are pushed out of the
 * D-cache first so the first pass measures a real fill rather than the residue
 * of the row above -- SCB_CleanInvalidateDCache_by_Addr is a no-op in effect
 * when the cache is off, and the barriers order the maintenance against the
 * timed loads either way.
 */
static uint32_t lat_ns10(uint32_t *buf, uint32_t wss_bytes, uint32_t clk)
{
	struct lat_ctx c;
	uint32_t target = clk / MEMBENCH_TARGET_DIV;

	build_chase(buf, wss_bytes);
	SCB_CleanInvalidateDCache_by_Addr(buf, (int32_t)wss_bytes);
	__DSB();
	__ISB();

	c.buf = buf;
	c.k = 256u;
	calibrate(lat_work, &c, &c.k, 256u, target);
	return lat_ns_tenths(timed_min(lat_work, &c), c.k, clk);
}

/* ---- measurement conditions ------------------------------------------------- */

/*
 * Print the raw state the numbers were produced under.  DELIBERATELY UNDECODED:
 * this app never programs the MPU or the caches -- both are inherited from the
 * bootloader -- and there is no public TRM to check an interpretation against.
 * Working out the effective attribute of an address from these registers means
 * handling region overlap priority, the inclusive limit encoding, PRIVDEFENA
 * and shareability, and getting any of that subtly wrong would put a confident
 * wrong word ("cached") next to a correct number.  Dumping is honest and the
 * reader can decode what they need.
 *
 * MEMSYSCTL is worth as much as the MPU here: FORCEWT changes what a write-back
 * region actually does, DCACTIVE/ICACTIVE say whether the caches are really
 * running, PFCR configures the prefetcher the latency column is written to
 * defeat, and ITCMCR/DTCMCR say whether the TCMs are even enabled.
 */
static void print_conditions(struct cli_instance *sh, uint32_t clk)
{
	uint32_t ctrl = MPU->CTRL;
	uint32_t type = MPU->TYPE;
	uint32_t nreg = (type >> 8) & 0xFFu;
	uint32_t saved = MPU->RNR;
	uint32_t i;

	cli_print(sh, "conditions (raw; this app never programs the MPU or the "
	              "caches -- both are inherited from the bootloader)\r\n");
	cli_print(sh, "  clock      %lu Hz (SCU CM55M read-back; DWT CYCCNT counts "
	              "core clocks)\r\n", (unsigned long)clk);
	cli_print(sh, "  mode       Secure, privileged, Thread\r\n");
	cli_print(sh, "  SCB->CCR   %08lx  (DC=%lu IC=%lu)\r\n",
	          (unsigned long)SCB->CCR,
	          (unsigned long)((SCB->CCR & SCB_CCR_DC_Msk) ? 1u : 0u),
	          (unsigned long)((SCB->CCR & SCB_CCR_IC_Msk) ? 1u : 0u));
	cli_print(sh, "  MSCR       %08lx  (DCACTIVE=%lu ICACTIVE=%lu FORCEWT=%lu "
	              "ECCEN=%lu)\r\n",
	          (unsigned long)MEMSYSCTL->MSCR,
	          (unsigned long)((MEMSYSCTL->MSCR & MEMSYSCTL_MSCR_DCACTIVE_Msk) ? 1u : 0u),
	          (unsigned long)((MEMSYSCTL->MSCR & MEMSYSCTL_MSCR_ICACTIVE_Msk) ? 1u : 0u),
	          (unsigned long)((MEMSYSCTL->MSCR & MEMSYSCTL_MSCR_FORCEWT_Msk) ? 1u : 0u),
	          (unsigned long)((MEMSYSCTL->MSCR & MEMSYSCTL_MSCR_ECCEN_Msk) ? 1u : 0u));
	cli_print(sh, "  PFCR       %08lx  (prefetcher)\r\n",
	          (unsigned long)MEMSYSCTL->PFCR);
	cli_print(sh, "  ITCMCR     %08lx   DTCMCR %08lx\r\n",
	          (unsigned long)MEMSYSCTL->ITCMCR,
	          (unsigned long)MEMSYSCTL->DTCMCR);
	cli_print(sh, "  MPU->TYPE  %08lx  CTRL %08lx (ENABLE=%lu PRIVDEFENA=%lu, "
	              "%lu regions)\r\n",
	          (unsigned long)type, (unsigned long)ctrl,
	          (unsigned long)((ctrl & MPU_CTRL_ENABLE_Msk) ? 1u : 0u),
	          (unsigned long)((ctrl & MPU_CTRL_PRIVDEFENA_Msk) ? 1u : 0u),
	          (unsigned long)nreg);
	cli_print(sh, "  MAIR0      %08lx   MAIR1  %08lx\r\n",
	          (unsigned long)MPU->MAIR0, (unsigned long)MPU->MAIR1);

	for (i = 0u; i < nreg; i++) {
		uint32_t rbar, rlar;

		MPU->RNR = i;
		__DSB();
		rbar = MPU->RBAR;
		rlar = MPU->RLAR;
		if ((rlar & MPU_RLAR_EN_Msk) == 0u)
			continue;               /* disabled region: nothing to say */
		cli_print(sh, "  region %-2lu  RBAR %08lx RLAR %08lx\r\n",
		          (unsigned long)i, (unsigned long)rbar,
		          (unsigned long)rlar);
	}
	MPU->RNR = saved;
	__DSB();
	__ISB();

	cli_print(sh, "  buffers    ITCM %08lx  DTCM %08lx  SRAM %08lx "
	              "(NOLOAD, written before first read)\r\n",
	          (unsigned long)(uintptr_t)itcm_bench_buf,
	          (unsigned long)(uintptr_t)dtcm_bench_buf,
	          (unsigned long)(uintptr_t)sram_bench_buf);
}

/* ---- command ---------------------------------------------------------------- */

static int cmd_membench(struct cli_instance *sh, int argc, char **argv)
{
	int do_itcm = 1, do_dtcm = 1, do_sram = 1;
	uint32_t clk = 0u;
	int rc = 0;

	if (argc >= 2) {
		const char *r = argv[1];

		do_itcm = do_dtcm = do_sram = 0;
		if (!strcmp(r, "all"))        do_itcm = do_dtcm = do_sram = 1;
		else if (!strcmp(r, "itcm"))  do_itcm = 1;
		else if (!strcmp(r, "dtcm"))  do_dtcm = 1;
		else if (!strcmp(r, "sram"))  do_sram = 1;
		else {
			cli_error(sh, "membench: unknown region '%s' "
			              "(itcm|dtcm|sram|all)\r\n", r);
			return 1;
		}
	}

	if (!membench_try_acquire()) {
		cli_error(sh, "membench: already running\r\n");
		return 1;
	}

	if (!bench_gate_check(sh, "membench", &clk)) {
		rc = 1;
		goto done;
	}
	if (dwt_enable() != 0) {
		cli_error(sh, "membench: DWT CYCCNT unavailable on this core\r\n");
		rc = 1;
		goto done;
	}

	bench_bufs_init();
	print_conditions(sh, clk);

	cli_print(sh, "\r\n%-24s %8s %8s %8s\r\n",
	          "bandwidth (MB/s)", "read", "write", "copy");
	if (do_itcm) {
		if (cli_cancel_requested(sh)) goto done;
		/* Read-only on purpose: this is the memory the code runs from. */
		bw_row(sh, "ITCM   ( 4KB, RO)", itcm_bench_buf,
		       ITCM_BENCH_BYTES / 4u, clk, 0);
	}
	if (do_dtcm) {
		if (cli_cancel_requested(sh)) goto done;
		bw_row(sh, "DTCM   ( 4KB)", dtcm_bench_buf,
		       DTCM_BENCH_BYTES / 4u, clk, 1);
	}
	if (do_sram) {
		if (cli_cancel_requested(sh)) goto done;
		bw_row(sh, "SRAM   ( 4KB)", sram_bench_buf,
		       SRAM_SMALL_BYTES / 4u, clk, 1);
		if (cli_cancel_requested(sh)) goto done;
		bw_row(sh, "SRAM   (64KB)", sram_bench_buf,
		       SRAM_BENCH_BYTES / 4u, clk, 1);
	}

	/* Latency vs working-set size (pointer-chase).  One row per size, one
	 * column per writable region -- ITCM has no column (building the chain
	 * would write into the region the code executes from).  If the SRAM
	 * column steps up somewhere and DTCM stays flat, that step is a cache
	 * working-set cliff; whether it is one is for the reader to decide from
	 * the register dump above. */
	if (do_dtcm || do_sram) {
		static const uint32_t sizes_kb[] = { 1u, 2u, 4u, 8u, 16u, 32u, 64u };
		size_t si;

		cli_print(sh, "\r\nlatency (ns/access, dependent-load chase, 64B "
		              "stride, pseudo-random order)\r\n");
		cli_print(sh, "  %6s", "wss");
		if (do_dtcm) cli_print(sh, " %8s", "DTCM");
		if (do_sram) cli_print(sh, " %8s", "SRAM");
		cli_print(sh, "\r\n");

		for (si = 0u; si < sizeof sizes_kb / sizeof sizes_kb[0]; si++) {
			uint32_t wss = sizes_kb[si] * 1024u;
			char cell[12];

			if (cli_cancel_requested(sh))
				goto done;
			cli_print(sh, "  %4lu KB", (unsigned long)sizes_kb[si]);
			if (do_dtcm) {
				if (wss <= DTCM_BENCH_BYTES) {
					fmt_ns(cell, sizeof cell,
					       lat_ns10(dtcm_bench_buf, wss, clk));
					cli_print(sh, " %8s", cell);
				} else
					cli_print(sh, " %8s", "--");
			}
			if (do_sram) {
				fmt_ns(cell, sizeof cell,
				       lat_ns10(sram_bench_buf, wss, clk));
				cli_print(sh, " %8s", cell);
			}
			cli_print(sh, "\r\n");
		}
	}

	/* Everything above was scaled by the clock read at entry; confirm it is
	 * still the clock (a whole run's worth of time has passed since). */
	if (!bench_gate_recheck(sh, "membench", clk))
		rc = 1;

done:
	membench_busy = 0u;   /* single cleanup point: guard cleared on every exit */
	return rc;
}

CLI_CMD_REGISTER_USAGE(membench, NULL, "memory bandwidth + latency benchmark",
                       "[itcm|dtcm|sram|all]", cmd_membench, 1, 1);
