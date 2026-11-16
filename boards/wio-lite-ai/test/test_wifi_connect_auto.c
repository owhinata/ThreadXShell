/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Issue #240 -- wifi.autoreconnect is read at the start of every association.
 *
 * src/wifi_connect.c is compiled unchanged (with the store, BSP_ENABLE_KV=1) and
 * wifi_connect_run() -- the one association the boot, `wifi reconnect` and a typed
 * `wifi connect` all run -- is driven against the stand-ins below, which record the
 * order of the calls that matter:
 *
 *   R  rtl_link_refuse_early()   (the telnet / background refusal, issue #244)
 *   D  wifi_auto_disarm()        (drops what was held, owhinata/wio-lite-ai#32)
 *   K  wifi_auto_setting_read()  (the read, with its warning asked for)
 *   B  rtl_link_begin()          (the claim of the link)
 *   A  wifi_auto_arm()           (a successful association, given what it read)
 *   E  rtl_link_end()            (the link given back)
 *
 * What is checked: a refused command reads nothing and changes nothing ("R"); every
 * other run reads the key after the disarm and before the link is claimed, drops
 * what is held again once the link is its own, and arms before it gives the link
 * back (the review of #240: what is held follows the association that finished
 * last, and the link is what orders them); a
 * successful association hands what IT read to the arm (every setting is walked: only
 * ON keeps), and says which on the console (`autoreconnect armed for ...` or the one
 * `autoreconnect off -- ...` line).
 *
 * [!] WHAT IT DOES NOT COVER: that the three callers reach wifi_connect_run() (read
 * from the code: cmds/cmd_wifi.c and src/kv_boot.c's cfg_wifi()), the read itself
 * (src/kv_boot.c's kv_boot_autoreconnect_read(), whose answer is test_kv_wifi.c's),
 * and src/wifi_auto.c, which is replaced here by a stand-in with the same rule
 * (arming keeps the credentials only when told to).  Two associations at once, and
 * the `wifi status` text, through the real src/wifi_auto.c, are test_wifi_auto.c's.
 */
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "cli_instance.h"
#include "cli.h"
#include "rtl_link.h"
#include "wifi_auto.h"
#include "wifi_connect.h"
#include "wifi_rpc.h"
#include "erpc.h"
#include "link_bridge.h"
#include "nx_net.h"
#include "net_core.h"
#include "kv_boot.h"
#include "stm32h7xx_hal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- the stand-ins ----------------------------------------------------------- */

static char ev[64];
static unsigned nev;
static int fk_refuse, fk_begin, fk_join_ok;
static enum wifi_auto_setting fk_kv;
static bool fk_warned, fk_armed;
static int fk_said_armed, fk_said_off;

static void note(char c)
{
	if (nev + 1u < sizeof ev) {
		ev[nev++] = c;
		ev[nev] = '\0';
	}
}

int rtl_link_refuse_early(struct cli_instance *sh, const char *what)
{ note('R'); return fk_refuse; }
void wifi_auto_disarm(const char *why) { note('D'); fk_armed = false; }
enum wifi_auto_setting wifi_auto_setting_read(bool warn, const char **store_state)
{
	note('K');
	fk_warned = warn;
	return fk_kv;
}
bool wifi_auto_arm(const char *ssid, const char *password, uint32_t security, bool keep)
{
	note('A');
	fk_armed = keep;              /* the rule src/wifi_auto.c keeps */
	return keep;
}
int rtl_link_begin(struct cli_instance *sh, bool power_on)
{ note('B'); return fk_begin; }
void rtl_link_end(struct cli_instance *sh) { note('E'); }
int rtl_abort_cb(void *ctx) { return 0; }
bool rtl_tcpip_inited(void) { return true; }
void rtl_tcpip_set_inited(bool v) { }

uint8_t erpc_module_gen(void) { return WIFI_BRIDGE_MIN_GEN; }
void erpc_set_module_gen(uint8_t gen) { }
int erpc_system_version(char *out, uint16_t cap, struct erpc_diag *d) { return -1; }
int wifi_rpc_off(const struct wifi_rpc_opts *o, int32_t *r) { *r = 0; return 0; }
int wifi_rpc_on(const struct wifi_rpc_opts *o, uint32_t m, int32_t *r)
{ *r = 0; return 0; }
int wifi_rpc_connect(const struct wifi_rpc_opts *o, const char *s, const char *p,
                     uint32_t sec, int32_t *r)
{
	*r = fk_join_ok ? WIFI_RPC_OK : -1;
	return 0;
}
int wifi_rpc_get_last_error(const struct wifi_rpc_opts *o, int32_t *e)
{ *e = WIFI_RPC_ERR_WRONG_PASSWORD; return 0; }   /* ends the retries at once */
const char *wifi_rpc_err_name(int32_t e) { return "fake"; }
int wifi_rpc_tcpip_init(const struct wifi_rpc_opts *o, int32_t *r) { *r = 0; return 0; }

int link_bridge_hold_extend(void) { return 0; }
enum link_bridge_state link_bridge_state(void) { return LINK_BRIDGE_UP; }
bool link_bridge_is_up(void) { return true; }
int link_bridge_up(const char **why) { return 0; }
int nx_net_write_status(net_svc_write_fn w, void *ctx) { return 0; }
bool net_core_source_is_dhcp(void) { return false; }

uint32_t HAL_GetTick(void) { return 0u; }

int cli_print(struct cli_instance *sh, const char *f, ...)
{
	if (strncmp(f, "  autoreconnect armed for", 25) == 0)
		fk_said_armed++;
	if (strncmp(f, "  autoreconnect off -- `kv set wifi.autoreconnect true bool`", 60) == 0)
		fk_said_off++;
	return 0;
}
int cli_error(struct cli_instance *sh, const char *f, ...) { return 0; }
int cli_write(struct cli_instance *sh, const void *d, size_t n) { return (int)n; }
int cli_sleep(struct cli_instance *sh, unsigned t) { return 0; }
bool cli_cancel_requested(struct cli_instance *sh) { return false; }

/* ---- the test ---------------------------------------------------------------- */

static struct cli_instance sh;
static int failures;

static int run(int refuse, enum wifi_auto_setting kv, int begin, int join_ok, bool was)
{
	nev = 0u;
	ev[0] = '\0';
	fk_refuse = refuse;
	fk_kv = kv;
	fk_begin = begin;
	fk_join_ok = join_ok;
	fk_warned = false;
	fk_armed = was;               /* held from an earlier association */
	fk_said_armed = fk_said_off = 0;
	return wifi_connect_run(&sh, "ap", "secret", WIFI_RPC_SEC_WPA2_AES_PSK, false,
	                        false);
}

static void check(int ok, const char *what)
{
	if (!ok) {
		printf("FAIL: %s (calls \"%s\")\n", what, ev);
		failures++;
	}
}

int main(void)
{
	int was, si;

	for (was = 0; was <= 1; was++) {
		for (si = WIFI_AUTO_SET_ON; si <= WIFI_AUTO_SET_NO_KV; si++) {
			enum wifi_auto_setting k = (enum wifi_auto_setting)si;
			bool on = (k == WIFI_AUTO_SET_ON);

			/* Refused first (telnet or `&`): nothing read, nothing changed. */
			check(run(1, k, RTL_LINK_READY, 1, was) == 1, "refused: returns 1");
			check(strcmp(ev, "R") == 0, "refused: no disarm, no read, no claim");
			check(fk_armed == (bool)was, "refused: what is held is as it was");

			/* The claim fails: disarm, read, then the claim -- in that order. */
			check(run(0, k, RTL_LINK_ERR, 1, was) == 1, "claim fails: returns 1");
			check(strcmp(ev, "RDKB") == 0, "claim fails: disarm, read, then claim");
			check(fk_warned, "claim fails: an association asks for the warning");
			check(!fk_armed, "claim fails: nothing held");

			/* The join fails: the same order, and nothing armed. */
			check(run(0, k, RTL_LINK_READY, 0, was) == 1, "join fails: returns 1");
			check(strcmp(ev, "RDKBDE") == 0,
			      "join fails: disarm, read, claim, disarm, give back; no arm");
			check(!fk_armed, "join fails: nothing held");

			/* Connected: arms with what it read, keeps only on ON, says which. */
			check(run(0, k, RTL_LINK_READY, 1, was) == 0, "connected: returns 0");
			check(strcmp(ev, "RDKBDAE") == 0,
			      "connected: disarm, read, claim, disarm, arm, then give back");
			check(fk_armed == on, "connected: credentials held only when true");
			check(fk_said_armed == on && fk_said_off == !on,
			      "connected: one line, `armed for` or `off`");
		}
	}

	if (failures) {
		printf("test_wifi_connect_auto: %d FAILED\n", failures);
		return 1;
	}
	printf("test_wifi_connect_auto: OK\n");
	return 0;
}
