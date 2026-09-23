/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_svc_config.h
 * @brief   Host-test stand-in for a board's nn capabilities (issue #122).
 *
 * Only for shell/test/test_nn_cmd_usage.c, which compiles the REAL
 * shell/cmds/cmd_nn.c.  The model loader is on; which sources it takes comes
 * from -D on the compile line, once per board shape, so one test file covers
 * the three combinations the boards declare.
 */
#ifndef NN_SVC_CONFIG_H
#define NN_SVC_CONFIG_H

#define NN_SVC_HAS_MODEL_LOAD  1

#endif /* NN_SVC_CONFIG_H */
