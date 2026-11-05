/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    qspi_flash.c
 * @brief   QSPI NOR flash driver (N25Q128A / MT25QL128) -- indirect mode.
 *
 * See qspi_flash.h for the API contract (thread-context only, per-operation
 * mutex, no memory-mapped access).  Hardware setup:
 *
 *   - QUADSPI on AHB3 (= HCLK 216 MHz), prescaler 3 -> SCLK 54 MHz.  FAST_READ
 *     0x0B is rated to 108 MHz, so 54 MHz is a 2x derating and keeps every
 *     1-line command (including plain READ's 54 MHz ceiling) legal.
 *   - Pins (UM1907 Table 13): PB2=CLK AF9, PB6=NCS AF10 (pull-up), PD11=IO0,
 *     PD12=IO1, PD13=IO3, PE2=IO2 all AF9.  PB2 doubles as BOOT1; it is only
 *     sampled at reset, so driving it as CLK at run time is harmless.
 *   - 8 dummy cycles for 0x0B is the device power-on default (VCR[7:4]=0xF
 *     yields 15, but the default *applied* count for 1-line fast read is 8 per
 *     the datasheet command table) -- no configuration-register write needed.
 *
 * Busy-wait policy: page program (~0.5 ms typ) spins on the status register
 * with a HAL_GetTick timeout; erase operations (0.25..250 s) poll WIP with a
 * tx_thread_sleep(1) between reads so other threads keep running.
 */
#include "qspi_flash.h"
#include "qspi_idle.h"

#include "stm32f7xx_hal.h"
#include "tx_api.h"

#define LOG_TAG "qspi"
#include "log.h"

/* N25Q128A command set (datasheet/ST component values).  All 1-1-1 except
 * 0x6B, which returns data on 4 lines (instruction/address stay 1-line). */
#define CMD_READ_ID        0x9Fu
#define CMD_FAST_READ      0x0Bu   /* 1-1-1, 8 dummy cycles @ <=108 MHz */
#define CMD_QUAD_READ      0x6Bu   /* FAST READ QUAD OUTPUT, 1-1-4      */
#define CMD_WRITE_ENABLE   0x06u
#define CMD_PAGE_PROGRAM   0x02u
#define CMD_ERASE_SUB      0x20u   /*  4 KB */
#define CMD_ERASE_SECTOR   0xD8u   /* 64 KB */
#define CMD_ERASE_CHIP     0xC7u
#define CMD_READ_SR        0x05u
#define CMD_READ_FSR       0x70u
#define CMD_CLEAR_FSR      0x50u
#define CMD_READ_VCR       0x85u   /* volatile configuration register */
#define CMD_WRITE_VCR      0x81u

#define SR_WIP             0x01u
#define FSR_PROT_ERR       0x02u
#define FSR_VPP_ERR        0x08u
#define FSR_PGM_ERR        0x10u
#define FSR_ERASE_ERR      0x20u
#define FSR_ANY_ERR        (FSR_PROT_ERR | FSR_VPP_ERR | FSR_PGM_ERR | FSR_ERASE_ERR)

#define DUMMY_CYCLES_READ   8u    /* device default for 0x0B (VCR[7:4]=0xF) */
#define DUMMY_CYCLES_QUAD   10u   /* 0x6B needs 10 -> programmed into VCR   */

/* VCR targets: [7:4] dummy cycles, bit3 XIP disable, [1:0] wrap (default).
 * Power-on default is 0xFB (dummy = per-command default). */
#define VCR_QUAD_TARGET    ((uint8_t)((DUMMY_CYCLES_QUAD << 4) | 0x0Bu))  /* 0xAB */
#define VCR_DEFAULT        0xFBu

/* WIP-wait ceilings (datasheet max + margin). */
#define TIMEOUT_PAGE_MS       20u      /* page program max 5 ms      */
#define TIMEOUT_SUBSECTOR_MS  2000u    /* subsector erase max 0.8 s  */
#define TIMEOUT_SECTOR_MS     5000u    /* sector erase max 3 s       */
#define TIMEOUT_CHIP_MS       300000u  /* bulk erase max 250 s       */

static QSPI_HandleTypeDef hqspi;
static TX_MUTEX qspi_lock;
static int qspi_ready;   /* set once by qspi_flash_init() */
static int quad_mode;    /* reads use 0x6B / 4 data lines       */
static uint8_t read_dummy = DUMMY_CYCLES_READ;   /* tracks the VCR setting */

/*
 * Failure record (issue #194): every HAL_QSPI_* call that does not return
 * HAL_OK, and every receive the HAL called successful that did not leave the
 * controller idle, is captured here BEFORE any recovery -- driver step, HAL
 * return, handle state, the controller's CR / SR / CCR / DLR / AR (never DR:
 * reading it pops the FIFO) and the tick -- and then the controller is brought
 * back to idle if it can be (fail_and_recover()).  The failed operation still
 * returns its error; nothing is retried.  Written only under the operation
 * mutex (or by init, before qspi_ready).
 */
#define RECOVER_TIMEOUT_MS   100u
#define RECOVER_SPIN_CAP     20000000u   /* bound even if the tick stopped */

static struct qspi_flash_record record;

static const struct qspi_flash_info flash_info = {
	.size           = QSPI_FLASH_SIZE,
	.sector_size    = QSPI_FLASH_SECTOR_SIZE,
	.subsector_size = QSPI_FLASH_SUBSECTOR_SIZE,
	.page_size      = QSPI_FLASH_PAGE_SIZE,
	.sclk_hz        = 54000000u,
};

const struct qspi_flash_info *qspi_flash_get_info(void)
{
	return &flash_info;
}

/* ---- locking ------------------------------------------------------------ */

static int op_lock(void)
{
	if (!qspi_ready)
		return QSPI_FLASH_ERR_STATE;
	if (tx_mutex_get(&qspi_lock, TX_WAIT_FOREVER) != TX_SUCCESS)
		return QSPI_FLASH_ERR_STATE;
	return 0;
}

static void op_unlock(void)
{
	tx_mutex_put(&qspi_lock);
}

/* ---- idle check, failure capture and recovery --------------------------- */

/* qspi_idle.h restates the bits so a host test can compile it; hold them to
 * the device header's. */
_Static_assert(QSPI_IDLE_CR_ABORT == QUADSPI_CR_ABORT, "CR.ABORT");
_Static_assert(QSPI_IDLE_SR_BUSY == QUADSPI_SR_BUSY, "SR.BUSY");
_Static_assert(QSPI_IDLE_CCR_ADMODE == QUADSPI_CCR_ADMODE, "CCR.ADMODE");
_Static_assert(QSPI_IDLE_CCR_DMODE == QUADSPI_CCR_DMODE, "CCR.DMODE");
_Static_assert(QSPI_IDLE_CCR_FMODE == QUADSPI_CCR_FMODE, "CCR.FMODE");

/* Controller registers without side effects (DR is deliberately not read).
 * Through QUADSPI directly: valid before init too (they read as reset values
 * or zero while the clock is gated). */
static void regs_read(struct qspi_flash_regs *r)
{
	r->cr  = QUADSPI->CR;
	r->sr  = QUADSPI->SR;
	r->ccr = QUADSPI->CCR;
	r->dlr = QUADSPI->DLR;
	r->ar  = QUADSPI->AR;
}

/*
 * Apply qspi_ccr_step() to the CCR, then read CR / SR / CCR back and judge
 * them with qspi_idle_ok().  The only write is the FMODE restore, and only
 * for a form with a data phase (qspi_idle.h says why that starts nothing).
 * Returns nonzero when the controller reads idle.  Caller holds the mutex.
 */
static int put_idle(void)
{
	uint32_t ccr = QUADSPI->CCR;

	if (qspi_ccr_step(QUADSPI->SR, ccr) == QSPI_CCR_RESTORE)
		QUADSPI->CCR = ccr & ~QUADSPI_CCR_FMODE;
	return qspi_idle_ok(QUADSPI->CR, QUADSPI->SR, QUADSPI->CCR);
}

/*
 * Bring the controller back to idle after a failure.
 *
 * HAL_QSPI_Abort() cannot be used: it acts only when State has bit 1 set (the
 * BUSY states) and does nothing in HAL_QSPI_STATE_ERROR, which is the state a
 * failed Command leaves behind.  So, by hand, the steps it takes:
 *   - if SR.BUSY is set, request CR.ABORT, then wait (bounded,
 *     RECOVER_TIMEOUT_MS) for ABORT to self-clear and BUSY to drop (RM0385
 *     14.3.14: both reset once the abort is complete);
 *   - clear TEF / TCF / SMF / TOF through FCR;
 *   - put_idle(): the FMODE restore under the qspi_idle.h rule, then the
 *     read-back of ABORT, BUSY and FMODE.
 * Only when that read-back is idle does the handle go back to READY.
 * Otherwise the handle is set to ERROR explicitly -- HAL_QSPI_Receive() puts
 * State back to READY on its way out even after its own timeout, which would
 * let the next call run against a busy controller.  In ERROR the HAL refuses
 * at once, that refusal comes back through fail_and_recover(), and so the
 * next driver call runs this bounded recovery again.
 * Returns 0 when the controller reads back idle.  Caller holds the mutex.
 */
static int recover_locked(void)
{
	uint32_t t0, spins = 0;

	if (QUADSPI->SR & QUADSPI_SR_BUSY)
		SET_BIT(QUADSPI->CR, QUADSPI_CR_ABORT);
	t0 = HAL_GetTick();
	while ((QUADSPI->CR & QUADSPI_CR_ABORT) ||
	       (QUADSPI->SR & QUADSPI_SR_BUSY)) {
		if ((HAL_GetTick() - t0) > RECOVER_TIMEOUT_MS ||
		    ++spins > RECOVER_SPIN_CAP)
			break;
	}
	WRITE_REG(QUADSPI->FCR, QUADSPI_FCR_CTEF | QUADSPI_FCR_CTCF |
	                        QUADSPI_FCR_CSMF | QUADSPI_FCR_CTOF);
	if (!put_idle()) {
		hqspi.Lock  = HAL_UNLOCKED;
		hqspi.State = HAL_QSPI_STATE_ERROR;
		return -1;
	}
	hqspi.ErrorCode = HAL_QSPI_ERROR_NONE;
	hqspi.Lock      = HAL_UNLOCKED;
	hqspi.State     = HAL_QSPI_STATE_READY;
	return 0;
}

/*
 * Record one failure: capture the evidence first, log it, then recover and
 * log whether that worked.  Returns QSPI_FLASH_ERR_HAL.  @p st is the HAL's
 * return (HAL_OK for a receive that did not leave the controller idle);
 * @p op / @p phase are string literals.
 */
static int fail_and_recover(HAL_StatusTypeDef st, const char *op,
                            const char *phase)
{
	struct qspi_flash_fail f;

	regs_read(&f.regs);
	f.tick   = HAL_GetTick();
	f.op     = op;
	f.phase  = phase;
	f.hal_rc = (int)st;
	f.state  = (uint32_t)hqspi.State;
	f.error  = hqspi.ErrorCode;

	record.fail_count++;
	LOG_ERR("fail #%lu %s-%s hal %d state 0x%02lx err 0x%lx t %lu",
	        (unsigned long)record.fail_count, op, phase, f.hal_rc,
	        (unsigned long)f.state, (unsigned long)f.error,
	        (unsigned long)f.tick);
	LOG_ERR("CR %08lx SR %08lx CCR %08lx DLR %08lx AR %08lx",
	        (unsigned long)f.regs.cr, (unsigned long)f.regs.sr,
	        (unsigned long)f.regs.ccr, (unsigned long)f.regs.dlr,
	        (unsigned long)f.regs.ar);

	f.recovered = (recover_locked() == 0);
	regs_read(&f.after);
	f.state_after = (uint32_t)hqspi.State;
	if (f.recovered)
		LOG_WRN("recovered: SR %08lx CCR %08lx state 0x%02lx",
		        (unsigned long)f.after.sr, (unsigned long)f.after.ccr,
		        (unsigned long)f.state_after);
	else
		LOG_ERR("recovery failed: CR %08lx SR %08lx CCR %08lx, handle "
		        "set to error", (unsigned long)f.after.cr,
		        (unsigned long)f.after.sr, (unsigned long)f.after.ccr);

	if (record.fail_count == 1u)
		record.first = f;
	record.last = f;
	return QSPI_FLASH_ERR_HAL;
}

/* One HAL return: 0 for HAL_OK, otherwise the failure path. */
static int hal_check(HAL_StatusTypeDef st, const char *op, const char *phase)
{
	if (st == HAL_OK)
		return 0;
	return fail_and_recover(st, op, phase);
}

/*
 * One receive (HAL_QSPI_Receive()) and what must hold after it: the HAL
 * leaves CCR.FMODE at indirect read when the transfer ended with BUSY clear,
 * so put_idle() puts it back (one CCR write, one read-back; no wait).  A
 * receive the HAL called successful that does not then read idle -- BUSY
 * still set, an abort pending, or FMODE not back at 00 -- is a failure
 * (phase "rxend") and goes through the same recovery.
 */
static int rx_check(HAL_StatusTypeDef st, const char *op)
{
	if (st != HAL_OK)
		return fail_and_recover(st, op, "rx");
	if (put_idle())
		return 0;
	return fail_and_recover(st, op, "rxend");
}

/* The driver-step name of an instruction, for the capture. */
static const char *op_name(uint8_t instruction)
{
	switch (instruction) {
	case 0x9Fu: return "rdid";
	case 0x0Bu: return "read";
	case 0x6Bu: return "qread";
	case 0x06u: return "wren";
	case 0x02u: return "prog";
	case 0x20u: return "erase4k";
	case 0xD8u: return "erase64k";
	case 0xC7u: return "erasechip";
	case 0x05u: return "rdsr";
	case 0x70u: return "rdfsr";
	case 0x50u: return "clfsr";
	case 0x85u: return "rdvcr";
	case 0x81u: return "wrvcr";
	default:    return "op";
	}
}

/* ---- 1-line command helpers (all run under the operation mutex) --------- */

/* Command descriptor template: 1-line instruction, optional 24-bit 1-line
 * address, optional 1-line data, SDR.  Fields not set here stay zero. */
static void cmd_init(QSPI_CommandTypeDef *c, uint8_t instruction)
{
	c->Instruction       = instruction;
	c->InstructionMode   = QSPI_INSTRUCTION_1_LINE;
	c->Address           = 0;
	c->AddressMode       = QSPI_ADDRESS_NONE;
	c->AddressSize       = QSPI_ADDRESS_24_BITS;
	c->AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
	c->AlternateBytes    = 0;
	c->AlternateBytesSize = 0;
	c->DataMode          = QSPI_DATA_NONE;
	c->NbData            = 0;
	c->DummyCycles       = 0;
	c->DdrMode           = QSPI_DDR_MODE_DISABLE;
	c->DdrHoldHalfCycle  = QSPI_DDR_HHC_ANALOG_DELAY;
	c->SIOOMode          = QSPI_SIOO_INST_EVERY_CMD;
}

/* Instruction-only command (WREN, CLEAR FSR, chip erase). */
static int send_simple(uint8_t instruction)
{
	QSPI_CommandTypeDef c;

	cmd_init(&c, instruction);
	return hal_check(HAL_QSPI_Command(&hqspi, &c, HAL_QSPI_TIMEOUT_DEFAULT_VALUE),
	                 op_name(instruction), "cmd");
}

/* Register read (RDSR/RDFSR/RDID): instruction then `len` data bytes. */
static int read_reg(uint8_t instruction, uint8_t *buf, uint32_t len)
{
	QSPI_CommandTypeDef c;

	cmd_init(&c, instruction);
	c.DataMode = QSPI_DATA_1_LINE;
	c.NbData   = len;
	if (hal_check(HAL_QSPI_Command(&hqspi, &c, HAL_QSPI_TIMEOUT_DEFAULT_VALUE),
	              op_name(instruction), "cmd") != 0)
		return QSPI_FLASH_ERR_HAL;
	return rx_check(HAL_QSPI_Receive(&hqspi, buf, HAL_QSPI_TIMEOUT_DEFAULT_VALUE),
	                op_name(instruction));
}

/*
 * Wait until WIP clears.  @p sleep selects the wait style: nonzero yields with
 * tx_thread_sleep(1) between status reads (erase: hundreds of ms .. minutes),
 * zero spins (page program: sub-ms).  @p timeout_ms bounds the wait either way.
 */
static int wait_ready(uint32_t timeout_ms, int sleep)
{
	uint32_t start = HAL_GetTick();
	uint8_t sr;
	int rc;

	for (;;) {
		rc = read_reg(CMD_READ_SR, &sr, 1);
		if (rc != 0)
			return rc;
		if ((sr & SR_WIP) == 0)
			return 0;
		if ((HAL_GetTick() - start) >= timeout_ms)
			return QSPI_FLASH_ERR_TIMEOUT;
		if (sleep)
			tx_thread_sleep(1);
	}
}

/*
 * Post-program/erase check: read the flag status register and fail (after
 * clearing the sticky bits) if the device latched a program/erase/protection
 * error.  WIP alone cannot distinguish "done" from "failed".
 */
static int check_flag_status(void)
{
	uint8_t fsr;
	int rc;

	rc = read_reg(CMD_READ_FSR, &fsr, 1);
	if (rc != 0)
		return rc;
	if (fsr & FSR_ANY_ERR) {
		LOG_ERR("flag status error 0x%02x", fsr);
		(void)send_simple(CMD_CLEAR_FSR);
		return QSPI_FLASH_ERR_FLASH;
	}
	return 0;
}

static int write_enable(void)
{
	uint8_t sr;
	int rc;

	rc = send_simple(CMD_WRITE_ENABLE);
	if (rc != 0)
		return rc;
	/* Confirm WEL latched (catches a write-protected/absent device early). */
	rc = read_reg(CMD_READ_SR, &sr, 1);
	if (rc != 0)
		return rc;
	return (sr & 0x02u) ? 0 : QSPI_FLASH_ERR_FLASH;
}

/* Verified register write (1 data byte): WREN (WEL checked) -> instruction +
 * byte -> WIP wait -> read @p readback_cmd and require @p value came back. */
static int write_reg_verified(uint8_t instruction, uint8_t readback_cmd,
                              uint8_t value)
{
	QSPI_CommandTypeDef c;
	uint8_t check = 0;
	int rc;

	rc = write_enable();
	if (rc != 0)
		return rc;
	cmd_init(&c, instruction);
	c.DataMode = QSPI_DATA_1_LINE;
	c.NbData   = 1;
	if (hal_check(HAL_QSPI_Command(&hqspi, &c, HAL_QSPI_TIMEOUT_DEFAULT_VALUE),
	              op_name(instruction), "cmd") != 0)
		return QSPI_FLASH_ERR_HAL;
	if (hal_check(HAL_QSPI_Transmit(&hqspi, &value, HAL_QSPI_TIMEOUT_DEFAULT_VALUE),
	              op_name(instruction), "tx") != 0)
		return QSPI_FLASH_ERR_HAL;
	rc = wait_ready(TIMEOUT_PAGE_MS, 0);
	if (rc != 0)
		return rc;
	rc = read_reg(readback_cmd, &check, 1);
	if (rc != 0)
		return rc;
	return (check == value) ? 0 : QSPI_FLASH_ERR_FLASH;
}

/*
 * Move the volatile configuration register to dummy = 10 so FAST READ QUAD
 * OUTPUT (0x6B) is usable, verified by read-back.  The VCR is volatile: a
 * power cycle restores the default, a warm reset (SYSRESETREQ) does not reset
 * the flash, so this runs idempotently every boot.  If the quad value cannot
 * be verified, the default is written back -- also verified -- and reads stay
 * on the 1-line / 8-dummy path.  Returns nonzero only when the VCR cannot be
 * brought into either known state (driver/flash dummy timing would diverge);
 * init then fails rather than publish a driver with unknown read timing.
 * Called from init before qspi_ready is published (no locking needed).
 */
static int quad_setup(void)
{
	if (write_reg_verified(CMD_WRITE_VCR, CMD_READ_VCR, VCR_QUAD_TARGET) == 0) {
		quad_mode  = 1;
		read_dummy = DUMMY_CYCLES_QUAD;
		return 0;
	}

	LOG_WRN("VCR quad setup failed; falling back to 1-line read");
	if (write_reg_verified(CMD_WRITE_VCR, CMD_READ_VCR, VCR_DEFAULT) == 0) {
		quad_mode  = 0;
		read_dummy = DUMMY_CYCLES_READ;
		return 0;
	}

	LOG_ERR("VCR in unknown state; QSPI disabled");
	return QSPI_FLASH_ERR_FLASH;
}

/* Common erase path: WREN -> erase cmd -> WIP wait (yielding) -> FSR check. */
static int erase_common(uint8_t instruction, int has_addr, uint32_t addr,
                        uint32_t timeout_ms)
{
	QSPI_CommandTypeDef c;
	int rc;

	rc = op_lock();
	if (rc != 0)
		return rc;

	rc = write_enable();
	if (rc == 0) {
		cmd_init(&c, instruction);
		if (has_addr) {
			c.Address     = addr;
			c.AddressMode = QSPI_ADDRESS_1_LINE;
		}
		rc = hal_check(HAL_QSPI_Command(&hqspi, &c, HAL_QSPI_TIMEOUT_DEFAULT_VALUE),
		               op_name(instruction), "cmd");
	}
	if (rc == 0)
		rc = wait_ready(timeout_ms, 1);
	if (rc == 0)
		rc = check_flag_status();

	op_unlock();
	return rc;
}

/* ---- public API ---------------------------------------------------------- */

int qspi_flash_init(void)
{
	GPIO_InitTypeDef g = {0};

	if (qspi_ready)
		return 0;

	if (tx_mutex_create(&qspi_lock, "qspi", TX_INHERIT) != TX_SUCCESS)
		return QSPI_FLASH_ERR_STATE;

	__HAL_RCC_GPIOB_CLK_ENABLE();
	__HAL_RCC_GPIOD_CLK_ENABLE();
	__HAL_RCC_GPIOE_CLK_ENABLE();
	__HAL_RCC_QSPI_CLK_ENABLE();

	/* PB2 = QUADSPI_CLK (AF9).  Also the BOOT1 strap -- sampled only while in
	 * reset, so run-time clocking is harmless (UM1907). */
	g.Mode      = GPIO_MODE_AF_PP;
	g.Pull      = GPIO_NOPULL;
	g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
	g.Alternate = GPIO_AF9_QUADSPI;
	g.Pin       = GPIO_PIN_2;
	HAL_GPIO_Init(GPIOB, &g);

	/* PB6 = QUADSPI_BK1_NCS (AF10), pulled up so the flash deselects whenever
	 * the controller releases the line. */
	g.Alternate = GPIO_AF10_QUADSPI;
	g.Pull      = GPIO_PULLUP;
	g.Pin       = GPIO_PIN_6;
	HAL_GPIO_Init(GPIOB, &g);

	/* PD11 = IO0, PD12 = IO1, PD13 = IO3 (AF9). */
	g.Alternate = GPIO_AF9_QUADSPI;
	g.Pull      = GPIO_NOPULL;
	g.Pin       = GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13;
	HAL_GPIO_Init(GPIOD, &g);

	/* PE2 = IO2 (AF9). */
	g.Pin = GPIO_PIN_2;
	HAL_GPIO_Init(GPIOE, &g);

	hqspi.Instance                = QUADSPI;
	hqspi.Init.ClockPrescaler     = 3;   /* 216 MHz / (3+1) = 54 MHz */
	hqspi.Init.FifoThreshold      = 4;
	hqspi.Init.SampleShifting     = QSPI_SAMPLE_SHIFTING_HALFCYCLE;
	hqspi.Init.FlashSize          = 23;  /* 2^(23+1) = 16 MB */
	hqspi.Init.ChipSelectHighTime = QSPI_CS_HIGH_TIME_6_CYCLE;  /* >=50 ns */
	hqspi.Init.ClockMode          = QSPI_CLOCK_MODE_0;
	hqspi.Init.FlashID            = QSPI_FLASH_ID_1;
	hqspi.Init.DualFlash          = QSPI_DUALFLASH_DISABLE;
	if (HAL_QSPI_Init(&hqspi) != HAL_OK) {
		tx_mutex_delete(&qspi_lock);
		LOG_ERR("QUADSPI init failed");
		return QSPI_FLASH_ERR_HAL;
	}

	/* Quad-read bring-up (owhinata/stm32f746g-disco#31) -- register traffic only, still
    before
	 * qspi_ready so no other caller can interleave.  Fails the whole init if
	 * the VCR cannot reach a known state (read timing would be undefined). */
	if (quad_setup() != 0) {
		HAL_QSPI_DeInit(&hqspi);
		tx_mutex_delete(&qspi_lock);
		return QSPI_FLASH_ERR_FLASH;
	}

	qspi_ready = 1;
	LOG_INF("QUADSPI up: 54 MHz, %s read, indirect mode",
	        quad_mode ? "quad (0x6B)" : "1-line (0x0B)");
	return 0;
}

int qspi_flash_quad_enabled(void)
{
	return quad_mode;
}

int qspi_flash_ready(void)
{
	return qspi_ready;
}

int qspi_flash_read_id(uint8_t id[3])
{
	int rc;

	rc = op_lock();
	if (rc != 0)
		return rc;
	rc = read_reg(CMD_READ_ID, id, 3);
	op_unlock();
	return rc;
}

int qspi_flash_read_status(uint8_t *sr)
{
	int rc;

	rc = op_lock();
	if (rc != 0)
		return rc;
	rc = read_reg(CMD_READ_SR, sr, 1);
	op_unlock();
	return rc;
}

int qspi_flash_read(uint32_t addr, void *buf, uint32_t len)
{
	QSPI_CommandTypeDef c;
	int rc;

	if (len == 0)
		return 0;
	if (addr >= QSPI_FLASH_SIZE || len > QSPI_FLASH_SIZE - addr)
		return QSPI_FLASH_ERR_PARAM;

	rc = op_lock();
	if (rc != 0)
		return rc;

	cmd_init(&c, quad_mode ? CMD_QUAD_READ : CMD_FAST_READ);
	c.Address     = addr;
	c.AddressMode = QSPI_ADDRESS_1_LINE;
	c.DataMode    = quad_mode ? QSPI_DATA_4_LINES : QSPI_DATA_1_LINE;
	c.NbData      = len;
	c.DummyCycles = read_dummy;
	rc = hal_check(HAL_QSPI_Command(&hqspi, &c, HAL_QSPI_TIMEOUT_DEFAULT_VALUE),
	               op_name(c.Instruction), "cmd");
	if (rc == 0)
		rc = rx_check(HAL_QSPI_Receive(&hqspi, buf, HAL_QSPI_TIMEOUT_DEFAULT_VALUE),
		              op_name(c.Instruction));

	op_unlock();
	return rc;
}

int qspi_flash_write(uint32_t addr, const void *buf, uint32_t len)
{
	const uint8_t *p = buf;
	QSPI_CommandTypeDef c;
	int rc = 0;

	if (len == 0)
		return 0;
	if (addr >= QSPI_FLASH_SIZE || len > QSPI_FLASH_SIZE - addr)
		return QSPI_FLASH_ERR_PARAM;

	rc = op_lock();
	if (rc != 0)
		return rc;

	while (len > 0) {
		/* Never let one PAGE PROGRAM cross a 256 B page boundary -- the
		 * device would wrap within the page and corrupt data. */
		uint32_t chunk = QSPI_FLASH_PAGE_SIZE - (addr % QSPI_FLASH_PAGE_SIZE);

		if (chunk > len)
			chunk = len;

		rc = write_enable();
		if (rc != 0)
			break;

		cmd_init(&c, CMD_PAGE_PROGRAM);
		c.Address     = addr;
		c.AddressMode = QSPI_ADDRESS_1_LINE;
		c.DataMode    = QSPI_DATA_1_LINE;
		c.NbData      = chunk;
		rc = hal_check(HAL_QSPI_Command(&hqspi, &c, HAL_QSPI_TIMEOUT_DEFAULT_VALUE),
		               "prog", "cmd");
		if (rc != 0)
			break;
		/* HAL_QSPI_Transmit takes a non-const pointer but only reads. */
		rc = hal_check(HAL_QSPI_Transmit(&hqspi, (uint8_t *)(uintptr_t)p,
		                                 HAL_QSPI_TIMEOUT_DEFAULT_VALUE),
		               "prog", "tx");
		if (rc != 0)
			break;
		rc = wait_ready(TIMEOUT_PAGE_MS, 0);
		if (rc != 0)
			break;
		rc = check_flag_status();
		if (rc != 0)
			break;

		addr += chunk;
		p    += chunk;
		len  -= chunk;
	}

	op_unlock();
	return rc;
}

int qspi_flash_erase_sector(uint32_t addr)
{
	if (addr >= QSPI_FLASH_SIZE)
		return QSPI_FLASH_ERR_PARAM;
	return erase_common(CMD_ERASE_SECTOR, 1,
	                    addr & ~(QSPI_FLASH_SECTOR_SIZE - 1u),
	                    TIMEOUT_SECTOR_MS);
}

int qspi_flash_erase_subsector(uint32_t addr)
{
	if (addr >= QSPI_FLASH_SIZE)
		return QSPI_FLASH_ERR_PARAM;
	return erase_common(CMD_ERASE_SUB, 1,
	                    addr & ~(QSPI_FLASH_SUBSECTOR_SIZE - 1u),
	                    TIMEOUT_SUBSECTOR_MS);
}

int qspi_flash_erase_chip(void)
{
	return erase_common(CMD_ERASE_CHIP, 0, 0, TIMEOUT_CHIP_MS);
}

/* ---- read-only views for `qspi info` (issue #194) ------------------------ */

void qspi_flash_regs_now(struct qspi_flash_regs *r, uint32_t *state,
                         uint32_t *error)
{
	/* No lock, no command: register reads only, so it shows what the
	 * controller says right now even if the driver or the mutex is wedged. */
	regs_read(r);
	*state = (uint32_t)hqspi.State;
	*error = hqspi.ErrorCode;
}

void qspi_flash_record_get(struct qspi_flash_record *out)
{
	/* The record is written under the operation mutex; take it with a bound
	 * so a wedged driver still lets the operator see the record. */
	int locked = qspi_ready &&
	             tx_mutex_get(&qspi_lock, 100u) == TX_SUCCESS;

	*out = record;
	if (locked)
		tx_mutex_put(&qspi_lock);
}
