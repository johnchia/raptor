/*
 * rmq_poll.c -- Daemon discovery and state collection
 */

#include "rmq_poll.h"
#include "rmq.h"

#include <rss_common.h>
#include <rss_ipc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CTRL_TIMEOUT_MS 1000
#define RESP_MAX	8192

static const char *const daemon_names[RMQ_D_COUNT] = {
	[RMQ_D_RVD] = "rvd", [RMQ_D_RSD] = "rsd", [RMQ_D_RAD] = "rad",
	[RMQ_D_ROD] = "rod", [RMQ_D_RIC] = "ric", [RMQ_D_RMR] = "rmr",
	[RMQ_D_RMD] = "rmd", [RMQ_D_RHD] = "rhd", [RMQ_D_RWD] = "rwd",
};

const char *rmq_daemon_name(rmq_daemon_t d)
{
	return (d >= 0 && d < RMQ_D_COUNT) ? daemon_names[d] : "?";
}

rmq_daemon_t rmq_daemon_by_name(const char *name)
{
	for (int i = 0; name && i < RMQ_D_COUNT; i++) {
		if (strcmp(name, daemon_names[i]) == 0)
			return (rmq_daemon_t)i;
	}
	return RMQ_D_COUNT;
}

bool rmq_daemons_differ(const rmq_daemons_t *a, const rmq_daemons_t *b)
{
	for (int i = 0; i < RMQ_D_COUNT; i++) {
		if (a->up[i] != b->up[i])
			return true;
	}
	return false;
}

/*
 * Send a command to a daemon and parse the reply. Returns a cJSON object the
 * caller deletes, or NULL if the daemon is absent or answered unusably —
 * which are the same thing from here, and both simply mean "no state".
 */
static cJSON *ask(const char *daemon, const char *cmd_json)
{
	char sock[128];
	snprintf(sock, sizeof(sock), RSS_SOCK_FMT, daemon);

	char *resp = malloc(RESP_MAX);
	if (!resp)
		return NULL;

	int rc = rss_ctrl_send_command(sock, cmd_json, resp, RESP_MAX, CTRL_TIMEOUT_MS);
	if (rc < 0) {
		free(resp);
		return NULL;
	}

	cJSON *root = cJSON_Parse(resp);
	free(resp);
	return root;
}

static const char *codec_name(int c)
{
	/* Mirrors rss_codec_t in raptor-hal/include/raptor_hal.h. Duplicated
	 * rather than included: rmq deliberately does not link the HAL, and
	 * one enum is a smaller cost than that dependency. */
	switch (c) {
	case 0:
		return "h264";
	case 1:
		return "h265";
	case 2:
		return "jpeg";
	case 3:
		return "mjpeg";
	default:
		return "unknown";
	}
}

static int json_int(const cJSON *o, const char *key, int fallback)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
	return cJSON_IsNumber(v) ? (int)cJSON_GetNumberValue(v) : fallback;
}

/* Copies a bool across only when the daemon reported one: an absent key means
 * the feature is not built in, and a false would claim it is present and off. */
static void copy_bool(cJSON *dst, const cJSON *src, const char *key)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(src, key);
	if (cJSON_IsBool(v))
		cJSON_AddBoolToObject(dst, key, cJSON_IsTrue(v));
}

/*
 * rvd: the ISP tuning, read back from the hardware rather than the config.
 *
 * A platform that does not implement a block leaves its getter untouched, so
 * the value reads 0 rather than being absent — indistinguishable from a real
 * zero from out here. The controls are honest about that by being controls:
 * they say what the ISP reports, which on such a platform is what it will keep
 * reporting whatever is written.
 */
