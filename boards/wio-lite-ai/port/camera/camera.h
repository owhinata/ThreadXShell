/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Wio Lite AI ThreadX Shell Project
 */
/**
 * @file    camera.h
 * @brief   OV2640 DVP camera over DCMI: XCLK, SCCB, snapshot and continuous
 *          capture (owhinata/wio-lite-ai#8).
 *
 * The board's J7 FPC-24 connector carries a plain 8-bit DVP camera into the
 * STM32H725's DCMI.  Phase 1 brought up everything the sensor needs *before* a
 * single pixel can move -- the master clock, the SCCB (I2C) control bus, the
 * PWDN/RESETB lines and a chip-ID read (the part in hand is an OV2640, SCCB
 * 0x30, chip ID 0x2642).  Phase 2 adds the pixel path: the QVGA RGB565 register
 * sequence, and one frame captured over DCMI + DMA2_Stream1 into a PSRAM frame
 * buffer.  Continuous capture is the BAND stream (owhinata/wio-lite-ai#35): the
 * DCMI runs free with the DMA in double-buffer mode over two 60-row bands in
 * AXI-SRAM, and a producer thread hands each band to the band sink
 * (port/camera/cam_band.c), which feeds the LCD preview and the NN.  (Phase 3b's
 * frame stream into a ring of PSRAM slots through svc/frame_pipeline was removed
 * by issue #142.)
 *
 * Two board facts drive the whole design and both differ from the STM32F746
 * Discovery firmware this project is otherwise ported from
 * (../stm32f746g-disco port/camera, B-CAMS-OMV / OV5640):
 *
 *   - **The host must generate XCLK.**  Schematic sheet 7 leaves the 24 MHz
 *     oscillator OSC1 and its series R15 DNP; the only populated path to the
 *     module's XCLK pin is R11 (0R) from DCMI_XCLK = PA2.  The f746's module
 *     clocks itself from an on-board crystal, so that firmware has no XCLK code
 *     at all.  An OmniVision sensor derives its SCCB timing from XVCLK, so
 *     **the I2C bus stays dead until the clock runs** -- the single most
 *     confusing failure mode here.  PA2 is driven by TIM5_CH3 (AF2); TIM2, the
 *     other candidate, is already the ThreadX execution-profile time source
 *     (port/threadx/tx_glue.c).
 *
 *   - **The camera supply cannot be switched off.**  U8 (ME6216A28M3G, SOT23)
 *     feeds VDD_2V8 straight from SYS_5V with no enable pin.  f746's
 *     camera_power_off() cuts a GPIO-controlled rail; here camera_off() can only
 *     assert PWDN, hold RESETB low and stop XCLK.  Say so in help text rather
 *     than pretending the rail is gated.
 *
 * Layering: this module sits in port/ over HAL/CMSIS/ThreadX and pulls in
 * nothing from app/.  Concurrency: every public call serializes on an internal
 * TX_MUTEX and may block, so the API is **thread-context only** -- never from
 * an ISR, and never before camera_init() ran in tx_application_define().
 *
 * Clean-room implementation; RM0468, the board schematic and the OmniVision
 * SCCB protocol description were used as a register / wiring reference only.
 */
#ifndef CAMERA_H
#define CAMERA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error returns (negative); 0 is success.  Same names/meanings as the f746
   driver so the shell layer reads identically across the two firmwares. */
#define CAM_OK             0
#define CAM_ERR_PARAM     -1   /* bad argument                                  */
#define CAM_ERR_HAL       -2   /* HAL / SCCB I/O reported an error              */
#define CAM_ERR_TIMEOUT   -3   /* bus never went idle                           */
#define CAM_ERR_STATE     -4   /* driver not initialized / sensor not powered   */
#define CAM_ERR_NO_SENSOR -5   /* nothing answered, or an unrecognised chip ID  */
#define CAM_ERR_NO_FRAME  -6   /* no captured frame available                   */
#define CAM_ERR_BUSY      -7   /* a capture or stream already owns the DCMI      */
#define CAM_ERR_IN_USE    -8   /* another holder needs the stream (issue #142)  */
#define CAM_ERR_TEST     -10   /* running with a different `test` setting       */
#define CAM_ERR_STILL    -11   /* an `lcd reset` holds the camera still (#142)  */

