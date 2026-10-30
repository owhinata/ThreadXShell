/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    fault_plugin.h
 * @brief   Whether a fault happened inside the loaded plugin, and what the
 *          fault handler does about it (issue #159 = #78 Step 4b).  tflm only.
 *
 * [!] A PLUGIN'S FAULT IS RECORDED AND THE BOARD RESETS AT ONCE (the shared
 * plugin rule; wio-lite-ai's since #110).  The reset IS the teardown -- there is
 * no camera to stop, no lease to give back and no session to release, because
 * none of them survive it -- so the handler reads only the loader's published
 * metadata (plugin_run_attribute()) and never follows a pointer into the plugin.
 * A fault anywhere else keeps this board's existing end: the full dump over the
 * UART, then the halt the IWDG resets (src/fault.c).  Under a debugger a plugin
 * fault halts too, so SWD can look.
 *
 * [!] IT SAYS WHERE THE PC IS, NOT WHAT DID IT.  An imprecise fault can honestly
 * claim no more; the offset is what feeds `arm-none-eabi-addr2line` against the
 * plugin ELF that was packed.
 */
#ifndef FAULT_PLUGIN_H
#define FAULT_PLUGIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** How a fault ends. */
enum fault_end {
	FAULT_END_DUMP_HALT = 0,  /**< not the plugin's: dump, then halt (today) */
	FAULT_END_RESET,          /**< the plugin's: recorded, reset now          */
	FAULT_END_DEBUG_HALT,     /**< the plugin's, under a debugger: halt       */
};

/**
 * @param pc        the stacked PC
 * @param pc_valid  the stacked frame was readable (else nothing is attributed)
 * @param debugger  a debugger owns the core (DHCSR C_DEBUGEN)
 * @param who       receives the plugin's name, or NULL when @p pc is not in it
 * @param off       receives the offset from the image base when it is
 * @return how the handler ends.  Safe from an exception: no lock, no
 *         allocation, nothing inside the plugin is read.
 */
enum fault_end fault_plugin_end(uint32_t pc, int pc_valid, int debugger,
                                const char **who, uint32_t *off);

#ifdef __cplusplus
}
#endif

#endif /* FAULT_PLUGIN_H */