static void collect_rvd_isp(cJSON *state)
{
	static const char *const keys[] = {
		"brightness",
		"contrast",
		"saturation",
		"sharpness",
		"hue",
		"sinter",
		"temper",
		"ae_comp",
		"max_again",
		"max_dgain",
		"dpc_strength",
		"drc_strength",
		"defog_strength",
		"highlight_depress",
		"backlight_comp",
		"hflip",
		"vflip",
		NULL,
	};

	cJSON *resp = ask("rvd", "{\"cmd\":\"get-isp\"}");
	if (!resp)
		return;

	cJSON *o = cJSON_AddObjectToObject(state, "image");
	if (o) {
		for (int i = 0; keys[i]; i++)
			cJSON_AddNumberToObject(o, keys[i], json_int(resp, keys[i], 0));

		/*
		 * Which of them this platform can actually set. Every key above
		 * reads back a number whether or not the ISP has the block, so
		 * without this a subscriber cannot tell a real 0 from an absent
		 * one — and a control offered for a block that is not there is
		 * one that does nothing, quietly.
		 */
		const cJSON *set = cJSON_GetObjectItemCaseSensitive(resp, "settable");
		if (cJSON_IsString(set) && set->valuestring)
			cJSON_AddStringToObject(o, "settable", set->valuestring);
	}

	cJSON_Delete(resp);
}

/* rvd: per-stream encoder state. Only the encoded video channels are
 * exposed; the JPEG channels carry no bitrate or GOP worth showing. */
static void collect_rvd(cJSON *state)
{
	cJSON *resp = ask("rvd", "{\"cmd\":\"status\"}");
	if (!resp)
		return;

	/* What the sensor delivers, which is the ceiling every stream size sits
	 * under and the only thing on the camera that describes the geometry it
	 * can produce. Absent when rvd could not determine it. */
	int sw = json_int(resp, "sensor_w", 0);
	int sh = json_int(resp, "sensor_h", 0);
	if (sw > 0 && sh > 0) {
		cJSON *o = cJSON_AddObjectToObject(state, "sensor");
		if (o) {
			char res[32];
			snprintf(res, sizeof(res), "%dx%d", sw, sh);
			cJSON_AddStringToObject(o, "resolution", res);
			cJSON_AddNumberToObject(o, "width", sw);
			cJSON_AddNumberToObject(o, "height", sh);
		}
	}

	const cJSON *streams = cJSON_GetObjectItemCaseSensitive(resp, "streams");
	if (cJSON_IsArray(streams)) {
		const cJSON *s = NULL;
		cJSON_ArrayForEach(s, streams)
		{
			int codec = json_int(s, "codec", -1);
			if (codec != 0 && codec != 1)
				continue; /* skip JPEG/MJPEG channels */

			/* Bound the channel so the key below cannot truncate,
			 * and because a channel outside this range is not
			 * something the entity table has a component for. */
			int chn = json_int(s, "chn", -1);
			if (chn < 0 || chn > 99)
				continue;

			char key[16];
			snprintf(key, sizeof(key), "stream%d", chn);

			cJSON *o = cJSON_AddObjectToObject(state, key);
			if (!o)
				continue;

			int w = json_int(s, "w", 0);
			int h = json_int(s, "h", 0);
			char res[32];
			snprintf(res, sizeof(res), "%dx%d", w, h);

			cJSON_AddStringToObject(o, "resolution", res);
			cJSON_AddStringToObject(o, "codec", codec_name(codec));
			cJSON_AddNumberToObject(o, "bitrate", json_int(s, "bitrate", 0));
			cJSON_AddNumberToObject(o, "avg_bitrate", json_int(s, "avg_bitrate", 0));
			cJSON_AddNumberToObject(o, "fps", json_int(s, "fps", 0));
			cJSON_AddNumberToObject(o, "gop", json_int(s, "gop", 0));
		}
	}

	cJSON_Delete(resp);
}

/* rsd: viewer count, listening port and who has to log in. */
static void collect_rsd(cJSON *state)
{
	cJSON *resp = ask("rsd", "{\"cmd\":\"config-show\"}");
	if (!resp)
		return;

	const cJSON *cfg = cJSON_GetObjectItemCaseSensitive(resp, "config");
	if (cJSON_IsObject(cfg)) {
		cJSON *o = cJSON_AddObjectToObject(state, "rtsp");
		if (o) {
			cJSON_AddNumberToObject(o, "clients", json_int(cfg, "clients", 0));
			cJSON_AddNumberToObject(o, "port", json_int(cfg, "port", 0));

			/* The account, so the text control shows what is set
			 * rather than a blank that could mean either. The
			 * password has no readback at any layer: the control
			 * that sets it always shows empty, which is the honest
			 * rendering of a value this bridge cannot see. */
			const cJSON *u = cJSON_GetObjectItemCaseSensitive(cfg, "username");
			cJSON_AddStringToObject(o, "username",
						cJSON_IsString(u) && u->valuestring ? u->valuestring
										    : "");
			cJSON_AddStringToObject(o, "password", "");
			cJSON_AddBoolToObject(
				o, "auth",
				cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cfg, "auth")));
		}
	}

	cJSON_Delete(resp);
}

