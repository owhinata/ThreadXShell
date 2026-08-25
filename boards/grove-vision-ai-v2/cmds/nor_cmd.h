/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nor_cmd.h
 * @brief   The one way a command takes the NOR writer reservation (#92).
 *
 * Exported out of cmd_nor.c so that `blob write` and `nor erase` / `nor write`
 * refuse in the same words.  The refusal is the point: it names the holder when
 * there is exactly one and it is a slot this build knows about, so an operator
 * gets "close `nn`" rather than "busy".  A second copy of that mapping would be
 * a second thing to keep in step with nor_state.h's slot list -- which is
 * exactly what nor_flash.h hands out the raw lease mask to avoid.
 */
#ifndef NOR_CMD_H
#define NOR_CMD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cli_instance;

/**
 * @brief  Bring the port up if it is not, then take the writer reservation.
 *
 * @param token  receives the reservation token; 0 on any refusal
 * @return 0 with the reservation held, or -1 having already told the operator
 *         why -- faulted, or busy and who has it.
 *
 * The caller owns the token from here and must give it back with
 * nor_unreserve() on every path.
 */
int nor_cmd_writer_enter(struct cli_instance *sh, uint32_t *token);

#ifdef __cplusplus
}
#endif

#endif /* NOR_CMD_H */
