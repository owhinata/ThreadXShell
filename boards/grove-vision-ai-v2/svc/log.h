/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    log.h
 * @brief   RAM log subsystem: levelled ring + dmesg + crash record.
 *
 * Port of the wio-lite-ai svc/log.h to the Grove Vision AI V2 (HX6538).
 * Messages are appended to a ring buffer in DTCM (.noinit.log), read back with
 * `dmesg`.  The fault handler (src/fault.c) records crashes here, and the SDK
 * driver diagnostics (src/xprintf_shim.c) land here as well.
 *
 * Persistence: every boot on this board goes through the Himax 2nd
 * bootloader, which reloads the ELF segments -- .noinit is in no LOAD segment
 * so the loader leaves it alone.  Surviving a RESETN-pin reset is CONFIRMED
 * (hardware, 2026-08-13: boot_count reached 2 with the first boot's records
 * still readable).  The reset in question was the board's UART auto-reset --
 * opening the serial port pulses RESETN through the CH343P's RTS line, see
 * this board's README.md -- NOT, as an earlier revision of this comment
 * claimed, the reboot at the end of an xmodem flash.  Other reset kinds
 * (power cycle in particular) remain untested, which is why log_init() still
 * validates the ring and rebuilds it when the magic is gone.
 *
 * Usage -- define LOG_TAG before including this header, then call the macros:
 *     #define LOG_TAG "uart"
 *     #include "log.h"
 *     LOG_INF("baud=%u", baud);
 * A message is dropped at compile time when its level is above
 * LOG_COMPILE_LEVEL, and at run time when above the threshold set by
 * log_set_level() (default INF).  Normal logs are NOT echoed to the console --
 * read them with `dmesg`.
 */
#ifndef LOG_H
#define LOG_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Severity levels: lower value == more severe (threshold compares with <=). */
#define LOG_LEVEL_ERR 0u
#define LOG_LEVEL_WRN 1u
#define LOG_LEVEL_INF 2u
#define LOG_LEVEL_DBG 3u

/* Compile-time floor: a level numerically above this is removed at build time.
 * Default keeps everything; override e.g. -DLOG_COMPILE_LEVEL=LOG_LEVEL_INF. */
#ifndef LOG_COMPILE_LEVEL
#define LOG_COMPILE_LEVEL LOG_LEVEL_DBG
#endif

/* Each translation unit may define its own module tag before #include "log.h". */
#ifndef LOG_TAG
#define LOG_TAG "??"
#endif

/* Longest message text and tag stored per record (each excluding the NUL). */
#define LOG_MSG_MAX 104
#define LOG_TAG_MAX 8

#define LOG_AT(lvl, ...) \
	do { \
		if ((lvl) <= LOG_COMPILE_LEVEL) \
			log_write((lvl), LOG_TAG, __VA_ARGS__); \
	} while (0)
#define LOG_ERR(...) LOG_AT(LOG_LEVEL_ERR, __VA_ARGS__)
#define LOG_WRN(...) LOG_AT(LOG_LEVEL_WRN, __VA_ARGS__)
#define LOG_INF(...) LOG_AT(LOG_LEVEL_INF, __VA_ARGS__)
#define LOG_DBG(...) LOG_AT(LOG_LEVEL_DBG, __VA_ARGS__)

/**
 * Validate (or re-initialise) the ring and record a boot marker.  Call once
 * from main() BEFORE fault_init() so the fault handler always finds a valid
 * ring.  log_write() is a no-op until this returns.
 */
void log_init(void);

/**
 * Append one formatted record at @p level tagged @p tag.  Safe from thread,
 * ISR and fault context (the ring update runs in a PRIMASK critical section
 * and calls no tx_* API).  No-op before log_init() or when @p level is above
 * the run-time threshold.  Console is never touched -- read with `dmesg`.
 */
void log_write(unsigned level, const char *tag, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
void log_vwrite(unsigned level, const char *tag, const char *fmt, va_list ap);

/** Drop all stored records, keeping the sequence counter (dmesg -c). */
void log_clear(void);

/** Decoded cause of THIS boot's reset.  On this board the cause register has
 *  not been identified (no public TRM); always "?" in M-G1. */
const char *log_reset_cause(void);

/** Run-time severity threshold: records above @p level are dropped (default
 *  INF).  log_get_level() returns the current threshold. */
void     log_set_level(unsigned level);
unsigned log_get_level(void);

/*
 * Line assembler: turn a stream of bytes into whole log records.  One
 * assembler belongs to one producer; not internally synchronised.  Used by
 * the xprintf shim, whose producer (the prebuilt driver) prints messages as
 * fragments.  ANSI CSI escapes are dropped on the way through.
 */
#define LOG_LINE_ASM_MAX (LOG_MSG_MAX + 1)

struct log_line {
	char     buf[LOG_LINE_ASM_MAX];
	uint16_t used;
	uint8_t  esc;                   /**< inside an escape sequence */
};

/** Reset an assembler (also correct on a zero-initialised one). */
void log_line_init(struct log_line *l);

/** Feed @p len bytes; emits one INF record per newline, tagged @p tag. */
void log_line_feed(struct log_line *l, const char *tag, const void *data,
                   size_t len);

/** Emit whatever is buffered, if anything (for a producer that ends mid-line). */
void log_line_flush(struct log_line *l, const char *tag);

/** One decoded record copied out for display. */
struct log_record {
	uint32_t ts_ms;                 /**< ThreadX tick (1 ms) at write time */
	uint32_t seq;                   /**< monotonic sequence number */
	uint8_t  level;                 /**< LOG_LEVEL_* */
	char     tag[LOG_TAG_MAX + 1];
	char     text[LOG_MSG_MAX + 1];
};

/** Snapshot cursor for one dmesg pass; fields are opaque to callers. */
struct log_iter {
	uint32_t pos;                   /**< next record offset (free-running) */
	uint32_t end;                   /**< head snapshot taken at iter_start */
};

/** Begin a pass over the records present now (snapshots head). */
void log_iter_start(struct log_iter *it);
/** Copy the next record into @p out; returns 1 on a record, 0 at end. */
int  log_iter_next(struct log_iter *it, struct log_record *out);

#ifdef __cplusplus
}
#endif

#endif /* LOG_H */