/*
 * rhd: the HTTP endpoint the picture is fetched from.
 *
 * The listener is cached on the bridge as well as reported, because every URL
 * rmq publishes is built from it — asked for rather than read out of [http],
 * since the config says what rhd was started with and this says what it is
 * doing. A port change restarts rhd, so the two agree in the end; between the
 * write and the restart they do not, and this is the half that is true.
 */
static void collect_rhd(struct rmq_state *st, cJSON *state)
{
	cJSON *resp = ask("rhd", "{\"cmd\":\"status\"}");
	if (!resp)
		return;

	st->http_port = json_int(resp, "port", 0);
	st->http_tls = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "tls"));

	cJSON *o = cJSON_AddObjectToObject(state, "http");
	if (o) {
		cJSON_AddNumberToObject(o, "port", st->http_port);
		cJSON_AddNumberToObject(o, "clients", json_int(resp, "clients", 0));
		cJSON_AddNumberToObject(o, "mjpeg", json_int(resp, "mjpeg", 0));

		/*
		 * The stream URL, spelled out. Home Assistant cannot be given
		 * an MJPEG camera over discovery — its MJPEG integration is
		 * added by hand and asks for the URL — so the useful thing the
		 * bridge can do is say what to paste, rather than leave it to
		 * be worked out from an address the camera knows and the
		 * person reading does not.
		 */
		char path[32], url[RMQ_URL_MAX];
		snprintf(path, sizeof(path), "/mjpeg?stream=%d", st->snapshot_stream);
		if (rmq_snapshot_url(st, path, false, url, sizeof(url)) == 0)
			cJSON_AddStringToObject(o, "mjpeg_url", url);
	}

	cJSON_Delete(resp);
}

/* ric: day/night decision plus the exposure readings behind it. */
static void collect_ric(cJSON *state)
{
	cJSON *resp = ask("ric", "{\"cmd\":\"status\"}");
	if (!resp)
		return;

	cJSON *o = cJSON_AddObjectToObject(state, "ir");
	if (!o) {
		cJSON_Delete(resp);
		return;
	}

	const cJSON *mode = cJSON_GetObjectItemCaseSensitive(resp, "mode");
	const cJSON *dnstate = cJSON_GetObjectItemCaseSensitive(resp, "state");
	if (cJSON_IsString(mode))
		cJSON_AddStringToObject(o, "mode", mode->valuestring);
	if (cJSON_IsString(dnstate))
		cJSON_AddStringToObject(o, "state", dnstate->valuestring);

	const cJSON *exp = cJSON_GetObjectItemCaseSensitive(resp, "exposure");
	if (cJSON_IsObject(exp)) {
		cJSON_AddNumberToObject(o, "total_gain", json_int(exp, "total_gain", 0));
		cJSON_AddNumberToObject(o, "exposure_us", json_int(exp, "exposure_us", 0));
		cJSON_AddNumberToObject(o, "ae_luma", json_int(exp, "ae_luma", 0));
	}

	cJSON_Delete(resp);

	/*
	 * The tuning behind the decision, from ric's own settings rather than
	 * the file — a threshold changed live has not been saved yet, and the
	 * control should show what is deciding rather than what was configured.
	 * ric reports no reading for the GPIO pins or the ADC channel, so
	 * those controls have nothing to display and do not claim to.
	 */
	static const char *const keys[] = {
		"night_luma",
		"night_gain",
		"day_gain_pct",
		"night_threshold",
		"day_threshold",
		"hysteresis_sec",
		"poll_interval_ms",
		"photo_ev_night",
		"photo_ev_deep",
		"photo_ev_day",
		"photo_rgain_rec",
		"photo_bgain_rec",
		NULL,
	};

	cJSON *th = ask("ric", "{\"cmd\":\"get-thresholds\"}");
	if (!th)
		return;

	const cJSON *trig = cJSON_GetObjectItemCaseSensitive(th, "trigger");
	if (cJSON_IsString(trig))
		cJSON_AddStringToObject(o, "trigger", trig->valuestring);
	for (int i = 0; keys[i]; i++)
		cJSON_AddNumberToObject(o, keys[i], json_int(th, keys[i], 0));

	cJSON_Delete(th);
}