/** Sensors this phase knows how to identify.  CAM_SENSOR_UNKNOWN means "some
 *  device ACKed on the SCCB bus but its ID did not match any candidate" -- a
 *  useful state, not an error to hide: `camera reg` can then poke at it. */
enum camera_sensor {
	CAM_SENSOR_NONE = 0,
	CAM_SENSOR_OV2640,
	CAM_SENSOR_OV5640,
	CAM_SENSOR_UNKNOWN,
};

/** Register-address width of the sensor's control bus, in bits. */
#define CAM_REG_WIDTH_8   8u
#define CAM_REG_WIDTH_16  16u

/** Capture geometry.  Phase 2 has exactly one mode; the struct exists so the
 *  shell never hardcodes the numbers and phase 3 can add modes without changing
 *  the command surface. */
#define CAMERA_FRAME_WIDTH   320u
#define CAMERA_FRAME_HEIGHT  240u
#define CAMERA_FRAME_BYTES   (CAMERA_FRAME_WIDTH * CAMERA_FRAME_HEIGHT * 2u)

/**
 * Band geometry for the staged stream (owhinata/wio-lite-ai#35).
 *
 * A band is a horizontal slice of the frame that the DMA delivers on its own.
 * 60 rows divides 240 exactly, which is what makes band boundaries coincide with
 * frame boundaries: the DCMI transfers active pixels only (RM0468 sec 36.3.6 /
 * 36.3.7), so a frame is exactly CAMERA_BANDS_PER_FRAME transfers and the fourth
 * one always ends where the frame does.  60 rows is also 38,400 B -- small enough
 * that two of them fit in AXI-SRAM beside everything else, and a multiple of 32 so
 * a cache maintenance operation on one band cannot touch the other.
 */
#define CAMERA_BAND_ROWS        60u
#define CAMERA_BANDS_PER_FRAME  (CAMERA_FRAME_HEIGHT / CAMERA_BAND_ROWS)
#define CAMERA_BAND_BYTES       (CAMERA_FRAME_WIDTH * CAMERA_BAND_ROWS * 2u)

/** Frames a snapshot discards before the one it keeps, so the sensor's own
 *  exposure and gain loops have converged.  Measured, not guessed: see
 *  camera_capture_locked() in camera.c.  Public because it is what makes a
 *  capture take ~1 s, and the shell says so before it blocks. */
#define CAMERA_WARM_FRAMES   15u

struct camera_mode {
	uint16_t width;
	uint16_t height;
	uint32_t frame_bytes;
	const char *format;    /**< "RGB565" */
};

/** Snapshot of driver + sensor state (camera_get_info). */
struct camera_info {
	uint8_t  ready;        /**< camera_init() completed                        */
	uint8_t  powered;      /**< XCLK running, PWDN released, RESETB deasserted */
	uint8_t  sensor;       /**< enum camera_sensor                             */
	uint8_t  i2c_addr;     /**< 7-bit SCCB address that answered, 0 if none    */
	uint8_t  reg_width;    /**< CAM_REG_WIDTH_8 / _16 of the identified sensor */
	uint8_t  configured;   /**< the QVGA RGB565 register sequence is loaded    */
	uint8_t  frame_valid;  /**< the frame buffer holds a captured image        */
	uint16_t chip_id;      /**< product ID (0x2642 = OV2640, 0x5640 = OV5640)  */
	uint16_t manuf_id;     /**< manufacturer ID (0x7FA2 for OmniVision), or 0  */
	uint32_t frame_bytes;  /**< bytes valid in the frame buffer                */
	uint32_t xclk_hz;      /**< XCLK actually being emitted, 0 when stopped    */
	uint32_t xclk_src_hz;  /**< TIM5 kernel clock the divider works from       */
	uint32_t i2c_ker_hz;   /**< I2C4 kernel clock (rcc_pclk4)                  */
};

/**
 * Configure GPIO, TIM5 and I2C4; create the driver mutex.  Idempotent.
 *
 * Touches no sensor: XCLK stays stopped, PWDN stays asserted and RESETB stays
 * low, so the module is left exactly as power-on reset left it.  Enables no
 * interrupt and waits on nothing, which is what makes it safe to call from
 * tx_application_define() before the scheduler runs (owhinata/wio-lite-ai#12's rule:
 * an interrupt source is only enabled after the ThreadX objects its ISR touches
 * exist -- phase 1 has no interrupt source at all).
 *
 * @return CAM_OK, or CAM_ERR_STATE / CAM_ERR_HAL.
 */
