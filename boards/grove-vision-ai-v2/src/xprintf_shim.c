/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    xprintf_shim.c
 * @brief   Board-owned xprintf(): SDK driver diagnostics -> the dmesg log ring.
 *
 * The prebuilt libdriver.a has exactly one unresolved x* symbol: xprintf
 * (verified with nm; e.g. hx_drv_timer.o calls it for diagnostics).  The SDK's
 * own library/common/xprintf.c is NOT compiled in this port -- it drags in the
 * clib console (console_io.h) this port deliberately does not link.  This shim
 * satisfies the reference instead: it formats with the shared svc/fmt engine
 * and feeds a line assembler into the RAM log, so driver chatter is readable
 * with `dmesg` and never touches the console.
 *
 * Prototype matches the SDK declaration (library/common/xprintf.h):
 *     void xprintf(const char *fmt, ...);
 *
 * Reentrancy: the driver may call this from thread and ISR context.  The line
 * assembler is a single shared instance, so the feed runs under a short
 * PRIMASK critical section (log_write itself is ISR/fault-safe already).
 */
#define LOG_TAG "sdk"
#include "log.h"

#include <stdarg.h>

#include "fmt.h"
#include "WE2_device.h"

static struct log_line g_sdk_line;      /* zero-initialised == valid (log.h) */

void xprintf(const char *fmt, ...)
{
	char buf[LOG_MSG_MAX + 1];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = fmt_vsnformat(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (n <= 0)
		return;
	if ((size_t)n >= sizeof buf)
		n = (int)sizeof buf - 1;

	{
		uint32_t pm = __get_PRIMASK();
		__disable_irq();
		log_line_feed(&g_sdk_line, LOG_TAG, buf, (size_t)n);
		__set_PRIMASK(pm);
	}
}