/*
 * rad: the levels the write path can move, so a control reads back the value
 * the hardware is actually at rather than the one last commanded.
 */
static void collect_rad(cJSON *state)
{
	cJSON *resp = ask("rad", "{\"cmd\":\"status\"}");
	if (!resp)
		return;

	cJSON *o = cJSON_AddObjectToObject(state, "audio");
	if (!o) {
		cJSON_Delete(resp);
		return;
	}

	const cJSON *codec = cJSON_GetObjectItemCaseSensitive(resp, "codec");
	if (cJSON_IsString(codec))
		cJSON_AddStringToObject(o, "codec", codec->valuestring);
	cJSON_AddNumberToObject(o, "sample_rate", json_int(resp, "sample_rate", 0));
	cJSON_AddNumberToObject(o, "volume", json_int(resp, "volume", 0));
	cJSON_AddNumberToObject(o, "gain", json_int(resp, "gain", 0));
	copy_bool(o, resp, "muted");

	/* The speaker keys are absent unless audio output is configured, and
	 * the effects keys unless the build carries them. */
	copy_bool(o, resp, "ao_enabled");
	if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "ao_enabled"))) {
		cJSON_AddNumberToObject(o, "ao_volume", json_int(resp, "ao_volume", 0));
		cJSON_AddNumberToObject(o, "ao_gain", json_int(resp, "ao_gain", 0));
	}
	copy_bool(o, resp, "aec");
	copy_bool(o, resp, "ns");
	copy_bool(o, resp, "hpf");
	copy_bool(o, resp, "agc");

	cJSON_Delete(resp);
}

/* rod: whether the overlay is being drawn, and how much of it there is. */
static void collect_rod(cJSON *state)
{
	cJSON *resp = ask("rod", "{\"cmd\":\"config-show\"}");
	if (!resp)
		return;

	const cJSON *cfg = cJSON_GetObjectItemCaseSensitive(resp, "config");
	if (cJSON_IsObject(cfg)) {
		cJSON *o = cJSON_AddObjectToObject(state, "osd");
		if (o) {
			copy_bool(o, cfg, "enabled");
			cJSON_AddNumberToObject(o, "elements", json_int(cfg, "elements", 0));
			cJSON_AddNumberToObject(o, "font_size", json_int(cfg, "font_size", 0));
		}
	}

	cJSON_Delete(resp);
}

static void collect_system(cJSON *state)
{
	int len = 0;
	char *up = rss_read_file("/proc/uptime", &len);
	if (up) {
		double secs = strtod(up, NULL);
		cJSON_AddNumberToObject(state, "uptime", (double)(long)secs);
		free(up);
	}
}

cJSON *rmq_poll_state(struct rmq_state *st, rmq_daemons_t *out)
{
	cJSON *state = cJSON_CreateObject();
	if (!state)
		return NULL;

	memset(out, 0, sizeof(*out));

	/* Liveness first: the pidfile check is cheap and tells us which
	 * sockets are worth a round trip. */
	cJSON *up = cJSON_AddObjectToObject(state, "up");
	for (int i = 0; i < RMQ_D_COUNT; i++) {
		bool alive = rss_daemon_check(daemon_names[i]) > 0;
		out->up[i] = alive;
		if (alive)
			out->up_count++;
		if (up)
			cJSON_AddBoolToObject(up, daemon_names[i], alive);
	}
	cJSON_AddNumberToObject(state, "daemons_up", out->up_count);

	if (out->up[RMQ_D_RVD]) {
		collect_rvd(state);
		collect_rvd_isp(state);
	}
	if (out->up[RMQ_D_RSD])
		collect_rsd(state);
	if (out->up[RMQ_D_RAD])
		collect_rad(state);
	if (out->up[RMQ_D_ROD])
		collect_rod(state);
	if (out->up[RMQ_D_RIC])
		collect_ric(state);
	if (out->up[RMQ_D_RHD])
		collect_rhd(st, state);
	else
		st->http_port = 0;

	collect_system(state);

	return state;
}
