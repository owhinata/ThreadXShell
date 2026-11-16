/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host stand-in for the HAL, for test_nor_board.c (issue #250): the one call
 * cmds/cmd_nor_board.c makes, HAL_GetTick(), defined by the test.
 */
#ifndef NOR_SHIM_STM32F7XX_HAL_H
#define NOR_SHIM_STM32F7XX_HAL_H

#include <stdint.h>

uint32_t HAL_GetTick(void);

#endif /* NOR_SHIM_STM32F7XX_HAL_H */
