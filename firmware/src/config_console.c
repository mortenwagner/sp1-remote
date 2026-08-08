/* Line-based JSON console for editing the control surface.
 *
 * Shape borrowed from bnjreece/feldd-sp1-firmware: one JSON object per line
 * in, one per line out. It is deliberately plain enough to drive by hand in
 * `screen` before a browser is involved, which is how it was debugged.
 *
 * Requests:
 *   {"t":"prof"}                        read the whole mapping
 *   {"t":"set","f":0,"cc":20,"ch":1}    set a fader's CC and/or channel
 *   {"t":"set","b":1,"cc":105,"ch":0}   set a button's CC and/or channel
 *   {"t":"save"}                        persist to flash
 *   {"t":"default"}                     back to the compiled-in mapping
 *   {"t":"quiet","on":1}                stop the diagnostic stream
 *
 * Replies always start with {"t":"..._r"} or {"t":"ok"} / {"t":"err"}, so a
 * host never has to guess whether a command applied.
 *
 * There is no JSON library here on purpose. The grammar is a handful of
 * integer fields, so a 40-line scanner is smaller and more predictable than
 * a parser, and it cannot fail in interesting ways on a byte stream shared
 * with printk diagnostics.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <string.h>
#include <stdlib.h>

#include "config_console.h"
#include "profile.h"
#include "presets.h"

#define LINE_CAP 192

static const struct device *const con = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static char line[LINE_CAP];
static int  line_len;

bool g_diag_quiet;                 /* main.c honours this for its diag line */

extern profile_t g_profile;        /* the live mapping, owned by main.c */

/* Find "key": and return its integer value, or `missing` when absent. Only
 * integers appear in this protocol, so no string or float handling. */
static int jint(const char *s, const char *key, int missing)
{
	char pat[16];
	int n = snprintk(pat, sizeof(pat), "\"%s\"", key);

	if (n <= 0) {
		return missing;
	}
	const char *p = strstr(s, pat);

	if (p == NULL) {
		return missing;
	}
	p += n;
	while (*p == ' ' || *p == ':') {
		p++;
	}
	if (*p == 't') {                   /* true */
		return 1;
	}
	if (*p == 'f') {                   /* false */
		return 0;
	}
	if (*p != '-' && (*p < '0' || *p > '9')) {
		return missing;
	}
	return (int)strtol(p, NULL, 10);
}

static bool jhas(const char *s, const char *key)
{
	char pat[16];

	snprintk(pat, sizeof(pat), "\"%s\"", key);
	return strstr(s, pat) != NULL;
}

static void emit(const char *s)
{
	for (const char *p = s; *p; p++) {
		uart_poll_out(con, (unsigned char)*p);
	}
	uart_poll_out(con, '\n');
}

static void reply_profile(void)
{
	char buf[LINE_CAP];
	int  n;

	/* Split across several lines: one object per control keeps every line
	 * comfortably under the buffer and makes the stream readable by eye. */
	for (int f = 0; f < PROFILE_NUM_FADERS; f++) {
		n = snprintk(buf, sizeof(buf),
			     "{\"t\":\"f_r\",\"i\":%d,\"cc\":%u,\"ch\":%u}",
			     f, g_profile.fader[f].cc, g_profile.fader[f].channel);
		if (n > 0) {
			emit(buf);
		}
	}
	for (int b = 0; b < PROFILE_NUM_BUTTONS; b++) {
		const button_cfg_t *c = &g_profile.button[b];

		n = snprintk(buf, sizeof(buf),
			     "{\"t\":\"b_r\",\"i\":%d,\"mode\":%d,\"cc\":%u,"
			     "\"ch\":%u,\"on\":%u,\"off\":%u,\"n\":%u,"
			     "\"st\":[%u,%u,%u,%u],\"init\":%u,\"slot\":%u}",
			     b, (int)c->mode, c->cc, c->channel, c->on_value,
			     c->off_value, c->n_steps, c->steps[0], c->steps[1],
			     c->steps[2], c->steps[3], c->init_step,
			     c->preset_slot);
		if (n > 0) {
			emit(buf);
		}
	}
	emit("{\"t\":\"prof_end\"}");
}

static void handle(const char *s)
{
	if (strstr(s, "\"prof\"") != NULL) {
		reply_profile();
		return;
	}

	if (strstr(s, "\"quiet\"") != NULL) {
		g_diag_quiet = jint(s, "on", 1) != 0;
		emit(g_diag_quiet ? "{\"t\":\"ok\",\"quiet\":1}"
				  : "{\"t\":\"ok\",\"quiet\":0}");
		return;
	}

	if (strstr(s, "\"default\"") != NULL) {
		uint8_t cap_len = g_profile.preset_capture_len;

		g_profile = profile_popgoblin_default;
		g_profile.preset_capture_len = cap_len;
		emit("{\"t\":\"ok\",\"default\":1}");
		return;
	}

	if (strstr(s, "\"save\"") != NULL) {
		bool ok = profile_store_save(&g_profile);

		emit(ok ? "{\"t\":\"ok\",\"saved\":1}"
			: "{\"t\":\"err\",\"save\":0}");
		return;
	}

	if (strstr(s, "\"set\"") != NULL) {
		int cc = jint(s, "cc", -1);
		int ch = jint(s, "ch", -1);

		if (cc > 127 || ch > 15) {
			emit("{\"t\":\"err\",\"why\":\"range\"}");
			return;
		}
		if (jhas(s, "f")) {
			int i = jint(s, "f", -1);

			if (i < 0 || i >= PROFILE_NUM_FADERS) {
				emit("{\"t\":\"err\",\"why\":\"index\"}");
				return;
			}
			if (cc >= 0) {
				g_profile.fader[i].cc = (uint8_t)cc;
			}
			if (ch >= 0) {
				g_profile.fader[i].channel = (uint8_t)ch;
			}
			emit("{\"t\":\"ok\"}");
			return;
		}
		if (jhas(s, "b")) {
			int i = jint(s, "b", -1);

			if (i < 0 || i >= PROFILE_NUM_BUTTONS) {
				emit("{\"t\":\"err\",\"why\":\"index\"}");
				return;
			}
			if (cc >= 0) {
				g_profile.button[i].cc = (uint8_t)cc;
			}
			if (ch >= 0) {
				g_profile.button[i].channel = (uint8_t)ch;
			}
			emit("{\"t\":\"ok\"}");
			return;
		}
		emit("{\"t\":\"err\",\"why\":\"target\"}");
		return;
	}

	emit("{\"t\":\"err\",\"why\":\"unknown\"}");
}

void config_console_poll(void)
{
	unsigned char c;

	if (!device_is_ready(con)) {
		return;
	}
	/* Non-blocking: drain whatever has arrived and return. Called from the
	 * control loop, so it must never wait. */
	while (uart_poll_in(con, &c) == 0) {
		if (c == '\r') {
			continue;
		}
		if (c == '\n') {
			line[line_len] = '\0';
			if (line_len > 0) {
				handle(line);
			}
			line_len = 0;
			continue;
		}
		if (line_len < LINE_CAP - 1) {
			line[line_len++] = (char)c;
		} else {
			line_len = 0;      /* overlong: resync on the next line */
		}
	}
}
