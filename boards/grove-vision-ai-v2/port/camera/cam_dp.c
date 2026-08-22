/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/*
 * HX6538 DATAPATH glue (issues #35, #36): MIPI receiver, INP crop/bin/subsample,
 * HW5x5 demosaic and WDMA3.
 *
 * Everything here is about the chip that RECEIVES and REDUCES what the sensor
 * sends; cam_sensor.h is the other side of that seam.  What the datapath needs
 * to know about the part -- its geometry, its link rate, its Bayer phase -- it
 * reads from the descriptor rather than from a register.
 *
 * Source: sdk/EPII_CM55M_APP_S/app/scenario_app/tflm_yolov8_od/cis_sensor/
 * cis_imx219/{cisdp_sensor.c,cisdp_cfg.h}, at the SHA cmake/himax_sdk.cmake
 * pins.  The call ORDER below is the donor's and should be treated as vendor
 * knowledge -- there is no public TRM for this part.
 *
 * THE FIXED CONFIGURATION.  640x480 RAW10 over 2 MIPI lanes, the sensor having
 * binned on chip -> no INP crop -> 4:2 binning -> 320x240 -> HW5x5 demosaic
 * (BGGR) -> WDMA3.  That is the donor's shipping OV5647 configuration rather
 * than one invented here.
 *
 * THREADING.  Thread context only, except cam_dp_arm_next(), which the
 * producer calls per frame.
 */
#include <stddef.h>
#include <stdint.h>

#include "hx_drv_csirx.h"
#include "hx_drv_edm.h"
#include "hx_drv_hw5x5.h"
#include "hx_drv_inp.h"
#include "hx_drv_scu.h"
#include "hx_drv_scu_export.h"
#include "hx_drv_swreg_aon.h"
#include "driver_interface.h"
#include "sensor_dp_lib.h"

#include "cam_dp.h"
#include "cam_sensor.h"
#include "cam_mipi_calc.h"
#include "cam_wdma3.h"
#include "timebase.h"

#define LOG_TAG "camdp"
#include "log.h"

/* The link is two lanes whatever is plugged into the connector. */
#define CAM_MIPI_LANES      2u

/*
 * The WDMA3 landing buffers -- TWO of one frame each since issue #59, so the
 * capture of frame N+1 can overlap the CPU's work on frame N.  Which one is
 * armed and which one is readable is cam_wdma3.c's state machine; this file
 * only owns the memory.
 *
 * In SRAM and NOT in TCM, for the same reason the LCD frame buffer is: the DMA
 * engines on this part cannot see TCM, and a transfer to a TCM address does not
 * fault -- it simply never arrives.  32-byte aligned because the CPU has to
 * invalidate one buffer a cache line at a time before reading each frame, and
 * an invalidate that starts mid-line would either miss bytes or discard a
 * neighbour's.  One symbol rather than two, so the placement gate keeps pinning
 * symbol -> size -> section -> region with only its expected size changed
 * (cmake/check_placement_budget.py); the donor's `mm_reserve` bump allocator
 * puts its buffer wherever the heap happened to be, which no gate can check.
 */
static uint8_t cam_raw_buf[CAM_WDMA3_BUFFERS][CAM_RAW_BYTES]
	__attribute__((section(".cam_raw"), aligned(32)));

_Static_assert(sizeof cam_raw_buf == 460800u,
               "the WDMA3 arena is not two 320x240 planar BGR frames");
/*
 * The row stride is CAM_RAW_BYTES, so this is also what keeps the two buffers
 * from sharing a cache line: while the DMA writes one buffer the CPU
 * invalidates the other, and a shared line would let that invalidate discard
 * bytes the DMA just delivered next door.
 */
_Static_assert((CAM_RAW_BYTES % 32u) == 0u,
               "a landing buffer is not a whole number of cache lines");

const uint8_t *cam_dp_completed_buffer(void)
{
	return cam_raw_buf[cam_wdma3_read_index()];
}