int camera_init(void);

/**
 * Run the power-up sequence: start XCLK, release PWDN, release RESETB, settle.
 * Blocks ~40 ms in tx_thread_sleep.  No SCCB traffic and no identification.
 */
int camera_on(void);

/**
 * Assert PWDN, hold RESETB low and stop XCLK.  Note this does NOT remove power
 * (see the file header) -- it is the deepest sleep this board can reach.
 * Clears the identification, so a later camera_reg_*() needs a fresh probe.
 */
int camera_off(void);

/**
 * Power-cycle the sensor (off, then on) and identify it over SCCB.
 *
 * On success @ref camera_info carries the sensor, its SCCB address, register
 * width and IDs.  On CAM_ERR_NO_SENSOR the sensor is deliberately left POWERED
 * so `camera scan` / `camera reg` can keep investigating -- unlike the f746
 * driver, which powers the module down on a failed probe because there the rail
 * is switchable and leaving it on costs current.
 */
int camera_probe(void);

/** Copy the current state out.  Never fails once camera_init() ran. */
int camera_get_info(struct camera_info *out);

/** Capture geometry / pixel format.  Constant in phase 2. */
int camera_get_mode(struct camera_mode *out);

/** Readout orientation as configured after ST's table, for `camera info`
 *  (owhinata/wio-lite-ai#53).  Never NULL. */
const char *camera_get_orient(void);

/** Which shape of stream owns the DCMI (camera_stream_stats.mode). */
enum camera_stream_mode {
	CAM_STREAM_NONE = 0,   /**< never started                                 */
	CAM_STREAM_BAND,       /**< 60-row bands into AXI-SRAM, pushed to a sink  */
};

/** Streaming counters (camera_stream_stats). */
struct camera_stream_stats {
	uint8_t  active;       /**< a stream is running right now                 */
	uint8_t  mode;         /**< enum camera_stream_mode; NONE until the first
	                        *   start, BAND after                             */
	uint32_t frames;       /**< frames completed (all four bands delivered)   */
	uint32_t elapsed_ms;   /**< run time (frozen at teardown for a post-stop read) */
	uint32_t dcmi_ovr;     /**< DCMI overrun / sync errors (terminal)         */
	uint32_t dma_fe;       /**< DMA FIFO/direct-mode errors tolerated          */
	uint32_t band_late;    /**< bands the consumer did not reach              */
	uint32_t band_torn;    /**< bands the DMA caught up with                  */
	uint32_t band_desync;  /**< frame ends off a band boundary                */
};

/*
 * ---- who holds the stream (issue #142) ----------------------------------------
 *
 * The BAND stream runs while, and only while, somebody holds it: the operator
 * (`camera stream start`) or nn (camera_stream_join()).  It has no length of its
 * own; it ends when the last holder leaves, or on an error -- which this board
 * does not recover from, so it is a confirmed end that marks every holder lost.
 */
struct cam_holder;          /* svc/cam_holders.h */
struct cam_holders_ticket;  /* svc/cam_holders.h */

/** A join's @p colorbar meaning "whatever the running stream has": never
 *  refused with CAM_ERR_TEST, and a start it makes is without the pattern.
 *  For a caller that was not asked for `test` (issue #142 review). */
#define CAM_COLORBAR_ANY (-1)

/**
 * The operator takes hold (`camera stream start`): starts the band stream if
 * nobody held it, otherwise only registers.  @p colorbar (the sensor's COM7 test
 * pattern) takes effect only on a start; a running stream with a different
 * setting refuses (CAM_ERR_TEST).  *@p already (may be NULL) is set when it was
 * already running.  A stop still finishing is waited for (bounded, once);
 * CAM_ERR_BUSY only if it outlasts that (retry).
 */
int camera_stream_start(int colorbar, int *already);

