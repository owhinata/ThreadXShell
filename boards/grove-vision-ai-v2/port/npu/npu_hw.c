/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    npu_hw.c
 * @brief   Ethos-U55 bring-up (issue #44).
 *
 * WHY THIS EXISTS AT ALL
 *
 * The SEC_ONLY path this port takes does NOT bring the NPU up.  Boot calls
 * TZ_Set_ALL_Secure(), which configures the U55's APB side and clears the MSC
 * interrupt status but stops there.  The PORSL/PORPL, per-master MSC and
 * reset->normal sequence lives in TZ_Set_Secure_ByCFG(), a function this build
 * never reaches.  So the NPU is sitting in whatever state the bootloader left
 * it in, and nothing in the existing image would have told us.
 *
 * The sequence below is the donor's secure branch (trustzone_cfg.c, the
 * IP_INST_NS_u55-undefined side), with two deliberate differences.
 *
 * DIFFERENCE 1: EVERY STEP IS READ BACK.  The donor writes and moves on.  This
 * port's doctrine is that inherited state is not trusted -- the same reason the
 * clock comes from an SCU read-back rather than a compile-time constant -- and
 * there is no public TRM to check the writes against, so the read-back IS the
 * specification.  A mismatch aborts the whole bring-up and leaves the shell
 * running without inference, rather than handing TFLM an NPU that is not in the
 * state it thinks.
 *
 * DIFFERENCE 2: THE MSC INTERRUPTS STAY DISABLED, AND VIOLATIONS FAULT.  The
 * donor enables the per-master read/write violation interrupts (IRQ 193/194).
 * This port's EPK rule is that no interrupt may be enabled but unaccounted, and
 * the obvious alternative -- poll the status instead -- turns out not to exist:
 * the SDK's SCU surface has irq_clear entry points and configuration getters,
 * but nothing that reports whether a master was filtered.
 *
 * So the filters are configured for SCU_MSC_RESP_ERR rather than the RAZ/WI
 * default.  A blocked NPU access then raises a bus error instead of reading as
 * zeros, and this port already turns that into a recorded fault.  That is the
 * better failure anyway: RAZ/WI would hand TFLM a model or an arena full of
 * zeros, which it would faithfully interpret as data.
 *
 * INTERRUPT ACCOUNTING.  The one line that IS enabled -- U55 at 192 -- goes
 * through the same transactional wrap the camera bring-up uses: snapshot,
 * bring up, wrap everything newly enabled, and on any failure roll the whole
 * set back.  The vendor's ethosu_init() may enable lines of its own; the
 * snapshot catches those without anyone having to know their numbers, which is
 * the point of doing it this way rather than naming 192 and hoping.
 */
#include "npu_hw.h"

#include "nor_flash.h"

#include "WE2_device.h"
#include "WE2_core.h"       /* EPII_NVIC_SetVector */
#include "hx_drv_scu.h"
#include "ethosu_driver.h"

#define LOG_TAG "npu"
#include "log.h"
#include "epk_irq_wrap.h"

/* Secure alias of the U55 control block.  U55_BASE_REG resolves to this under
 * TRUSTZONE_SEC_ONLY, but it is spelled out so a change in the SDK's macro
 * maze cannot silently point the driver somewhere else. */
#define NPU_BASE_SECURE 0x53020000u

static struct ethosu_driver npu_drv;
static struct epk_irq_wrapset npu_irqs;
static const char *fail_reason;
static uint8_t     hw_ready;
/* The committed flash lease: non-zero exactly while hw_ready is (issue #86). */
static uint32_t    nor_lease;

static int fail(const char *why)
{
	fail_reason = why;
	LOG_ERR("bring-up refused: %s", why);
	return -1;
}

/*
 * The SCU half of the sequence, each write followed by its getter.
 *
 * PORSL secure + PORPL privileged is the donor's secure-world configuration:
 * the NPU issues its AXI transactions as a secure, privileged master, which is
 * what lets it read the model out of the flash alias and the arena out of SRAM
 * in a build where the whole address space is secure.
 */