/*
 * The demosaic's Bayer phase.  The descriptor's value is the DEFAULT, not a
 * certainty -- see the note on cam_dp_set_bayer() for why, and for what a
 * wrong one looks like.
 */
static uint8_t cam_bayer = (uint8_t)DEMOS_PATTENMODE_BGGR;
/* Set once the console has chosen a phase, so that a bring-up after a fault
 * does not quietly put the sensor default back and undo a bench measurement. */
static uint8_t cam_bayer_user;

int cam_dp_set_bayer(uint8_t pattern)
{
	if (pattern > (uint8_t)DEMOS_PATTENMODE_RGGB)
		return -1;
	/*
	 * Both fields together, and the caller holds the camera API mutex
	 * (issue #80).  cam_bayer_user is read by cam_dp_seed_bayer(), which
	 * bring-up calls -- and bring-up runs under that same mutex, so the two
	 * cannot interleave and a chosen phase cannot be silently replaced by the
	 * fitted part's default half way through being recorded.
	 */
	cam_bayer = pattern;
	cam_bayer_user = 1u;
	return 0;
}

uint8_t cam_dp_bayer(void)
{
	return cam_bayer;
}

const char *cam_dp_bayer_name(uint8_t pattern)
{
	switch (pattern) {
	case DEMOS_PATTENMODE_BGGR: return "bggr";
	case DEMOS_PATTENMODE_GBRG: return "gbrg";
	case DEMOS_PATTENMODE_GRBG: return "grbg";
	case DEMOS_PATTENMODE_RGGB: return "rggb";
	default:                    return "?";
	}
}

/*
 * Adopt the fitted part's default phase, unless the console has already chosen
 * one -- a bench measurement must not be undone by a bring-up after a fault.
 */
void cam_dp_seed_bayer(uint8_t pattern)
{
	if (!cam_bayer_user && pattern <= (uint8_t)DEMOS_PATTENMODE_RGGB)
		cam_bayer = pattern;
}

/* ---- MIPI receiver ------------------------------------------------------- */

/*
 * Point the MIPI clock at the PLL.  This is a DP-domain clock mux and divider,
 * NOT the system or CPU clock tree: the board rule is that the app inherits
 * what the bootloader configured, and this does not touch any of it.
 */
static uint32_t cam_dp_select_mipi_clock(void)
{
	SCU_PDHSC_DPCLK_CFG_T cfg;
	uint32_t pllfreq = 0u;
	uint32_t mipi_pixel_clk = 0u;

	if (hx_drv_scu_get_pdhsc_dpclk_cfg(&cfg) != SCU_NO_ERROR) {
		LOG_ERR("dp clock config read failed");
		return 0u;
	}

	hx_drv_swreg_aon_get_pllfreq(&pllfreq);

	cfg.mipiclk.hscmipiclksrc = SCU_HSCMIPICLKSRC_PLL;
	cfg.mipiclk.hscmipiclkdiv = (pllfreq == 400000000u) ? 1u : 0u;

	if (hx_drv_scu_set_pdhsc_dpclk_cfg(cfg, 0, 1) != SCU_NO_ERROR) {
		LOG_ERR("dp clock config write failed");
		return 0u;
	}

	/* Read the rate BACK rather than predicting it.  The SDK's own platform
	 * init drops this call's return value, so a failed read there is
	 * indistinguishable from a good one; here a failure has to be a
	 * failure, because everything downstream (the HS counts and the FIFO
	 * fill) is computed from this number. */
	if (hx_drv_scu_get_freq(SCU_CLK_FREQ_TYPE_HSC_MIPI_RXCLK,
	                        &mipi_pixel_clk) != SCU_NO_ERROR) {
		LOG_ERR("mipi rx clock read-back failed");
		return 0u;
	}
	return mipi_pixel_clk / 1000000u;
}