/**
 * The operator lets go (`camera stream stop`).  [!] Refused with CAM_ERR_IN_USE
 * while another holder needs the stream -- nothing changes and the operator
 * stays (D8); their names go to @p users (up to @p max, *@p nusers; any may be
 * NULL).  Otherwise the stream stops; this waits for the producer (<= 1 s)
 * to end the stream that was running when the stop was asked -- not whatever
 * runs by the time it looks.  Clears the operator's lost mark.
 */
int camera_stream_stop(const char **users, int max, int *nusers);

/**
 * camera_stream_stop(), also handing back in *@p gen (may be NULL) the
 * generation of the stream it asked to end (svc/cam_gen.h; CAM_GEN_NONE when
 * nothing was running).  The shared `camera off` (issue #143) waits on THAT
 * stream with camera_stream_ended(), never on "is a stream running".
 */
int camera_stream_stop_gen(const char **users, int max, int *nusers,
                           uint32_t *gen);

/** Nonzero once the stream of generation @p gen has ended (CAM_GEN_NONE: at
 *  once).  A lock-free read of the teardown's record. */
int camera_stream_ended(uint32_t gen);

/**
 * Holder @p h joins the stream, taking the operator's place too when nothing
 * streams -- in which case it starts it with @p colorbar.  Joining a running
 * stream with a different explicit @p colorbar is refused (CAM_ERR_TEST);
 * CAM_COLORBAR_ANY takes the running one.  A stop still finishing is waited for
 * as in camera_stream_start().  *@p tk says what this call registered and under
 * which generations, for camera_stream_leave() to undo.  Clears @p h's lost
 * mark.
 */
int camera_stream_join(const struct cam_holder *h, int colorbar,
                       struct cam_holders_ticket *tk);

/**
 * Holder @p h leaves.  @p undo is the join's ticket when this undoes a failed
 * start (exactly what that join registered goes, and only while it is still the
 * one it made); NULL for an ordinary leave (the operator stays, the stream runs
 * on), which also clears @p h's lost mark.  Waits for the stream to stop when
 * this was the last holder.
 */
int camera_stream_leave(const struct cam_holder *h,
                        const struct cam_holders_ticket *undo);

/** The holders' names in registration order (static strings, up to @p max);
 *  *@p lost (may be NULL): holders the last confirmed end lost. */
int camera_stream_holders(const char **names, int max, int *lost);

/** The names of the holders that keep the operator from stopping the stream. */
int camera_stream_users(const char **users, int max);

/**
 * Was @p h lost at a confirmed end, and has not joined since?  A lost holder's
 * consumer must not resume on a stream somebody else restarts.
 */
int camera_stream_lost(const struct cam_holder *h);

/** The running (or last) band stream's `test` setting. */
int camera_stream_colorbar(void);

/**
 * Band consumer, called from the producer thread once per delivered band.
 *
 * @param band  0 .. CAMERA_BANDS_PER_FRAME-1, always in that order: the driver
 *              drops the remainder of a frame rather than hand out a gap, so a
 *              consumer that starts a frame at band 0 gets all of it or nothing.
 * @param px    CAMERA_BAND_ROWS rows of RGB565, CAMERA_FRAME_WIDTH wide, in
 *              AXI-SRAM.  Valid only for the duration of the call, and READ-ONLY:
 *              the DMA owns this memory and the driver has just invalidated it.
 *
 * Thread context (never an ISR), but on the DRIVER'S producer thread -- what runs
 * here delays the next band, so it must finish well inside a band period (~18 ms
 * at the sensor's ~13.5 fps).  Do not block on anything slow here; the display
 * flip in app/cam_preview.c is deliberately handed to another thread.
 */
typedef void (*camera_band_fn)(void *ctx, unsigned band, const uint16_t *px,
                               unsigned rows);

/**
 * The BAND stream: the DCMI lands in AXI-SRAM, not in the PSRAM.
 *
 * WHY THIS MODE EXISTS (owhinata/wio-lite-ai#35).  The panel is portrait and the
 * camera landscape, so every displayed frame is transposed, and a transpose reads one
 * side with a stride.  With whole frames in PSRAM that strided side was the
 * external OCTOSPI1 -- 25 ms of CPU per frame, and enough extra bus arbitration
 * to make the DCMI's FIFO complain.  Staging 60-row bands in AXI-SRAM puts the
 * strided reads on internal RAM and takes the DCMI off OCTOSPI1 altogether.
 *
 * The DMA runs in double-buffer mode over two fixed band buffers and the memory
 * registers are never rewritten (RM0468 sec 15.3.11's on-the-fly target change is
 * not needed).  The stream fills no frame, so `camera save` / `send` keep showing the
 * last camera_capture().  It is exclusive with camera_capture().
 */
