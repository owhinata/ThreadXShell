/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nor_svc.h
 * @brief   The contract of the shared `nor` command (issue #250, epic #213).
 *
 * shell/cmds/cmd_nor.c holds the grammar, the argument checks (4 KB aligned
 * erases, the device's end, the bounds of `read` and `write`), the telnet and
 * background question (shell/cmds/nor_console.c), the acceptance test, every
 * line `nor` prints and the dmesg line of each erase, write and test.  A board
 * defines the one table below, nor_svc_board, under that fixed name (its
 * cmds/cmd_nor_board.c) and says what it has in its svc/nor_svc_config.h.
 *
 * [!] THE PROTECTION STAYS WITH THE BOARD.  How the device is held while it is
 * erased or written (the blob store's busy flag on f746g-disco and wio-lite-ai,
 * the writer reservation on grove-vision-ai-v2), which ranges are refused (the
 * kv partition; the seam's writable interval), whether the device is up: all
 * of it is in the board's functions.  The shared command takes no hold and
 * gives none back, so it has no exit on which one could be forgotten.
 *
 * THE HOLD.  hold() takes the board's hold, calls @p body once with the
 * operations that may be used only inside it, and gives the hold back at its
 * one exit once body returned; when the hold cannot be taken it calls nothing
 * and says why.  The operations and their context are valid for that call of
 * body only.  The shared command owns no storage
 * (cmake/check_no_mutable_storage.py), so it has nowhere to keep them.
 *
 * [!] AN ADAPTER TAKES NO `struct cli_instance`.  Printing and Ctrl+C are the
 * shared command's; the progress function of an erase is the only way back up,
 * and on grove-vision-ai-v2 it runs with the memory-mapped window down: the
 * shared one only prints and asks for Ctrl+C.
 *
 * Every string an adapter returns (why, names, hints) is a string literal or
 * lives as long as the image, so it outlives the call and needs no copy.
 * Freestanding: no HAL, no board header, no tx_api.h.
 */
#ifndef NOR_SVC_H
#define NOR_SVC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The erase unit every board has (4 KB); the shared command checks the
 *  table against it, and every erase it asks for is a whole number of it. */
#define NOR_SVC_ERASE_UNIT 4096u

/** `nor read` takes 1 .. this many bytes; `nor write` this many at most. */
#define NOR_SVC_READ_MAX   1024u
#define NOR_SVC_WRITE_MAX  256u

/** What one call did. */
enum nor_svc_status {
	/** Done (for hold(): the body ran). */
	NOR_SVC_OK = 0,
	/** The hold is somebody else's; nothing was done.  why: who. */
	NOR_SVC_BUSY,
	/** The device is down or faulted; nothing was done.  why: the reason. */
	NOR_SVC_DOWN,
	/** The board's operation refused the request before anything went on
	 *  the wire (outside what may be written, a target that is not erased).
	 *  why: the reason. */
	NOR_SVC_REFUSED,
	/** The part did not answer with the window down; nothing was sent. */
	NOR_SVC_NO_TRANSPORT,
	/** A transfer failed.  why: the driver's reason. */
	NOR_SVC_IO,
	/** Stopped part way by the device or the driver; done says how far. */
	NOR_SVC_INCOMPLETE,
	/** Stopped part way by the progress function (Ctrl+C); done says how
	 *  far, and everything up to it was erased. */
	NOR_SVC_CANCELLED,
	/** What was read back is not what was written.  Terminal on a board
	 *  that cannot tell it from a lying window (grove-vision-ai-v2). */
	NOR_SVC_FAULTED,
};

/** The result of one call.  The caller owns it; the adapter sets every field
 *  it knows and leaves the rest as the caller zeroed them. */
struct nor_svc_result {
	enum nor_svc_status status;
	const char *why;
	/** Bytes the device took, and bytes read back and compared by the
	 *  board's own operation (0 when it does not read back). */
	uint32_t done;
	uint32_t verified;
	/** The first byte that did not read back (only with bad_valid). */
	uint32_t bad_off;
	uint8_t  bad_got;
	uint8_t  bad_want;
	uint8_t  bad_valid;
	/** The JEDEC id the operation read again with the window down. */
	uint8_t  jedec_valid;
	uint8_t  jedec[3];
};

/** Progress and cancel of an erase.  The board calls tick() after each 4 KB
 *  it erased, if it can (nor_svc_board::erase_cancels); non-zero stops the
 *  erase after that unit. */
struct nor_svc_progress {
	void *ctx;
	int (*tick)(void *ctx, uint32_t done, uint32_t total);
};

/** The operations valid only inside hold()'s call of the body. */
struct nor_svc_ops {
	void *dev;
	void (*erase)(void *dev, uint32_t addr, uint32_t len,
	              const struct nor_svc_progress *p, struct nor_svc_result *r);
	void (*program)(void *dev, uint32_t addr, const uint8_t *data,
	                uint32_t len, struct nor_svc_result *r);
	/** The test's reads, under the hold; NULL on a board without a test. */
	void (*read)(void *dev, uint32_t addr, uint8_t *buf, uint32_t len,
	             struct nor_svc_result *r);
};

typedef void (*nor_svc_body_fn)(void *ctx, const struct nor_svc_ops *ops);

/** What a range is asked for. */
enum nor_svc_op { NOR_SVC_OP_ERASE = 0, NOR_SVC_OP_WRITE };

/** A range's answer: OK, or why not. */
enum nor_svc_range_kind {
	NOR_SVC_RANGE_OK = 0,
	/** It touches [lo, hi), which is @ref name's ("the kv store"). */
	NOR_SVC_RANGE_INSIDE,
	/** It is not wholly inside [lo, hi), what may be written. */
	NOR_SVC_RANGE_OUTSIDE,
	/** A write must start on a multiple of @ref lo bytes. */
	NOR_SVC_RANGE_ALIGN,
};

struct nor_svc_range {
	enum nor_svc_range_kind kind;
	uint32_t lo, hi;
	const char *name;
	/** Appended to the refusal (" -- `kv format yes` erases it"), or "". */
	const char *hint;
};

/** The lines every board's `nor info` has, sampled with the board's own. */
struct nor_svc_info {
	const char *state;
	const char *state_note;    /**< "" or why, said after " -- "    */
	uint8_t jedec_valid;
	uint8_t jedec[3];
	const char *jedec_note;    /**< "" or how the id was read, in () */
};

/** One whole line of the board's own `nor info` lines.  @return the bytes
 *  taken, or negative. */
typedef int (*nor_svc_write_fn)(void *ctx, const char *s, size_t len);
/** The shared lines, written from the board's sample.  @return 0 or -1. */
typedef int (*nor_svc_common_fn)(void *ctx, const struct nor_svc_info *in);

struct nor_svc_board {
	/** The part, for the head of `nor info` ("W25Q128JV on OCTOSPI2"). */
	const char *part;
	uint32_t size;
	uint32_t erase_unit;       /**< must be NOR_SVC_ERASE_UNIT */
	uint32_t page;
	/** The erase calls the progress function after each 4 KB. */
	uint8_t erase_cancels;
	/** NULL: the board has the subcommand.  Otherwise it has not, and the
	 *  string ("" for nothing) is said after "no `nor <sub>` on this board". */
	const char *no_read;
	const char *no_write;
	const char *no_test;
	/**
	 * `nor info`: sample everything once, then call @p common with the shared
	 * values and write the board's own lines.  @return 0; or -1 after a
	 * write failed; or 1 with @p r saying why nothing could be sampled.
	 */
	int (*info)(nor_svc_common_fn common, nor_svc_write_fn write, void *ctx,
	            struct nor_svc_result *r);
	/** `nor read`, with no hold (NULL with no_read). */
	void (*read)(uint32_t addr, uint8_t *buf, uint32_t len,
	             struct nor_svc_result *r);
	/** Is [addr, addr+len) one the board refuses?  A pure function; the
	 *  board's operations ask again.  @p len is not 0 and the range is on
	 *  the device. */
	void (*range)(enum nor_svc_op op, uint32_t addr, uint32_t len,
	              struct nor_svc_range *out);
	/** See THE HOLD.  r->status OK when the body ran. */
	void (*hold)(nor_svc_body_fn body, void *ctx, struct nor_svc_result *r);
};

extern const struct nor_svc_board nor_svc_board;

#ifdef __cplusplus
}
#endif

#endif /* NOR_SVC_H */