static void cam_dp_set_hscnt(uint32_t mipi_pixel_clk_mhz)
{
	MIPIRX_DPHYHSCNT_CFG_T hscnt;

	hscnt.mipirx_dphy_hscnt_clk_en = 0;
	hscnt.mipirx_dphy_hscnt_ln0_en = 1;
	hscnt.mipirx_dphy_hscnt_ln1_en = 1;
	hscnt.mipirx_dphy_hscnt_clk_val = 0x03;

	/* The vendor's table, unchanged.  There is no formula behind it in any
	 * document available for this part. */
	if (mipi_pixel_clk_mhz == 200u) {
		hscnt.mipirx_dphy_hscnt_ln0_val = 0x10;
		hscnt.mipirx_dphy_hscnt_ln1_val = 0x10;
	} else if (mipi_pixel_clk_mhz == 300u) {
		hscnt.mipirx_dphy_hscnt_ln0_val = 0x18;
		hscnt.mipirx_dphy_hscnt_ln1_val = 0x18;
	} else {
		hscnt.mipirx_dphy_hscnt_ln0_val = 0x06;
		hscnt.mipirx_dphy_hscnt_ln1_val = 0x06;
	}

	sensordplib_csirx_set_hscnt(hscnt);
}

int cam_dp_csirx_enable(void)
{
	const struct cam_sensor_desc *sd = cam_sensor_current();
	struct cam_mipi_link link;
	struct cam_mipi_fifo_fill fill;
	SCU_DP_SWRESET_T swrst;
	SCU_VMUTE_CFG_T vmute;
	uint32_t mipi_pixel_clk_mhz;

	mipi_pixel_clk_mhz = cam_dp_select_mipi_clock();
	if (mipi_pixel_clk_mhz == 0u)
		return -1;

	link.bitrate_1lane_mhz = sd->mipi_clock_mhz * 2u;
	link.lanes             = CAM_MIPI_LANES;
	link.pixel_dpp         = sd->dpp;
	link.line_length       = sd->sensor_w;
	link.pixel_clk_mhz     = mipi_pixel_clk_mhz;

	if (cam_mipi_fifo_fill(&link, &fill) != 0) {
		LOG_ERR("mipi fifo fill undefined for a %lu MHz pixel clock",
		        (unsigned long)mipi_pixel_clk_mhz);
		return -1;
	}

	/* Reset both PHYs.  Note this reads the CURRENT reset state and puts
	 * back exactly what it found for every other block -- the DP software
	 * reset register covers more than MIPI, and writing a whole-register
	 * constant here would reset peripherals this port does not own. */
	if (drv_interface_get_dp_swreset(&swrst) != DRIVER_INTERFACE_NO_ERROR) {
		LOG_ERR("dp software reset state read failed");
		return -1;
	}
	swrst.HSC_MIPIRX = 0;
	swrst.HSC_MIPITX = 0;
	(void)hx_drv_scu_set_DP_SWReset(swrst);
	udelay(50u);
	swrst.HSC_MIPIRX = 1;
	swrst.HSC_MIPITX = 1;
	(void)hx_drv_scu_set_DP_SWReset(swrst);

	cam_dp_set_hscnt(mipi_pixel_clk_mhz);

	sensordplib_csirx_set_pixel_depth(sd->dpp);
	sensordplib_csirx_set_deskew(0);
	sensordplib_csirx_set_fifo_fill(fill.rx);
	sensordplib_csirx_enable(CAM_MIPI_LANES);

	/*
	 * The transmitter leg.  This port sends nothing out over CSI, but the
	 * datapath's INP stage is fed through the TX block on this part and the
	 * vendor brings it up unconditionally -- dropping it is not a saving,
	 * it is a datapath that never produces a frame.
	 */
	sensordplib_csitx_set_dphy_clkmode(CSITX_DPHYCLOCK_CONT);
	sensordplib_csitx_set_pixel_depth(sd->dpp);
	sensordplib_csitx_set_deskew(0);
	sensordplib_csitx_set_fifo_fill(fill.tx);
	sensordplib_csitx_enable(CAM_MIPI_LANES,
	                         sd->mipi_clock_mhz * 2u,
	                         sd->sensor_w, sd->sensor_h);

	vmute.timingsrc = SCU_VMUTE_CTRL_TIMING_SRC_VMUTE;
	vmute.txphypwr  = SCU_VMUTE_CTRL_TXPHY_PWR_DISABLE;
	vmute.ctrlsrc   = SCU_VMUTE_CTRL_SRC_SW;
	vmute.swctrl    = SCU_VMUTE_CTRL_SW_ENABLE;
	(void)hx_drv_scu_set_vmute(&vmute);

	return 0;
}

