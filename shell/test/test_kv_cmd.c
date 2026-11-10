/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test: the shared `kv` command (shell/cmds/cmd_kv.c, issue #214) through
 * the REAL dispatcher over a stand-in store (the svc/kv/kv.h calls are defined
 * here; kv_type_name() is the real svc/kv/kv_record.c).  The board's words come
 * from kv_cfg/kv_svc_config.h, which are made up.
 *
 * No gate compares what a command prints, so the lines are pinned here:
 *  - the one table of secret keys: `kv list` hides wifi.psk's value (its length
 *    only), shows an unreadable one as unreadable, and `kv get` still prints it;
 *  - how each type's text becomes the stored bytes, and every refusal of a value
 *    or a type, before the store is called;
 *  - `kv desc` joins its words with single spaces and refuses over 64;
 *  - the state gate names the store's state and calls nothing;
 *  - every store result's sentence;
 *  - `kv info` (the board's medium in its first line) and `kv format` (yes
 *    required, the board's notice before it starts);
 *  - how a stored string, u32, bool and bytes value is shown, escapes included.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cli_instance.h"
#include "cli_internal.h"
#include "cli_backend_dummy.h"
#include "host_glue.h"
#include "kv.h"
#include "kv_svc_config.h"

/* ---- the stand-in store ---------------------------------------------------- */

#define STUB_MAX 8
static struct {
	int state;
	int rc;                       /* forced answer for every data call (0 = act) */
	int calls;                    /* data calls that reached the store            */
	int formats;
	unsigned n;
	char key[STUB_MAX][KV_KEY_MAX + 1];
	struct kv_value val[STUB_MAX];
	/* what the last kv_set / kv_set_desc was handed */
	uint8_t set_type;
	uint8_t set_val[KV_VALUE_MAX];
	uint32_t set_len;
	int set_desc_null;
	char last_desc[KV_DESC_MAX + 1];
	struct kv_info info;
	int info_rc;
} st;

static void stub_reset(void)
{
	memset(&st, 0, sizeof st);
	st.state = KV_STATE_READY;
}

static int stub_find(const char *key)
{
	unsigned i;

	for (i = 0; i < st.n; i++)
		if (strcmp(st.key[i], key) == 0)
			return (int)i;
	return -1;
}

static void stub_put(const char *key, uint8_t type, const void *v, uint32_t len,
                     const char *desc)
{
	int i = stub_find(key);

	if (i < 0) {
		i = (int)st.n++;
		strcpy(st.key[i], key);
	}
	st.val[i].type = type;
	st.val[i].len = len;
	memcpy(st.val[i].data, v, len);
	st.val[i].data[len] = '\0';
	strcpy(st.val[i].desc, desc ? desc : "");
}

int kv_state(void) { return st.state; }

const char *kv_state_str(void)
{
	return st.state == KV_STATE_READY ? "ready" : "TESTSTATE not ready";
}

int kv_set(const char *key, uint8_t type, const void *val, uint32_t len,
           const char *desc)
{
	st.calls++;
	st.set_type = type;
	st.set_len = len;
	memcpy(st.set_val, val, len);
	st.set_desc_null = (desc == NULL);
	if (st.rc)
		return st.rc;
	stub_put(key, type, val, len, desc);
	return KV_OK;
}

int kv_get(const char *key, struct kv_value *out)
{
	int i;

	st.calls++;
	if (st.rc)
		return st.rc;
	i = stub_find(key);
	if (i < 0)
		return KV_ERR_NOTFOUND;
	*out = st.val[i];
	return KV_OK;
}

int kv_set_desc(const char *key, const char *desc)
{
	int i;

	st.calls++;
	strcpy(st.last_desc, desc);
	if (st.rc)
		return st.rc;
	i = stub_find(key);
	if (i < 0)
		return KV_ERR_NOTFOUND;
	strcpy(st.val[i].desc, desc);
	return KV_OK;
}

int kv_del(const char *key)
{
	(void)key;
	st.calls++;
	return st.rc;
}

int kv_format(void)
{
	st.formats++;
	return st.rc;
}

int kv_get_info(struct kv_info *info)
{
	*info = st.info;
	return st.info_rc;
}

int kv_foreach(const char *prefix, kv_iter_cb cb, void *arg)
{
	unsigned i;

	st.calls++;
	if (st.rc)
		return st.rc;
	for (i = 0; i < st.n; i++) {
		if (prefix && strncmp(st.key[i], prefix, strlen(prefix)) != 0)
			continue;
		if (cb(st.key[i], &st.val[i], arg) != 0)
			break;
	}
	return KV_OK;
}

/* ---- harness --------------------------------------------------------------- */

CLI_BACKEND_DUMMY_DEFINE(tr0);
static struct cli_instance sh0;
static int failures;

static void reset(void)
{
	memset(&sh0, 0, sizeof sh0);
	sh0.tr = &tr0;
	tr0.sh = &sh0;
	strcpy(sh0.prompt, "> ");
	cli_dummy_clear_output(&tr0);
	cli_dummy_clear_rx(&tr0);
	cli_dummy_reset_stats(&tr0);
	cli_dummy_set_tx_fail(&tr0, 0);
	cli_dummy_set_tx_cap(&tr0, 0);
	cli_test_set_tx_wait_hook(NULL, NULL);
	cli_test_set_sleep_hook(NULL, NULL);
	stub_reset();
}

static void run(const char *line)
{
	cli_dummy_clear_output(&tr0);
	cli_dummy_inject(&tr0, line, strlen(line));
	cli_test_pump(&sh0);
}

static int has(const char *needle)
{
	return strstr(cli_dummy_output_str(&tr0), needle) != NULL;
}

static void expect(const char *what, int ok)
{
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		printf("       output was: \"%s\"\n", cli_dummy_output_str(&tr0));
		failures++;
	}
}