static int npu_scu_bring_up(void)
{
	SCU_U55_AXI_MSC_CFG_T msc;
	SCU_U55_PORSL_E porsl;
	SCU_U55_PORPL_E porpl;
	SCU_U55_STATE_E state;

	/* Both AXI masters: secure, and errors reported as ERROR responses rather
	 * than silently reading zero -- a filtered read that returns 0 would look
	 * to TFLM like a model full of zeros.  Interrupts off, see DIFFERENCE 2. */
	msc.state         = SCU_MSC_MASTER_SECURE;
	msc.resp          = SCU_MSC_RESP_ERR;
	msc.irq_rd_enable = 0u;
	msc.irq_wr_enable = 0u;
	msc.callback      = NULL;
	if (hx_drv_scu_set_u55_m0_msc_cfg(msc) != SCU_NO_ERROR)
		return fail("U55 M0 MSC config rejected");
	if (hx_drv_scu_set_u55_m1_msc_cfg(msc) != SCU_NO_ERROR)
		return fail("U55 M1 MSC config rejected");

	if (hx_drv_scu_set_U55_PORSL(SCU_U55_PORSL_SECURE) != SCU_NO_ERROR)
		return fail("U55 PORSL write rejected");
	if (hx_drv_scu_get_U55_PORSL(&porsl) != SCU_NO_ERROR ||
	    porsl != SCU_U55_PORSL_SECURE)
		return fail("U55 PORSL did not read back secure");

	if (hx_drv_scu_set_U55_PORPL(SCU_U55_PORPL_PRIVILEGED) != SCU_NO_ERROR)
		return fail("U55 PORPL write rejected");
	if (hx_drv_scu_get_U55_PORPL(&porpl) != SCU_NO_ERROR ||
	    porpl != SCU_U55_PORPL_PRIVILEGED)
		return fail("U55 PORPL did not read back privileged");

	/* Reset then normal, in that order: PORSL/PORPL are sampled out of reset,
	 * so setting them after the release would not take. */
	if (hx_drv_scu_set_u55_state(SCU_U55_STATE_RESET) != SCU_NO_ERROR)
		return fail("U55 reset assert rejected");
	if (hx_drv_scu_set_u55_state(SCU_U55_STATE_NORMAL) != SCU_NO_ERROR)
		return fail("U55 reset release rejected");
	if (hx_drv_scu_get_u55_state(&state) != SCU_NO_ERROR ||
	    state != SCU_U55_STATE_NORMAL)
		return fail("U55 did not read back out of reset");

	return 0;
}

/* The driver's IRQ trampoline.  Installed into the ITCM vector table at run
 * time, the same way the prebuilt peripheral drivers install theirs. */
static void npu_irq_handler(void)
{
	ethosu_irq_handler(&npu_drv);
}