void cam_dp_csirx_disable(void)
{
	sensordplib_csirx_disable();
}

int cam_dp_csirx_clear_errors(void)
{
	int rc = 0;

	if (hx_drv_csirx_clr_errirq_state() != CSIRX_NO_ERROR)
		rc = -1;
	if (hx_drv_csirx_clr_dphyerrirq_state() != CSIRX_NO_ERROR)
		rc = -1;
	if (rc != 0)
		LOG_ERR("csirx error status clear failed");
	return rc;
}

void cam_dp_csirx_errors(struct cam_csirx_errors *out)
{
	uint32_t err = 0u, dphy = 0u;

	if (out == NULL)
		return;

	out->readable = 1;
	if (hx_drv_csirx_get_errirq_state(&err) != CSIRX_NO_ERROR)
		out->readable = 0;
	if (hx_drv_csirx_get_dphyerrirq_state(&dphy) != CSIRX_NO_ERROR)
		out->readable = 0;

	out->err     = err;
	out->dphyerr = dphy;
}

/* ---- datapath ------------------------------------------------------------ */

/* The INP chain: crop, bin, subsample.  Shared by both output legs. */
static int cam_dp_inp_config(void)
{
	const struct cam_sensor_desc *sd = cam_sensor_current();
	INP_CROP_T crop = { 0 };

	crop.start_x = 0;
	crop.start_y = 0;
	crop.last_x  = (sd->crop_w != 0u) ? (uint16_t)(sd->crop_w - 1u) : 0u;
	crop.last_y  = (sd->crop_h != 0u) ? (uint16_t)(sd->crop_h - 1u) : 0u;

	/*
	 * Two very different reductions, which is the point of carrying both
	 * parts:
	 *   imx219  3280x2464 -> crop 3200x2400 -> 10:2 bin -> 640x480 -> 4:2
	 *           subsample -> 320x240.  One 10.25x step, box-averaged then
	 *           decimated -- soft, and it aliases.
	 *   ov5647  640x480 out of the SENSOR -> 4:2 bin -> 320x240.  The
	 *           sensor does the hard part with its own binning.
	 * Every "NTO2" here works in Bayer PAIRS, which is what keeps the
	 * mosaic phase intact across the reduction.
	 */
	if (sensordplib_set_sensorctrl_inp_wi_crop_bin(
	            SENSORDPLIB_SENSOR_HM2130, SENSORDPLIB_STREAM_NONEAOS,
	            sd->sensor_w, sd->sensor_h,
	            (INP_SUBSAMPLE_E)sd->subsample, crop,
	            (INP_BINNING_E)sd->binning) != 0) {
		LOG_ERR("inp crop/bin/subsample config failed");
		return -1;
	}
	return 0;
}

/*
 * The RAW leg: INP -> WDMA2, no demosaic.
 *
 * This exists to answer questions the demosaiced output cannot.  What lands in
 * the buffer is the Bayer MOSAIC the HW5x5 would have been fed, so the true
 * phase can be READ OFF it -- the two green positions are the highest and
 * nearly equal -- instead of inferred from the sensor's mirror setting; and the
 * 8-bit samples the MIPI receiver made of the sensor's RAW10 can be looked at
 * directly rather than argued about.
 *
 * One frame is 320x240 = 76,800 bytes, so it shares the WDMA3 buffer.
 */
