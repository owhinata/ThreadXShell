/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_load_end.h
 * @brief   Where this board's `nn model load` ends, and what that obliges
 *          (issue #122).
 *
 * The load runs one order -- stage and check the copy -> nn_model_reload() ->
 * only if the backend took the new model, swap the plugin -- and then has to
 * say which model is open, whether that is a failure, whether the plugin is
 * unpublished, whether the last result goes and which container's claims
 * describe what is open.  Those answers are this table.
 *
 * WHY IT IS A PURE FUNCTION IN ITS OWN FILE.  Two of its rows cannot be produced
 * from a console: a backend that refuses the new model AND then the previous one
 * it was running (the other staging slot is untouched, so the rebuild "should
 * not fail"), and a plugin the device refuses after the host packer ran the
 * device's own validator over it.  So the decision is separated from the code
 * that acts on it and test/test_nn_load_end.c walks every entry.
 *
 * [!] WHAT IT DOES NOT COVER.  That the plugin is swapped only after the
 * backend, and that the adapter acts on every flag, is held down by there being
 * one call site in port/nn/nn_svc_wio.c -- not by this file.
 */
#ifndef NN_LOAD_END_H
#define NN_LOAD_END_H

#ifdef __cplusplus
extern "C" {
#endif

/** Whose container claims describe what is open afterwards. */
enum nn_load_claims {
	NN_LOAD_CLAIMS_KEEP = 0,   /**< the previous model's, unchanged         */
	NN_LOAD_CLAIMS_NEW,        /**< this load's (none for a bare model)     */
	NN_LOAD_CLAIMS_NONE,       /**< nothing is open, so nobody's            */
};

/** What the adapter must do.  Every flag is an obligation, not a hint. */
struct nn_load_verdict {
	unsigned char state;       /**< enum nn_model_state, for the operator    */
	unsigned char ok;          /**< the load's status is success             */
	unsigned char unload;      /**< unpublish the plugin, explicitly         */
	unsigned char invalidate;  /**< the last result goes: what is open moved */
	unsigned char claims;      /**< enum nn_load_claims                      */
};

/**
 * Should the plugin be swapped at all?  Only when the backend TOOK the new
 * model -- a plugin swapped before that, or after a rollback, would destroy the
 * previous model's decoder for nothing (the reservation is one region).
 *
 * @param reload_rc    what nn_model_reload() returned
 * @param model_after  whether it left a model open (its own out-parameter)
 */
int nn_load_swaps_plugin(int reload_rc, int model_after);

/**
 * Decide, once the plugin step has run (or been skipped).
 *
 * @param plugin_refused  the plugin was refused after the backend took the new
 *                        model; ignored when nn_load_swaps_plugin() said no
 *
 * [!] A SUCCESS THAT LEFT NO MODEL IS EMPTY AND A FAILURE.  The backend refuses
 * a model with no inputs, so it cannot happen -- and if it did, the answer that
 * cannot leave a decoder behind a model that is gone is the one to give.
 */
void nn_load_decide(int reload_rc, int model_after, int plugin_refused,
                    struct nn_load_verdict *v);

#ifdef __cplusplus
}
#endif

#endif /* NN_LOAD_END_H */