/* ---- tests ----------------------------------------------------------------- */

static void t_secret(void)
{
	uint8_t bad = 0u;

	reset();
	stub_put("wifi.psk", KV_TYPE_STR, "hunter22", 8u, "the passphrase");
	stub_put("wifi.ssid", KV_TYPE_STR, "home", 4u, NULL);
	run("kv list\r");
	expect("list hides the PSK's value, shows its length and description",
	       has("  wifi.psk               str   (hidden, 8 B)   -- the passphrase\r\n") &&
	       !has("hunter22"));
	expect("list shows other values",
	       has("  wifi.ssid              str   home\r\n") && has("2 keys\r\n"));
	run("kv list wifi.s\r");
	expect("a prefix narrows, and one key is singular",
	       has("wifi.ssid") && !has("wifi.psk") && has("1 key\r\n"));
	run("kv get wifi.psk\r");
	expect("get names the key and prints the value",
	       has("wifi.psk (str) = hunter22\r\n  the passphrase\r\n"));

	/* An unreadable secret is shown as unreadable, not as hidden. */
	reset();
	stub_put("wifi.psk", KV_TYPE_INVALID, &bad, 0u, NULL);
	run("kv list\r");
	expect("an unreadable PSK says so",
	       has("  wifi.psk               ?     <unreadable record>\r\n") &&
	       !has("hidden"));
	/* Only that key: a near name is not a secret. */
	reset();
	stub_put("wifi.psk2", KV_TYPE_STR, "visible", 7u, NULL);
	stub_put("wifi.ps", KV_TYPE_STR, "shown", 5u, NULL);
	run("kv list\r");
	expect("the table matches whole keys only",
	       has("visible") && has("shown") && !has("hidden"));
}