int cam_dp_config_raw(void)
{
	/* The RAW leg is WDMA2 and one-shot only, so there is no layout to
	 * capture and nothing will ever flip: reset so the completed-buffer
	 * accessor answers "buffer 0", which is where WDMA2 lands. */
	cam_wdma3_reset();
	sensordplib_set_xDMA_baseaddrbyapp(0u,
	                                   (uint32_t)(uintptr_t)cam_raw_buf[0],
	                                   0u);
	if (cam_dp_inp_config() != 0)
		return -1;
	sensordplib_set_raw_wdma2(CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT, NULL);
	return 0;
}

int cam_dp_config(void)
{
	/*
	 * [!] ZERO-INITIALISED, and that is not decoration.
	 *
	 * HW5x5_CFG_T has twelve fields and the demosaic path uses eight of
	 * them; the other three (fir_procmode, firlpf_bndmode, fir_lbp_th)
	 * belong to the FIR path.  The donor leaves them unset -- i.e. passes
	 * whatever was on the stack -- and the vendor entry point is called
	 * hx_drv_hw5x5_set_allCfg(), which is not a name that suggests it skips
	 * the fields the current path does not use.
	 *
	 * Two of them are shared with the demosaic in the SDK's own words:
	 * firlpf_bndmode is "FIR *and LPF* boundary extend mode" and the
	 * neighbouring demoslpf_roundmode is "Demosaic *and LPF* rounding mode".
	 * So stack garbage here can plausibly reach the block that produces the
	 * picture.  Zeroing gives FIR_PROCMODE_LBP1 / FIRLPF_BNDODE_EXTEND0 /
	 * threshold 0 -- defined values, the same on every boot, whether or not
	 * the hardware ends up caring.
	 */
	HW5x5_CFG_T hw5x5 = { 0 };

	/*
	 * WDMA1 and WDMA2 are left at zero deliberately.  Disassembly of
	 * libsensordp.a shows the HW5x5 -> WDMA3 path (setup_dp_hw5x5_wdma3 ->
	 * setup_sensor_dp_hw5x5_xdma1) reads g_dp_wdma3_addr and nothing else;
	 * the other two are for the JPEG and raw legs this port does not build.
	 * Zero rather than a scratch buffer so that a future path change which
	 * DOES use them faults at once instead of quietly writing into memory
	 * that belongs to something else.
	 *
	 * The global is always LANDING BUFFER 0.  The vendor derives all three
	 * channel addresses from it, so every configuration starts the stream
	 * on buffer 0; cam_wdma3 adopts that layout below and owns every later
	 * move (issue #59).
	 */
	sensordplib_set_xDMA_baseaddrbyapp(0u, 0u,
	                                   (uint32_t)(uintptr_t)cam_raw_buf[0]);

	if (cam_dp_inp_config() != 0)
		return -1;

	hw5x5.hw5x5_path         = HW5x5_PATH_THROUGH_DEMOSAIC;
	hw5x5.demos_bndmode      = DEMOS_BNDODE_REFLECT;
	hw5x5.demos_color_mode   = DEMOS_COLORMODE_RGB;
	hw5x5.demos_pattern_mode = (DEMOS_PATTENMODE_E)cam_bayer;
	hw5x5.demoslpf_roundmode = DEMOSLPF_ROUNDMODE_FLOOR;
	hw5x5.hw55_crop_stx      = 0;
	hw5x5.hw55_crop_sty      = 0;
	hw5x5.hw55_in_width      = CAM_FRAME_WIDTH;
	hw5x5.hw55_in_height     = CAM_FRAME_HEIGHT;

	/* NULL callback: this port registers its own with
	 * hx_dplib_register_cb() instead of going through the SDK's
	 * event_handler scheduler. */
	sensordplib_set_hw5x5_wdma3(hw5x5, NULL);

	/*
	 * Adopt the channel layout the vendor just programmed (issue #59).
	 * This reads the six address/size registers back, refuses anything
	 * that does not tile buffer 0 exactly, and resets the machine to
	 * "buffer 0 armed and readable" -- which also re-establishes the
	 * indices after the vendor's software reset, whose effect on these
	 * registers no document describes.
	 */
	{
		const uint32_t bases[CAM_WDMA3_BUFFERS] = {
			(uint32_t)(uintptr_t)cam_raw_buf[0],
			(uint32_t)(uintptr_t)cam_raw_buf[1],
		};

		if (cam_wdma3_capture_layout(bases, CAM_RAW_BYTES) != 0) {
			LOG_ERR("wdma3 layout refused: %s (status %08lx)",
			        cam_wdma3_fault(),
			        (unsigned long)cam_wdma3_fault_status());
			return -1;
		}
	}
	return 0;
}

