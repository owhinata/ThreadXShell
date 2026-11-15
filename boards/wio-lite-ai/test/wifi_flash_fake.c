/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Stand-ins for everything cmds/cmd_wifi_flash.c calls, for test_wifi_flash_guard.c
 * (issue #235).  They only count: the question under test is whether a subcommand
 * reaches wifi_auto_disarm() / link_bridge_down() before the telnet refusal.  Every
 * call that would move hardware fails, so a path that got past the guard stops there.
 * The prototypes come from the board's real headers, so a signature change breaks
 * this build instead of drifting.
 */
/* A fake ignores most of what it is given. */
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "cli_instance.h"
#include "net_tty.h"
#include "xfer_console.h"
#include "rtl8720.h"
#include "rtl8720_flash.h"
#include "rtl8720_img.h"
#include "psram.h"
#include "log.h"
#include "rtl_link.h"
#include "wifi_auto.h"
#include "link_bridge.h"

/* Read and reset by the test (declared there as extern). */
struct cli_instance *fk_telnet_sh;   /* the console the test calls telnet, or NULL */
int fk_guard_calls, fk_disarm, fk_bridge_down, fk_hw_claim, fk_psram, fk_out;
const char *fk_refused_as;

/* Refuse only when asked about the telnet console itself, as net_tty_is_console() does. */
static int refuse(struct cli_instance *sh, const char *as)
{ if (sh == NULL || sh != fk_telnet_sh) return 0; fk_refused_as = as; return 1; }
int net_tty_guard(struct cli_instance *sh, const char *what)
{ fk_guard_calls++; return refuse(sh, what); }
int xfer_refuse_if_telnet(struct cli_instance *sh) { return refuse(sh, "xfer"); }
void wifi_auto_disarm(const char *why) { fk_disarm++; }
enum link_bridge_state link_bridge_state(void) { return LINK_BRIDGE_UP; }
void link_bridge_down(void) { fk_bridge_down++; }
const char *link_bridge_state_name(enum link_bridge_state s) { return "up"; }
int rtl_link_hw_claim(struct cli_instance *sh, bool b) { fk_hw_claim++; return -1; }
void rtl_link_hw_release(struct cli_instance *sh) { }
int psram_ready(void) { return 1; }
int psram_acquire(void) { fk_psram++; return 0; }
void psram_release(void) { }

/* Text out: counted, not formatted.  The guard fake prints nothing, so a non-zero
 * count on a refused run means the command spoke before it asked. */
int cli_print(struct cli_instance *sh, const char *f, ...) { return ++fk_out; }
int cli_error(struct cli_instance *sh, const char *f, ...) { return ++fk_out; }
int cli_warn(struct cli_instance *sh, const char *f, ...) { return ++fk_out; }
int cli_hexdump_base(struct cli_instance *sh, const void *d, size_t n, unsigned long long b)
{ return ++fk_out; }
int cli_sleep(struct cli_instance *sh, unsigned t) { return 1; }  /* Ctrl+C */
int cli_console_claim(struct cli_instance *sh) { return -1; }
void cli_console_release(struct cli_instance *sh) { }
void log_write(unsigned l, const char *t, const char *f, ...) { }

/* Never reached by the cases tested; failing values all the same. */
void rtl8720_reset(void) { }
void rtl8720_uart_close(void) { }
void rtl_link_forget_module(void) { }
int rtl_abort_cb(void *ctx) { return 1; }
int rtl_dl_enter(uint32_t h, int (*a)(void *), void *c) { return -1; }
int rtl_dl_load_flashloader(uint32_t b, int (*a)(void *), void *c) { return -1; }
int rtl_dl_detect_size(struct rtl_dl_size *s, int (*a)(void *), void *c) { return -2; }
int rtl_dl_flash_status(uint8_t sr[3], int (*a)(void *), void *c) { return -1; }
int rtl_dl_flash_jedec(struct rtl_dl_jedec *j, int (*a)(void *), void *c) { return -1; }
int rtl_dl_flash_chksum(uint32_t o, uint32_t l, uint32_t t, uint32_t *s,
                        int (*a)(void *), void *c) { return -1; }
int rtl_dl_read_flash(uint32_t o, uint32_t n, uint8_t *b, uint32_t cap,
                      int (*a)(void *), void *c) { return -1; }
int rtl_dl_flash_program(uint32_t o, const uint8_t *d, uint32_t l, uint32_t h,
                         struct rtl_dl_program *r, int (*a)(void *), void *c)
{ return -2; }
void rtl_dl_digest_init(struct rtl_dl_digest *d) { }
void rtl_dl_digest_add(struct rtl_dl_digest *d, const uint8_t *p, uint32_t n) { }
uint32_t rtl_dl_digest_value(const struct rtl_dl_digest *d) { return 0u; }
const uint8_t rtl_dl_km0_magic[RTL_DL_KM0_MAGIC_LEN];

static uint8_t img_bytes[16];
static struct rtl_img img = { "fake.bin", 4096u, 4096u, 0u, 1 };   /* valid: write goes on */
const struct rtl_img *rtl_img_get(void) { return &img; }
const uint8_t *rtl_img_data(void) { return img_bytes; }
uint32_t rtl_img_verify(void) { return 1u; }
int rtl_img_probe(void) { return -1; }
int rtl_img_finish(void) { return -1; }
void rtl_img_invalidate(void) { }
const struct ym_sink *rtl_img_sink(void) { return NULL; }
int xfer_refuse_if_busy(struct cli_instance *sh) { return 1; }
int xfer_send_source(struct cli_instance *sh, const struct ym_source *s) { return -1; }
int xfer_recv_sink_locked(struct cli_instance *sh, const struct ym_sink *k,
                          struct ym_recv_diag *d) { return -1; }
void ymodem_recv_diag_init(struct ym_recv_diag *d) { }