static void t_set(void)
{
	reset();
	run("kv set net.mode dhcp\r");
	expect("str is the default type",
	       st.set_type == KV_TYPE_STR && st.set_len == 4u &&
	       memcmp(st.set_val, "dhcp", 4) == 0 && st.set_desc_null &&
	       has("net.mode (str) = 4 B\r\n"));
	run("kv set net.shell.port 0x1F90 u32\r");
	expect("u32 is stored little-endian",
	       st.set_type == KV_TYPE_U32 && st.set_len == 4u &&
	       st.set_val[0] == 0x90 && st.set_val[1] == 0x1F &&
	       st.set_val[2] == 0 && st.set_val[3] == 0 &&
	       has("net.shell.port (u32) = 4 B\r\n"));
	run("kv set wifi.enable on bool\r");
	expect("bool on is 1", st.set_type == KV_TYPE_BOOL && st.set_len == 1u &&
	       st.set_val[0] == 1u);
	run("kv set wifi.enable no bool\r");
	expect("bool no is 0", st.set_val[0] == 0u);
	run("kv set x 0xA0b1 bytes\r");
	expect("bytes from hex, 0x optional",
	       st.set_type == KV_TYPE_BYTES && st.set_len == 2u &&
	       st.set_val[0] == 0xA0 && st.set_val[1] == 0xB1 && has("x (bytes) = 2 B\r\n"));

	st.calls = 0;
	run("kv set x maybe bool\r");
	expect("a bad bool is refused before the store",
	       has("kv: expected true/false (or 1/0, on/off, yes/no)\r\n") && st.calls == 0);
	run("kv set x 12x u32\r");
	expect("a bad number is refused",
	       has("kv: not a number (decimal or 0x hex)\r\n") && st.calls == 0);
	run("kv set x abc bytes\r");
	expect("odd hex is refused",
	       has("kv: expected an even number of hex digits (max 256 bytes)\r\n") &&
	       st.calls == 0);
	run("kv set x zz bytes\r");
	expect("a bad hex digit is refused", has("kv: bad hex digit\r\n") && st.calls == 0);
	run("kv set x 1 float\r");
	expect("an unknown type is refused",
	       has("kv: unknown type 'float' (str/u32/bool/bytes)\r\n") && st.calls == 0);
}

static void t_desc(void)
{
	char line[128];

	reset();
	stub_put("net.mode", KV_TYPE_STR, "dhcp", 4u, NULL);
	run("kv desc net.mode dhcp / static / manual\r");
	expect("desc joins its words with spaces",
	       strcmp(st.last_desc, "dhcp / static / manual") == 0 &&
	       has("net.mode: dhcp / static / manual\r\n"));
	st.calls = 0;
	strcpy(line, "kv desc net.mode ");
	memset(line + 17, 'd', 60);
	strcpy(line + 77, " dddd\r");     /* 60 + 1 + 4 = 65 */
	run(line);
	expect("a 65-character description is refused",
	       has("kv: description must be at most 64 characters\r\n") && st.calls == 0);
	st.rc = KV_ERR_NOTFOUND;
	run("kv desc no.such words\r");
	expect("a missing key says so", has("kv: no such key\r\n"));
}