int cam_dp_capture_start(void)
{
	sensordplib_set_mclkctrl_xsleepctrl_bySCMode();
	if (sensordplib_set_sensorctrl_start() != 0) {
		LOG_ERR("sensor controller start failed");
		return -1;
	}
	return 0;
}

int cam_dp_frame_complete(const uint8_t **completed)
{
	if (cam_wdma3_frame_complete() != 0) {
		LOG_ERR("wdma3 frame refused: %s (status %08lx)",
		        cam_wdma3_fault(),
		        (unsigned long)cam_wdma3_fault_status());
		return -1;
	}
	if (completed != NULL)
		*completed = cam_raw_buf[cam_wdma3_read_index()];
	return 0;
}

int cam_dp_arm_next(void)
{
	if (cam_wdma3_arm_next() != 0) {
		LOG_ERR("wdma3 arm refused: %s (status %08lx)",
		        cam_wdma3_fault(),
		        (unsigned long)cam_wdma3_fault_status());
		return -1;
	}
	return 0;
}

/* The one-function seam cam_wdma3.h declares: the retrigger belongs to the
 * sensordp layer, and this file is the one that already speaks it. */
void cam_wdma3_hw_retrigger(void)
{
	sensordplib_retrigger_capture();
}

void cam_dp_full_stop(void)
{
	/* The vendor's order, and every failure path in this port funnels
	 * through it.  Nothing here checks a return value or bails out early:
	 * this runs when things have ALREADY gone wrong, and a stop that gives
	 * up half way leaves a datapath that is neither running nor quiet. */
	sensordplib_stop_capture();
	sensordplib_start_swreset();
	sensordplib_stop_swreset_WoSensorCtrl();
	(void)cam_sensor_stream_off();
	sensordplib_csirx_disable();
}

/* ---- the EDM seam (issue #68) -------------------------------------------- */

void cam_dp_edm_observe(cam_dp_edm_cb cb)
{
	/*
	 * The vendor prototype takes the same shape (a uint32_t event), so this
	 * is a pass-through and not an adapter.  Passing NULL clears the vendor's
	 * pointer; the ordering rules that make either direction safe are in
	 * cam_dp.h and enforced by where camera.c calls this.
	 */
	hx_drv_edm_register_timing_cb((EDM_ISREvent_t)cb);
}

void cam_dp_edm_read(uint32_t *mask, uint32_t wdt[3])
{
	uint32_t i;

	if (mask != NULL) {
		*mask = 0u;
		hx_drv_edm_get_mask(mask);
	}
	if (wdt == NULL)
		return;
	/* The vendor numbers its watchdogs from one; the array is from zero. */
	for (i = 0u; i < 3u; i++) {
		wdt[i] = 0u;
		hx_drv_edm_get_wdt_count((uint8_t)(i + 1u), &wdt[i]);
	}
}

/* ---- chip revision ------------------------------------------------------- */

uint32_t cam_dp_chip_version(void)
{
	uint32_t chipid = 0u, version = 0u;

	if (hx_drv_scu_get_version(&chipid, &version) != SCU_NO_ERROR)
		return 0u;
	return chipid;
}

int cam_dp_needs_rev_c_bounce(void)
{
	return cam_dp_chip_version() == CAM_CHIP_VERSION_C;
}
