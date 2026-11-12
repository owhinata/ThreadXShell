/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    ymodem.h
 * @brief   Clean-room YMODEM-CRC batch sender + receiver (svc/ layer).
 *
 * Transfers a single file over a byte stream using the YMODEM protocol (CRC-16
 * variant, 1024-byte STX blocks with a 128-byte SOH block 0 carrying the
 * filename + exact size).  The transport and the data source/sink are both
 * injected as vtables, so this core has NO dependency on the shell, ThreadX or
 * HAL -- it sits in the freestanding svc/ layer next to fmt.c/log.c and is
 * unit-tested on the host (shell/test/test_ymodem.c) against mock IO.
 *
 * Ported verbatim from ../stm32f746g-disco (its owhinata/stm32f746g-disco#50) for
 * owhinata/wio-lite-ai#19 M4: the RTL8720DN full-chip flash backup (`wifi flash
 * backup`) needs to stream megabytes off the board, and this is the shell's designed
 * binary-transfer path. The shell wires @ref ym_io to cli_read_byte()/cli_write()
 * (see shell/cmds/cmd_xfer.c) and @ref ym_source to the RTL8720 download-protocol
 * flash reader.
 *
 * owhinata/wio-lite-ai#19 M5 added the RECEIVE direction (@ref ymodem_recv / @ref
 * ym_sink): to (re)write the RTL8720DN's firmware the board must be able to take an
 * image FROM the PC -- that is also the only way to restore the verified stock
 * backup, so it has to exist BEFORE anything writes the module's boot sectors.  The
 * sender is deliberately untouched by that work: it is the M4 path already proven on
 * board #2.
 *
 * Clean-room: written from the YMODEM/XMODEM-CRC protocol description, NOT
 * derived from lrzsz (GPL) or any other implementation.  Interoperates with the
 * PC-side lrzsz `rb`/`rz` receivers.
 */
#ifndef YMODEM_H
#define YMODEM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @ref ym_io getc() sentinels (distinct from any 0..255 byte value). */
#define YM_IO_TIMEOUT (-1)   /**< no byte within the requested timeout */
#define YM_IO_ABORT   (-2)   /**< local abort: Ctrl+C / thread kill */

/**
 * Byte transport, injected by the caller.  Pure: no knowledge of cli_instance.
 */
struct ym_io {
	void *ctx;
	/** Blocking-with-timeout single-byte read.  Returns 0..255 on a byte, or
	 *  YM_IO_TIMEOUT / YM_IO_ABORT.  timeout_ms==0 means "poll once". */
	int  (*getc)(void *ctx, unsigned timeout_ms);
	/** Send @p len bytes, blocking until queued/sent.  0 on success, <0 fatal. */
	int  (*put)(void *ctx, const uint8_t *buf, size_t len);
};

/**
 * Byte source (the file or camera frame), injected by the caller.
 */
struct ym_source {
	void       *ctx;
	const char *name;   /**< basename for block 0 (no '/'), e.g. "frame.raw" */
	uint32_t    size;   /**< exact byte count for block 0 */
	/** Read up to @p want bytes at the current position into @p dst; set
	 *  *@p got to the number produced (0 == EOF).  Returns 0 on success, <0 on
	 *  a read fault (e.g. a concurrent capture changed the frame, or an FX
	 *  error).  Sequential; ymodem advances the position.  A short read
	 *  (*got < want) that is NOT EOF is allowed -- the core re-reads to fill a
	 *  full block, so only *got == 0 ends the stream. */
	int  (*read)(void *ctx, uint8_t *dst, uint32_t want, uint32_t *got);
};

/**
 * Byte sink (the receive-side counterpart of @ref ym_source), injected by the
 * caller.  For owhinata/wio-lite-ai#19 M5 this is the RTL8720 firmware-image staging
 * buffer in PSRAM (app/rtl8720_img.c).
 */
struct ym_sink {
	void *ctx;
	/** Called once, after block 0 parses, with the sender's filename and exact
	 *  size.  @p name is NUL-terminated and may be empty; @p size is 0 when the
	 *  sender declared none.  Return <0 to REJECT the file (too big, no room,
	 *  hardware not ready) -- the receiver then cancels the batch with CAN and
	 *  ymodem_recv() returns YM_ERR_SINK.  No write() follows a rejection. */
	int  (*begin)(void *ctx, const char *name, uint32_t size);
	/** Append @p len bytes at the current position.  Called only between a
	 *  successful begin() and the end of the file, and never with more than the
	 *  declared size in total (the trailing CPMEOF padding of the final block is
	 *  trimmed).  Return <0 to abort (same teardown as a begin() rejection). */
	int  (*write)(void *ctx, const uint8_t *data, uint32_t len);
};

/** Bytes in one framed send block: kind, seq, ~seq, 1024 data bytes, CRC-16. */
#define YM_SEND_AREA_BYTES (3u + 1024u + 2u)
/** Bytes in one received block body (the largest, an STX block's data). */
#define YM_RECV_AREA_BYTES 1024u

/**
 * The sender's working area: the one block it frames and puts on the wire.
 * Its layout is visible so that a board can define one, but only svc/ymodem.c
 * reads or writes the bytes.  CPU only: io->put() copies them out.
 */
struct ym_send_area {
	uint8_t block[YM_SEND_AREA_BYTES];
};

/** The receiver's working area: one block body as it arrives.  As above, the
 *  board only places it; svc/ymodem.c alone touches the bytes. */
struct ym_recv_area {
	uint8_t data[YM_RECV_AREA_BYTES];
};

/**
 * Who the transfers' working areas belong to (issue #230).  svc/ymodem.c holds
 * no writable storage of its own; the board defines ONE of these, with its
 * own send and receive areas, and passes it to every ymodem_send() and
 * ymodem_recv().  The two areas are separate, so a send and a receive never
 * share bytes.  Where the board places them is its decision (the board README
 * says where and why).
 */
struct ym_xfer {
	struct ym_send_area *send;   /**< used by ymodem_send() only */
	struct ym_recv_area *recv;   /**< used by ymodem_recv() only */
	/**
	 * "A transfer is running on this owner": 0 when idle, which is what the
	 * board initialises it to.  Only svc/ymodem.c touches it, and only with
	 * the compiler's atomic operations (no library call, no ThreadX, no
	 * interrupt masking): ymodem_send() / ymodem_recv() set it from 0 to 1 in
	 * one step before anything else and put it back to 0 at their one exit;
	 * ymodem_busy() reads it.  Taken from threads only, never from an ISR.
	 *
	 * This makes "one transfer at a time" hold for the whole firmware because
	 * a board defines exactly ONE struct ym_xfer and passes it to every
	 * transfer (issue #230): a send and a receive refuse each other too, not
	 * just two of a kind.  It is not the console claim (cli_console_claim(),
	 * counted per console, #229): that one is per console, this one is
	 * global.  The order is claim the console -> this -> ... -> this back ->
	 * release the console; neither waits, so the two cannot deadlock.
	 */
	unsigned char running;
};

/** ymodem_send() / ymodem_recv() outcome. */
enum ym_result {
	YM_OK = 0,       /**< file transferred and the batch was closed */
	YM_ERR_CANCEL,   /**< peer sent CAN, or the IO returned ABORT */
	YM_ERR_TIMEOUT,  /**< exhausted retries waiting for a handshake / ACK / block */
	YM_ERR_SOURCE,   /**< source->read() returned <0 (frame changed / FX error) */
	YM_ERR_IO,       /**< put() returned <0 (transport failure) */
	YM_ERR_SINK,     /**< sink->begin()/write() returned <0 (recv only) */
	YM_ERR_PROTO,    /**< unrecoverable framing error, e.g. an out-of-order block */
	/**
	 * Refused before it started: another transfer is running on the same
	 * struct ym_xfer (issue #230).  Nothing was done -- not one byte put or
	 * read on @p io, no CAN sequence, the source / sink never called, and the
	 * running transfer's state and record untouched.  (A receive's own
	 * @p diag is still set to the "nothing received" value.)  That promise is
	 * about the protocol core only: the board's wiring still drains ITS OWN
	 * console afterwards, as after any failure, since a PC the operator had
	 * already started may be sending to it.
	 */
	YM_ERR_BUSY,
};

/**
 * Would a ymodem_send() / ymodem_recv() on @p x started now be refused with
 * YM_ERR_BUSY?  1 while a transfer is running on @p x, 0 otherwise (an atomic
 * read of the mark, issue #230).  For a caller that prints a notice to the
 * operator ("start the receiver now") before it starts a transfer: asking
 * first keeps that notice from going out for a transfer that will be refused.
 * It is only an early answer -- another console can start a transfer between
 * this and the call -- and what decides is the atomic take inside
 * ymodem_send() / ymodem_recv().
 */
int ymodem_busy(const struct ym_xfer *x);

/**
 * Drive one full YMODEM batch send of a single file.  Blocking; runs to
 * completion or a terminal error.  On ANY non-success result a CAN sequence is
 * emitted so the PC side (rz/rb) tears down cleanly.  At most src->size bytes are
 * sent regardless of what the source returns (block 0's size is authoritative).
 * Frames every block in @p x's send area.  Not reentrant on one @p x: the
 * caller must serialise transfers -- the shell does so by holding the output
 * lock.  Returns YM_ERR_BUSY, having done nothing (no CAN either), when another
 * transfer is running on @p x; see enum ym_result.
 */
enum ym_result ymodem_send(struct ym_xfer *x, const struct ym_io *io,
                           const struct ym_source *src);

/**
 * Drive one full YMODEM batch RECEIVE of a single file into @p sink.  Blocking;
 * runs to completion or a terminal error.  Interoperates with the PC-side lrzsz
 * `sb` sender and with ymodem_send() above.
 *
 * Sequence: emit 'C' until block 0 arrives -> sink->begin(name, size) -> ACK ->
 * 'C' -> data blocks (ACK each, NAK a bad CRC so the sender resends) -> EOT ->
 * ACK -> 'C' -> the sender's closing null block 0 -> ACK.
 *
 * YM_OK REQUIRES REACHING THAT CLOSING BLOCK.  The batch is only complete once
 * the empty-name block 0 has been received and acknowledged: ymodem_send() always
 * emits one (see ym_run()), and leaving it unread would strand the sender's last
 * ACK and dump the remainder into whatever reads the console next.  A file whose
 * data arrived but whose batch never closed returns YM_ERR_TIMEOUT -- callers must
 * treat that as a failure and discard what the sink accumulated, not as success.
 *
 * At most the block-0 size is delivered to the sink (that size is authoritative,
 * exactly as on the send side).  A short final block's CPMEOF padding is trimmed.
 * Reads every block into @p x's receive area (separate from the send area).
 * Not reentrant on one @p x: the caller must serialise transfers.  Returns
 * YM_ERR_BUSY, having done nothing but set @p diag, when another transfer is
 * running on @p x; see enum ym_result.
 *
 * @p diag is the caller's own record (see struct ym_recv_diag), never NULL.
 * ymodem_recv() sets it to ymodem_recv_diag_init()'s value on entry (before it
 * asks for the mark, so a refused receive reads "nothing received") and fills
 * it as the transfer runs; nothing else writes it, so it still holds this
 * transfer's counts when the caller reads them.
 */
struct ym_recv_diag;
enum ym_result ymodem_recv(struct ym_xfer *x, const struct ym_io *io,
                           const struct ym_sink *sink,
                           struct ym_recv_diag *diag);

/**
 * Why a ymodem_recv() went the way it did.  A failed receive on real
 * hardware looks the same from the outside whichever layer broke, so this records
 * enough to tell them apart without another round trip:
 *
 *   short_read > 0   the body of a block did not arrive in full -> BYTES WERE LOST
 *                    on the transport (ring overflow, USB flow control), not a
 *                    protocol problem.
 *   bad_crc > 0      full-length blocks arriving corrupted -> the link is damaging
 *                    data rather than dropping it.
 *   bad_seq > 0      well-formed blocks in the wrong order -> a protocol mismatch.
 *   blocks == 0      nothing was ever accepted, not even block 0.
 *
 * @ref first_* snapshot the FIRST rejected block, so a cascade of retries does not
 * bury the original cause.  The caller owns it (issue #230): ymodem_recv()
 * writes the one it is given and keeps no copy.
 */
struct ym_recv_diag {
	uint32_t blocks;       /**< blocks accepted (block 0 included) */
	uint32_t bad_crc;      /**< blocks rejected on CRC */
	uint32_t bad_seq;      /**< blocks rejected on seq / ~seq */
	uint32_t short_read;   /**< block bodies that timed out part-way */
	uint32_t timeouts;     /**< header waits that expired */
	int      first_kind;   /**< first rejected block: SOH/STX, or -1 if none */
	int      first_seq;    /**< its seq byte, or -1 if not read */
	int      first_nseq;   /**< its ~seq byte, or -1 if not read */
	uint32_t first_got;    /**< body bytes actually received before it failed */
	uint32_t first_want;   /**< body bytes expected (128 or 1024) */
	uint16_t first_crc_want;
	uint16_t first_crc_got;
};

/**
 * Set @p d to the value a record has before any block was read: every count 0
 * and the first_* fields at their "none" values (first_kind, first_seq and
 * first_nseq -1).  A caller sets its record with this before it hands it on,
 * so a path that fails before ymodem_recv() runs (a refused console, for one)
 * still reads as "nothing received" rather than an earlier transfer's counts or
 * whatever the stack held.
 */
void ymodem_recv_diag_init(struct ym_recv_diag *d);

/**
 * CRC-16/CCITT (XMODEM variant: poly 0x1021, init 0x0000).  Exposed for the
 * host unit test.  ymodem_crc16("123456789", 9) == 0x31C3.
 */
uint16_t ymodem_crc16(const uint8_t *p, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* YMODEM_H */
