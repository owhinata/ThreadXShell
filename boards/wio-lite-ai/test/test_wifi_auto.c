/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Issue #240 -- src/wifi_auto.c and src/wifi_connect.c compiled unchanged (with the
 * store, BSP_ENABLE_KV=1) against the stand-ins below: the module, the link, the host
 * stack and the store's read.  Two things are checked.
 *
 * 1. TWO ASSOCIATIONS AT ONCE (both reviews of #240).  wifi_connect_run() is entered
 *    by the USB console (priority 16) and by the boot's configuration thread (15),
 *    and nothing keeps the two apart before rtl_link_begin().  The coarse link mutex
 *    then puts the two associations one after the other.  What is held at the end
 *    must follow the association that FINISHED last -- it is what the module is
 *    joined to: that one's credentials when it read true, nothing when it read
 *    false.  Neither association may leave its credentials held after reading false.
 *
 *    The order that broke it: A (the USB console) holds the link and B (the boot)
 *    waits for it.  The moment A gives the link back, B -- the higher priority --
 *    takes it and runs to the end, and only then does the rest of A run.  Until the
 *    second review A armed in that rest, after B, so A's answer overwrote B's.
 *
 *    The stand-ins keep the link as a holder and a log of who took and gave it back,
 *    refuse a second taker while it is held (the radio parts never overlap), and run
 *    B whole at the moment A gives the link back -- the order above.  B's read is
 *    then later than in the real run (B read before it waited), which changes
 *    nothing: each association keeps its own answer.  Both pairs are run:
 *
 *      a_false_b_true:  A reads false, B reads true  -> B's credentials held
 *      a_true_b_false:  A reads true,  B reads false -> nothing held
 *
 * 2. THE `wifi status` TEXT: wifi_auto_status_text() for every setting, held and not,
 *    and wifi_auto_print() end to end for the store not open (the heading carries the
 *    store's state) and for a read that must not warn.
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
#include "log.h"
#include "tx_api.h"
#include "stm32h7xx_hal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- the stand-ins ----------------------------------------------------------- */

uint32_t connect_shim_primask;

static char out[4096];          /* everything printed, since the last clear */
static size_t nout;
static int warned;              /* store warnings reported */

static void out_clear(void)
{
	nout = 0u;
	out[0] = '\0';
}

static int out_add(const char *f, va_list ap)
{
	int n = vsnprintf(out + nout, sizeof out - nout, f, ap);

	if (n > 0)
		nout += ((size_t)n < sizeof out - nout) ? (size_t)n : sizeof out - nout - 1u;
	return n;
}

int cli_print(struct cli_instance *sh, const char *f, ...)
{
	va_list ap;
	int n;

	va_start(ap, f);
	n = out_add(f, ap);
	va_end(ap);
	return n;
}
int cli_error(struct cli_instance *sh, const char *f, ...)
{
	va_list ap;
	int n;

	va_start(ap, f);
	n = out_add(f, ap);
	va_end(ap);
	return n;
}
int cli_write(struct cli_instance *sh, const void *d, size_t n) { return (int)n; }
int cli_sleep(struct cli_instance *sh, unsigned t) { return 0; }
bool cli_cancel_requested(struct cli_instance *sh) { return false; }
void log_write(unsigned l, const char *t, const char *f, ...) { }

/*
 * The store: what the association running at each depth reads.  depth 0 is A,
 * 1 is B, run when A gives the link back.
 */
static enum wifi_auto_setting kv_at[2];
static int depth;
static const char *kv_state_words = "";
enum wifi_auto_setting kv_boot_autoreconnect_read(bool warn, const char **store_state)
{
	enum wifi_auto_setting s = kv_at[depth];

	if (warn && (s == WIFI_AUTO_SET_NOT_BOOL || s == WIFI_AUTO_SET_UNREADABLE))
		warned++;
	if (store_state != NULL)
		*store_state = (s == WIFI_AUTO_SET_NO_STORE) ? kv_state_words : "";
	return s;
}

int wifi_rpc_connect(const struct wifi_rpc_opts *o, const char *s, const char *p,
                     uint32_t sec, int32_t *r)
{
	*r = WIFI_RPC_OK;
	return 0;
}

/*
 * The coarse link mutex: who holds it (0 = nobody, else depth + 1) and every take
 * ('+') and give-back ('-') in order, as "A+A-B+B-".  A take while it is held is a
 * radio part overlapping another one, which the real mutex never allows.
 */
static struct cli_instance sh_usb, sh_cfg;
static int link_holder, link_overlap;
static char link_log[32];
static const char *inner_ssid;   /* B's network, run when A gives the link back */
static int inner_rc = -1;

static void link_note(char who, char what)
{
	size_t n = strlen(link_log);

	if (n + 2u < sizeof link_log) {
		link_log[n] = who;
		link_log[n + 1u] = what;
		link_log[n + 2u] = '\0';
	}
}

int rtl_link_begin(struct cli_instance *sh, bool power_on)
{
	if (link_holder != 0)
		link_overlap++;
	link_holder = depth + 1;
	link_note((char)('A' + depth), '+');
	return RTL_LINK_READY;
}

void rtl_link_end(struct cli_instance *sh)
{
	link_holder = 0;
	link_note((char)('A' + depth), '-');
	/* B, of higher priority, was waiting: it takes the link now and runs to the end
	 * before the rest of A. */
	if (inner_ssid != NULL && depth == 0) {
		const char *ssid = inner_ssid;

		inner_ssid = NULL;
		depth = 1;
		inner_rc = wifi_connect_run(&sh_cfg, ssid, "pw-of-B",
		                            WIFI_RPC_SEC_WPA2_AES_PSK, false, false);
		depth = 0;
	}
}

int rtl_link_refuse_early(struct cli_instance *sh, const char *what) { return 0; }
int rtl_abort_cb(void *ctx) { return 0; }
bool rtl_tcpip_inited(void) { return true; }
void rtl_tcpip_set_inited(bool v) { }
uint32_t rtl_link_forget_gen(void) { return 1u; }
uint8_t erpc_module_gen(void) { return WIFI_BRIDGE_MIN_GEN; }
void erpc_set_module_gen(uint8_t gen) { }
int erpc_system_version(char *o, uint16_t cap, struct erpc_diag *d) { return -1; }
int wifi_rpc_off(const struct wifi_rpc_opts *o, int32_t *r) { *r = 0; return 0; }
int wifi_rpc_on(const struct wifi_rpc_opts *o, uint32_t m, int32_t *r)
{ *r = 0; return 0; }
int wifi_rpc_get_last_error(const struct wifi_rpc_opts *o, int32_t *e)
{ *e = WIFI_RPC_ERR_WRONG_PASSWORD; return 0; }
const char *wifi_rpc_err_name(int32_t e) { return "fake"; }
int wifi_rpc_tcpip_init(const struct wifi_rpc_opts *o, int32_t *r) { *r = 0; return 0; }
int link_bridge_hold_extend(void) { return 0; }
enum link_bridge_state link_bridge_state(void) { return LINK_BRIDGE_UP; }
bool link_bridge_is_up(void) { return true; }
int link_bridge_up(const char **why) { return 0; }
int nx_net_write_status(net_svc_write_fn w, void *ctx) { return 0; }
bool net_core_source_is_dhcp(void) { return false; }
uint32_t HAL_GetTick(void) { return 0u; }
ULONG tx_time_get(void) { return 0u; }

/* ---- helpers ----------------------------------------------------------------- */

static int failures;

static void check(int ok, const char *what)
{
	if (!ok) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

/* `wifi status`'s own lines, read with the store answering ON (only the "held"
 * lines matter here, and the read must not change what is held). */
static void status_into(char *buf, size_t cap)
{
	enum wifi_auto_setting keep0 = kv_at[0], keep1 = kv_at[1];

	kv_at[0] = kv_at[1] = WIFI_AUTO_SET_ON;
	out_clear();
	wifi_auto_print(&sh_usb);
	(void)snprintf(buf, cap, "%s", out);
	kv_at[0] = keep0;
	kv_at[1] = keep1;
}

static int holds(const char *status, const char *ssid)
{
	char want[64];

	(void)snprintf(want, sizeof want, "  armed for \"%s\"", ssid);
	return strstr(status, want) != NULL;
}

static int holds_any(const char *status)
{
	return strstr(status, "  armed for \"") != NULL;
}

/* Run A (reading @a_kv) with B (reading @b_kv) waiting for the link; leave
 * `wifi status` at the end in @end. */
static int overlap(enum wifi_auto_setting a_kv, enum wifi_auto_setting b_kv,
                   char *end, size_t cap)
{
	int rc;

	wifi_auto_disarm("test start");
	kv_at[0] = a_kv;
	kv_at[1] = b_kv;
	inner_ssid = "B";
	inner_rc = -1;
	link_holder = link_overlap = 0;
	link_log[0] = '\0';
	rc = wifi_connect_run(&sh_usb, "A", "pw-of-A", WIFI_RPC_SEC_WPA2_AES_PSK,
	                      false, false);
	status_into(end, cap);
	return rc;
}

/* ---- 1. two associations at once ---------------------------------------------- */

static void check_order(const char *name)
{
	char what[96];

	(void)snprintf(what, sizeof what, "%s: A took and gave the link back, then B "
	               "(\"%s\")", name, link_log);
	check(strcmp(link_log, "A+A-B+B-") == 0, what);
	(void)snprintf(what, sizeof what, "%s: the radio parts never overlap", name);
	check(link_overlap == 0 && link_holder == 0, what);
}

static void test_a_false_b_true(void)
{
	char end[1024];

	check(overlap(WIFI_AUTO_SET_FALSE, WIFI_AUTO_SET_ON, end, sizeof end) == 0 &&
	      inner_rc == 0, "a_false_b_true: both connect");
	check_order("a_false_b_true");
	check(!holds(end, "A"), "a_false_b_true: A read false, A's credentials not held");
	check(holds(end, "B"),
	      "a_false_b_true: B finished last and read true, so B's credentials are held");
}

static void test_a_true_b_false(void)
{
	char end[1024];

	check(overlap(WIFI_AUTO_SET_ON, WIFI_AUTO_SET_FALSE, end, sizeof end) == 0 &&
	      inner_rc == 0, "a_true_b_false: both connect");
	check_order("a_true_b_false");
	check(!holds(end, "A"),
	      "a_true_b_false: A is no longer joined, so A's credentials are not held");
	check(!holds_any(end),
	      "a_true_b_false: B finished last and read false, so nothing is held");
}

/* ---- 2. the `wifi status` text ------------------------------------------------- */

static const char next_keeps[] =
	"  the next successful `wifi connect` / `wifi reconnect` keeps the credentials\r\n";
static const char next_drops[] =
	"  held until the next `wifi connect` / `wifi reconnect`; if the link is lost "
	"before then, it re-associates with it\r\n";
static const char turn_on[] =
	"  `kv set wifi.autoreconnect true bool`, then `wifi reconnect` (or "
	"`wifi connect`) to arm it\r\n";
static const char no_kv[] =
	"  this build has no configuration store, so it stays off\r\n";

static void test_status_text(void)
{
	static const struct {
		enum wifi_auto_setting s;
		const char *heading;
		const char *next_held, *next_not_held;
	} t[] = {
		{ WIFI_AUTO_SET_ON,
		  "wifi: autoreconnect on (wifi.autoreconnect is true)\r\n",
		  NULL, next_keeps },
		{ WIFI_AUTO_SET_FALSE,
		  "wifi: autoreconnect off (wifi.autoreconnect is false)\r\n",
		  next_drops, turn_on },
		{ WIFI_AUTO_SET_MISSING,
		  "wifi: autoreconnect off (wifi.autoreconnect is not set)\r\n",
		  next_drops, turn_on },
		{ WIFI_AUTO_SET_NOT_BOOL,
		  "wifi: autoreconnect off (wifi.autoreconnect is not a bool -- it was "
		  "stored without `bool`)\r\n",
		  next_drops, turn_on },
		/* The store cannot be used: nothing it would refuse is suggested. */
		{ WIFI_AUTO_SET_UNREADABLE,
		  "wifi: autoreconnect off (wifi.autoreconnect could not be read -- the "
		  "store is busy or failing)\r\n",
		  next_drops, NULL },
		{ WIFI_AUTO_SET_NO_STORE,
		  "wifi: autoreconnect off (the configuration store is not open: %s)\r\n",
		  next_drops, NULL },
		{ WIFI_AUTO_SET_NO_KV, "wifi: autoreconnect off\r\n", no_kv, no_kv },
	};
	unsigned i;
	int armed;

	for (i = 0; i < sizeof t / sizeof t[0]; i++) {
		for (armed = 0; armed <= 1; armed++) {
			struct wifi_auto_status_text x = wifi_auto_status_text(t[i].s, armed);
			const char *want = armed ? t[i].next_held : t[i].next_not_held;
			char what[64];

			(void)snprintf(what, sizeof what, "status text: setting %d, %s",
			               (int)t[i].s, armed ? "held" : "not held");
			check(x.heading != NULL && strcmp(x.heading, t[i].heading) == 0, what);
			check((want == NULL) ? (x.next == NULL)
			                     : (x.next != NULL && strcmp(x.next, want) == 0), what);
			/* No suggestion the state cannot follow. */
			if (t[i].s == WIFI_AUTO_SET_UNREADABLE || t[i].s == WIFI_AUTO_SET_NO_STORE)
				check(x.next == NULL || strstr(x.next, "kv set") == NULL, what);
		}
	}
	/* Not a value of the enum: off, and nothing suggested. */
	check(strstr(wifi_auto_status_text((enum wifi_auto_setting)0, false).heading,
	             "off") != NULL &&
	      wifi_auto_status_text((enum wifi_auto_setting)0, false).next == NULL,
	      "status text: an unknown setting is off");
}

static void test_status_print(void)
{
	/* The store not open: the heading says in what state, and nothing else is
	 * asked of it. */
	wifi_auto_disarm("test");
	kv_at[0] = WIFI_AUTO_SET_NO_STORE;
	kv_state_words = "not initialized yet";
	out_clear();
	wifi_auto_print(&sh_usb);
	check(strstr(out, "wifi: autoreconnect off (the configuration store is not open: "
	                  "not initialized yet)\r\n") != NULL,
	      "status print: the store's state in the heading");
	check(strstr(out, "kv set") == NULL && strstr(out, "wifi reconnect") == NULL,
	      "status print: no `kv set` / `wifi reconnect` while the store is not open");

	/* A key stored as a string: said in the heading, never warned by `wifi status`. */
	warned = 0;
	kv_at[0] = WIFI_AUTO_SET_NOT_BOOL;
	out_clear();
	wifi_auto_print(&sh_usb);
	check(warned == 0, "status print: `wifi status` does not add a warning");
	check(strstr(out, "is not a bool") != NULL, "status print: the heading says it");

	/* ...while an association does warn. */
	kv_at[0] = WIFI_AUTO_SET_NOT_BOOL;
	check(wifi_connect_run(&sh_usb, "A", "pw", WIFI_RPC_SEC_WPA2_AES_PSK, false,
	                       false) == 0 && warned == 1,
	      "status print: an association reports the warning once");
}

int main(void)
{
	test_a_false_b_true();
	test_a_true_b_false();
	test_status_text();
	test_status_print();

	if (failures) {
		printf("test_wifi_auto: %d FAILED\n", failures);
		return 1;
	}
	printf("test_wifi_auto: OK\n");
	return 0;
}