static void t_gate_and_errors(void)
{
	static const struct { int rc; const char *line; } r[] = {
		{ KV_ERR_PARAM,    "kv: bad key or value\r\n" },
		{ KV_ERR_STATE,    "kv: store not ready\r\n" },
		{ KV_ERR_BUSY,     "kv: store busy\r\n" },
		{ KV_ERR_IO,       "kv: flash I/O failed\r\n" },
		{ KV_ERR_NOTFOUND, "kv: no such key\r\n" },
		{ KV_ERR_FULL,     "kv: partition full\r\n" },
		{ KV_ERR_TRUNC,    "kv: value longer than the buffer\r\n" },
		{ KV_ERR_FORMAT,   "kv: stored record is unreadable\r\n" },
		{ KV_ERR_TYPE,     "kv: value is not of the expected type\r\n" },
		{ -42,             "kv: unknown error\r\n" },
	};
	static const char *const data[] = {
		"kv list\r", "kv get a\r", "kv set a b\r", "kv desc a b\r", "kv del a\r",
	};
	unsigned i;

	reset();
	st.state = KV_STATE_CORRUPT;
	for (i = 0; i < sizeof data / sizeof data[0]; i++) {
		run(data[i]);
		expect(data[i], has("kv: TESTSTATE not ready\r\n") && st.calls == 0);
	}

	reset();
	for (i = 0; i < sizeof r / sizeof r[0]; i++) {
		st.rc = r[i].rc;
		run("kv get a\r");
		expect(r[i].line, has(r[i].line));
	}
	st.rc = KV_ERR_BUSY;
	run("kv list\r");
	expect("list failure", has("kv: list failed: store busy\r\n"));
	st.rc = KV_OK;
	run("kv del a\r");
	expect("del", has("deleted a\r\n"));
}

static void t_show(void)
{
	uint8_t u[4] = { 0x2A, 0, 0, 0 }, b = 1u, raw[3] = { 0x00, 0xAB, 0x7F };

	reset();
	stub_put("s", KV_TYPE_STR, "a\x01" "b\x7f", 4u, NULL);
	stub_put("u", KV_TYPE_U32, u, 4u, NULL);
	stub_put("b", KV_TYPE_BOOL, &b, 1u, NULL);
	stub_put("y", KV_TYPE_BYTES, raw, 3u, NULL);
	run("kv get s\r");
	expect("control bytes in a string are escaped", has("s (str) = a\\x01b\\x7F\r\n"));
	run("kv get u\r");
	expect("u32 in decimal and hex", has("u (u32) = 42 (0x2A)\r\n"));
	run("kv get b\r");
	expect("bool", has("b (bool) = true\r\n"));
	run("kv get y\r");
	expect("bytes in hex", has("y (bytes) = 00AB7F\r\n"));
}

static void t_info_format(void)
{
	reset();
	st.info.part_offset = 0x0u;
	st.info.part_size = 1024u * 1024u;
	st.info.sec_size = 4096u;
	st.info.kv_count = 3u;
	st.info.value_bytes = 40u;
	st.info.obj_bytes = 10486u;
	run("kv info\r");
	expect("info: the board's medium and the numbers",
	       has("KV store (FlashDB on the TESTMEDIUM):\r\n") &&
	       has("  state     : ready\r\n") &&
	       has("  partition : offset 0x000000, 1024 KB, 4096 B sectors\r\n") &&
	       has("  keys      : 3\r\n") && has("  values    : 40 B\r\n") &&
	       has("  on flash  : 10486 B of 1048576 B (1%)\r\n"));
	st.info_rc = KV_ERR_BUSY;
	run("kv info\r");
	expect("info: usage unavailable, no zeros",
	       has("  usage     : unavailable (store busy)\r\n") && !has("keys"));

	reset();
	st.state = KV_STATE_CORRUPT;     /* format is the way out: no gate */
	run("kv format\r");
	expect("format needs yes",
	       has("kv: this ERASES every stored setting -- confirm with `kv format yes`\r\n") &&
	       st.formats == 0);
	run("kv format no\r");
	expect("only yes", st.formats == 0);
	run("kv format yes\r");
	expect("format: the board's notice, then done",
	       has("TESTFORMAT erasing -- a while...\r\nformatted -- the store is empty\r\n") &&
	       st.formats == 1);
	st.rc = KV_ERR_IO;
	run("kv format yes\r");
	expect("format failure", has("kv: format failed: flash I/O failed\r\n"));
}

int main(void)
{
	printf("test_kv_cmd:\n");
	t_secret();
	t_set();
	t_desc();
	t_gate_and_errors();
	t_show();
	t_info_format();
	if (failures != 0) {
		printf("test_kv_cmd: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_kv_cmd: all passed\n");
	return 0;
}
