/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nor_cmd.h
 * @brief   Who holds the NOR, in the words `nor` and `blob` both refuse with
 *          (#92, #250).
 *
 * Exported out of cmds/cmd_nor_board.c so that `blob write` and `nor erase` /
 * `nor write` refuse in the same words.  The refusal is the point: it names the holder when
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
 * @brief  Say who holds the part, in the words the `nor` hold refuses with.
 *
 * One cli_error line, "<who_asks>: busy -- <holder>", naming the holder when
 * there is exactly one cmds/cmd_nor_board.c knows.  Exported for the shared `blob`
 * command's busy refusal (cmds/cmd_blob_board.c, issue #194), so that `blob`
 * and `nor` name a holder from one mapping.
 */
void nor_cmd_say_busy(struct cli_instance *sh, const char *who_asks);

#ifdef __cplusplus
}
#endif

#endif /* NOR_CMD_H */
