/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host-test shim for the SDK's hx_drv_scu.h.  Declares only the entry points
 * the seam calls, with the enumerator values copied from the SDK's
 * hx_drv_scu_export.h (SCU_NO_ERROR = 0, SCU_TIMERCTRL_CPU = 0) -- the mocks
 * live in seam_host_env.c.
 */
#ifndef SEAM_SHIM_HX_DRV_SCU_H
#define SEAM_SHIM_HX_DRV_SCU_H

#include <stdint.h>

typedef enum {
	SCU_NO_ERROR = 0,
	SCU_ERROR_INVALID_PARAMETERS,
} SCU_ERROR_E;

#define SCU_TIMERCTRL_CPU              0u
#define SCU_CLK_FREQ_TYPE_SB_APB_1_CLK 0u

SCU_ERROR_E hx_drv_scu_set_timer_clk_en(uint32_t id, uint8_t en);
SCU_ERROR_E hx_drv_scu_get_timer_clk_en(uint32_t id, uint8_t *en);
SCU_ERROR_E hx_drv_scu_set_timer_clkdiv(uint32_t id, uint32_t div);
SCU_ERROR_E hx_drv_scu_get_timer_clkdiv(uint32_t id, uint32_t *div);
SCU_ERROR_E hx_drv_scu_set_timer_ctrl(uint32_t id, uint32_t ctrl);
SCU_ERROR_E hx_drv_scu_get_freq(uint32_t type, uint32_t *hz);

#endif /* SEAM_SHIM_HX_DRV_SCU_H */
