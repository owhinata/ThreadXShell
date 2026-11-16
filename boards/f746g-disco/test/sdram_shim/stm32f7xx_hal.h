/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host stand-in for the HAL, for test_sdram_board.c (issue #251): the PRIMASK
 * calls cmds/cmd_sdram.c makes around the SDRAM owner word, defined by the
 * test (which counts them).
 */
#ifndef SDRAM_SHIM_STM32F7XX_HAL_H
#define SDRAM_SHIM_STM32F7XX_HAL_H

#include <stdint.h>

uint32_t __get_PRIMASK(void);
void __set_PRIMASK(uint32_t pm);
void __disable_irq(void);

#endif /* SDRAM_SHIM_STM32F7XX_HAL_H */
