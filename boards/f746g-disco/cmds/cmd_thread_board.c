/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_thread_board.c
 * @brief   This board's instance of the shared `thread` command's state
 *          (issue #206).
 *
 * shell/cmds/cmd_thread.c owns no mutable storage, so the cpu% baseline and the
 * single-run guard it keeps between runs are defined here, as plain .bss
 * (zero is the boot state).  The type and the reason are in
 * shell/cmds/thread_cmd_board.h.  Without the Execution Profile Kit the command
 * keeps nothing and nothing is defined.
 */
#include "thread_cmd_board.h"

#ifdef TX_EXECUTION_PROFILE_ENABLE
struct thread_cmd_state thread_cmd_board_state;
#endif