/* Started only by the holder table above (issue #142): camera.c passes the one
   band sink, port/camera/cam_band.c, so there is no public entry point. */

/** Nonzero while the band stream is running (the only stream since issue #142;
 *  camera_streaming() is the same answer, kept for the OCTOSPI1 retune gate). */
int camera_band_streaming(void);

/** Nonzero while a stream owns the DCMI (its preview transposes into the LTDC
 *  frame buffer in PSRAM).  Read by app/psram.c to refuse OCTOSPI1 retunes --
 *  see psram_acquire(). */
int camera_streaming(void);

/**
 * Keep the camera still: refused with CAM_ERR_BUSY while a stream runs, and
 * with CAM_ERR_STILL while another claim is out; once granted, every stream
 * start refuses with CAM_ERR_STILL until camera_still_release() (issue #142 D9:
 * `lcd reset` holds
 * it across the panel reset).  The look and the claim are one step under the
 * camera lock, so a start cannot slip in between them.  CAM_ERR_STATE when the
 * camera is not initialised -- nothing can start then, and nothing is owed.
 */
int camera_still_claim(void);
void camera_still_release(void);

int camera_stream_stats(struct camera_stream_stats *out);

/**
 * Configure the sensor if needed, then capture ONE frame into the driver's
 * frame buffer over DCMI + DMA.  Blocks until the frame lands or the transfer
 * times out (~1 s); the first call after a power cycle also spends ~250 ms
 * writing the QVGA RGB565 register sequence.
 *
 * @param colorbar  nonzero selects the sensor's internal test pattern instead
 *                  of the live image -- it proves the DCMI/DMA path without
 *                  involving optics or lighting.
 *
 * The frame buffer lives in the OCTOSPI1 PSRAM.  This call does NOT arbitrate
 * that bus: the caller must hold the psram guard (the shell does), because a
 * concurrent retune of OCTOSPI1 would stall the AXI, not merely spoil the
 * frame.
 *
 * @return CAM_OK, CAM_ERR_NO_SENSOR, CAM_ERR_HAL (sync/overrun/DMA error),
 *         CAM_ERR_TIMEOUT (no frame arrived), CAM_ERR_BUSY, CAM_ERR_STATE.
 */
int camera_capture(int colorbar);

/**
 * Copy @p len bytes at @p offset out of the captured frame.
 *
 * @param gen  (may be NULL) receives the frame generation counter.  A caller
 *             that reads the frame in several calls must check it does not
 *             change between them -- frame_valid alone cannot tell that a
 *             concurrent capture replaced the pixels underneath.
 * @return CAM_OK, CAM_ERR_NO_FRAME, CAM_ERR_PARAM, CAM_ERR_STATE.
 */
int camera_frame_read(uint32_t offset, void *dst, uint32_t len, uint32_t *gen);

/**
 * Scan 7-bit SCCB addresses 0x08..0x77 and store the ones that ACK.
 *
 * @param addrs  caller buffer receiving the ACKing addresses (may be NULL)
 * @param max    capacity of @p addrs
 * @param found  receives the number of devices found (may exceed @p max)
 * @return CAM_OK, or CAM_ERR_STATE when the sensor is not powered.
 */
int camera_scan(uint8_t *addrs, unsigned max, unsigned *found);

/**
 * Raw SCCB register access at an explicit address and width -- the tool for a
 * sensor camera_probe() could not identify, and the one used to debug register
 * sequences in phase 2.  @p reg_width is CAM_REG_WIDTH_8 or _16.
 */
int camera_reg_read(uint8_t i2c_addr, uint16_t reg, unsigned reg_width,
                    uint8_t *val);
int camera_reg_write(uint8_t i2c_addr, uint16_t reg, unsigned reg_width,
                     uint8_t val);

/** Human-readable forms for the shell. */
const char *cam_strerror(int rc);
const char *camera_sensor_name(unsigned sensor);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_H */