int npu_hw_init(void)
{
	struct epk_irq_snapshot snap;
	uint32_t lease = 0u;

	if (hw_ready)
		return 0;               /* already ready: acquire nothing */
	fail_reason = NULL;

	/* [!] THE FLASH LEASE COMES FIRST, AND OUTSIDE THIS SNAPSHOT (issue #86).
	 * The model lives in flash and is parsed in place, so the read window has
	 * to exist before anything downstream can look at it -- but bringing that
	 * window up is what enables the QSPI DMA interrupt, and that line belongs
	 * to port/nor/.  It used to land in npu_irqs, so npu_hw_deinit() disabled
	 * it on every `nn close` while the old one-way latch went on reporting the
	 * flash as initialised.
	 *
	 * [!] The token stays LOCAL until this whole transaction succeeds.  Three
	 * things below can still fail, and cmd_nn.c does NOT call npu_hw_deinit()
	 * when npu_hw_init() returns non-zero -- so each of those returns has to
	 * hand the lease back itself. */
	if (nor_acquire(NOR_LEASE_NPU, &lease) != 0)
		return fail(nor_fail_reason() != NULL
		            ? nor_fail_reason()
		            : "the flash window is unavailable; the model is unreadable");

	/* Everything the NPU bring-up enables must end up wrapped; take the
	 * reference point after the flash, so this set is only the NPU's own. */
	grove_epk_irq_snapshot(&snap);

	if (npu_scu_bring_up() != 0) {
		(void)nor_release(lease);
		return -1;
	}

	/* Clear any violation the bootloader or a previous run left latched, so a
	 * fault during inference can only be ours. */
	(void)hx_drv_scu_set_u55_m0_msc_irq_clear();
	(void)hx_drv_scu_set_u55_m1_msc_irq_clear();

	EPII_NVIC_SetVector((IRQn_Type)U55_IRQn, (uint32_t)npu_irq_handler);
	NVIC_EnableIRQ((IRQn_Type)U55_IRQn);

	if (ethosu_init(&npu_drv, (void *)NPU_BASE_SECURE,
	                NULL,   /* no fast-memory area: U55 has none here */
	                0u,
	                1,      /* security_enable  */
	                1) != 0) {   /* privilege_enable */
		NVIC_DisableIRQ((IRQn_Type)U55_IRQn);
		(void)nor_release(lease);
		(void)fail("ethosu_init failed");
		return -1;
	}

	/* Wrap the U55 line and anything ethosu_init() turned on behind our back.
	 * On failure the set is rolled back, which also disables what it enabled --
	 * so a refused bring-up cannot leave an unaccounted interrupt live. */
	if (!grove_epk_irq_wrap_new(&snap, &npu_irqs)) {
		grove_epk_irq_unwrap_set(&npu_irqs);
		ethosu_deinit(&npu_drv);
		NVIC_DisableIRQ((IRQn_Type)U55_IRQn);
		(void)nor_release(lease);
		(void)fail("could not account the NPU interrupts (EPK wrap failed)");
		return -1;
	}

	/* Committed only here, alongside hw_ready -- the two are what
	 * npu_hw_deinit() keys off, and neither may be true without the other. */
	nor_lease = lease;
	hw_ready = 1u;
	LOG_INF("Ethos-U55 up (secure, privileged), IRQ %d wrapped",
	        (int)U55_IRQn);
	return 0;
}

void npu_hw_deinit(void)
{
	uint32_t lease;

	if (!hw_ready)
		return;                 /* nothing committed: a no-op, as nn close needs */
	hw_ready = 0u;

	/* Cleared before it is released, so a second call through any path finds
	 * nothing to hand back rather than handing the same claim back twice. */
	lease = nor_lease;
	nor_lease = 0u;

	ethosu_deinit(&npu_drv);
	NVIC_DisableIRQ((IRQn_Type)U55_IRQn);
	grove_epk_irq_unwrap_set(&npu_irqs);

	/* [!] The NOR lease goes back LAST, and only the NPU's own wrapset was
	 * unwound above.  IRQ 133 is port/nor/'s and stays wrapped and enabled --
	 * that difference is the whole of issue #86. */
	(void)nor_release(lease);

	/* Back into reset: an idle NPU that still has bus mastership is a thing
	 * nobody is watching. */
	(void)hx_drv_scu_set_u55_state(SCU_U55_STATE_RESET);
}

const char *npu_hw_fail_reason(void)
{
	return fail_reason;
}

int npu_hw_ready(void)
{
	return hw_ready ? 1 : 0;
}

unsigned npu_hw_wrapped_irqs(int *out, unsigned max)
{
	unsigned n;

	if (!hw_ready || out == NULL)
		return 0u;
	n = npu_irqs.count < max ? npu_irqs.count : max;
	for (unsigned i = 0; i < n; i++)
		out[i] = npu_irqs.irqn[i];
	return n;
}
