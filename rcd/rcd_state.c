/*
 * rcd_state.c -- see rcd_state.h
 */

#include "rcd_state.h"
#include "rcd.h"
#include "rcd_config.h"
#include "rcd_ipc.h"
#include "rcd_proto.h"
#include "rcd_schema.h"

#include <rss_common.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *codec_name(int c)
{
	/* Mirrors rss_codec_t in raptor-hal/include/raptor_hal.h. Duplicated
	 * rather than included: rcd deliberately does not link the HAL, and
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
 * the value reads 0 rather than being absent -- indistinguishable from a real
 * zero from out here. The controls are honest about that by being controls:
 * they say what the ISP reports, which on such a platform is what it will keep
 * reporting whatever is written.
 */
static void collect_rvd_isp(cJSON *state)
{
	static const char *const isp_keys[] = {
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

	cJSON *resp = rcd_ask_json("rvd", "{\"cmd\":\"get-isp\"}");
	if (!resp)
		return;

	cJSON *o = cJSON_AddObjectToObject(state, "image");
	if (o) {
		for (int i = 0; isp_keys[i]; i++)
			cJSON_AddNumberToObject(o, isp_keys[i], json_int(resp, isp_keys[i], 0));

		/*
		 * Which of them this platform can actually set. Every key above
		 * reads back a number whether or not the ISP has the block, so
		 * without this a subscriber cannot tell a real 0 from an absent
		 * one -- and a control offered for a block that is not there is
		 * one that does nothing, quietly.
		 */
		const cJSON *set = cJSON_GetObjectItemCaseSensitive(resp, "settable");
		if (cJSON_IsString(set) && set->valuestring)
			cJSON_AddStringToObject(o, "settable", set->valuestring);
	}

	cJSON_Delete(resp);
}

/* rvd: per-stream encoder state. Only the encoded video channels are exposed;
 * the JPEG channels carry no bitrate or GOP worth showing. */
static void collect_rvd(cJSON *state)
{
	cJSON *resp = rcd_ask_json("rvd", "{\"cmd\":\"status\"}");
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

	/*
	 * Whether the camera encodes JPEG at all, which is what decides if
	 * there is a picture to offer. rvd lists its JPEG channels alongside
	 * the video ones and they vanish from the list when [jpeg] is off, so
	 * this is the camera's own answer rather than a config key someone has
	 * to keep in agreement with it.
	 */
	int jpeg_channels = 0;

	const cJSON *streams = cJSON_GetObjectItemCaseSensitive(resp, "streams");
	if (cJSON_IsArray(streams)) {
		const cJSON *s = NULL;
		cJSON_ArrayForEach(s, streams)
		{
			int codec = json_int(s, "codec", -1);
			if (codec == 2 || codec == 3)
				jpeg_channels++;
			if (codec != 0 && codec != 1)
				continue; /* skip JPEG/MJPEG channels */

			/* Bound the channel so the key below cannot truncate,
			 * and because a channel outside this range is not
			 * something a client has a control for. */
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

	cJSON *j = cJSON_AddObjectToObject(state, "jpeg");
	if (j)
		cJSON_AddNumberToObject(j, "channels", jpeg_channels);

	cJSON_Delete(resp);
}

/* rsd: viewer count, listening port and who has to log in. */
static void collect_rsd(cJSON *state)
{
	cJSON *resp = rcd_ask_json("rsd", "{\"cmd\":\"config-show\"}");
	if (!resp)
		return;

	const cJSON *cfg = cJSON_GetObjectItemCaseSensitive(resp, "config");
	if (cJSON_IsObject(cfg)) {
		cJSON *o = cJSON_AddObjectToObject(state, "rtsp");
		if (o) {
			cJSON_AddNumberToObject(o, "clients", json_int(cfg, "clients", 0));
			cJSON_AddNumberToObject(o, "port", json_int(cfg, "port", 0));

			/* The account, so a text control shows what is set
			 * rather than a blank that could mean either. The
			 * password has no readback at any layer: the control
			 * that sets it always shows empty, which is the honest
			 * rendering of a value nothing here can see. */
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
 * rhd: the HTTP endpoint a picture is fetched from.
 *
 * Asked for rather than read out of [http], because the config says what rhd
 * was started with and this says what it is doing. A port change restarts rhd,
 * so the two agree in the end; between the write and the apply they do not,
 * and this is the half that is true.
 *
 * The address is deliberately absent. rcd has no idea which of the camera's
 * interfaces a given client can reach it on, and a URL built from a guess is
 * worse than none -- so the port is reported and whoever holds a socket to the
 * client builds the URL.
 */
static void collect_rhd(cJSON *state)
{
	cJSON *resp = rcd_ask_json("rhd", "{\"cmd\":\"status\"}");
	if (!resp)
		return;

	cJSON *o = cJSON_AddObjectToObject(state, "http");
	if (o) {
		cJSON_AddNumberToObject(o, "port", json_int(resp, "port", 0));
		cJSON_AddNumberToObject(o, "clients", json_int(resp, "clients", 0));
		cJSON_AddNumberToObject(o, "mjpeg", json_int(resp, "mjpeg", 0));
		cJSON_AddBoolToObject(o, "tls",
				      cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "tls")));
	}

	cJSON_Delete(resp);
}

/* ric: day/night decision plus the exposure readings behind it. */
static void collect_ric(cJSON *state)
{
	cJSON *resp = rcd_ask_json("ric", "{\"cmd\":\"status\"}");
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
	 * the file -- a threshold changed live has not been saved yet, and a
	 * control should show what is deciding rather than what was configured.
	 */
	static const char *const th_keys[] = {
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

	cJSON *th = rcd_ask_json("ric", "{\"cmd\":\"get-thresholds\"}");
	if (!th)
		return;

	const cJSON *trig = cJSON_GetObjectItemCaseSensitive(th, "trigger");
	if (cJSON_IsString(trig))
		cJSON_AddStringToObject(o, "trigger", trig->valuestring);
	for (int i = 0; th_keys[i]; i++)
		cJSON_AddNumberToObject(o, th_keys[i], json_int(th, th_keys[i], 0));

	cJSON_Delete(th);
}

/*
 * rad: the levels the write path can move, so a control reads back the value
 * the hardware is actually at rather than the one last commanded.
 */
static void collect_rad(cJSON *state)
{
	cJSON *resp = rcd_ask_json("rad", "{\"cmd\":\"status\"}");
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
	cJSON *resp = rcd_ask_json("rod", "{\"cmd\":\"config-show\"}");
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

cJSON *rcd_cmd_state(rcd_state_t *st, const cJSON *root)
{
	(void)root;

	cJSON *resp = rcd_ok();
	if (!resp)
		return NULL;

	/* Liveness first: the pidfile check is cheap and tells us which
	 * sockets are worth a round trip. */
	bool alive[RCD_D_COUNT];
	int up_count = 0;
	cJSON *up = cJSON_AddObjectToObject(resp, "up");
	for (int i = 0; i < RCD_D_COUNT; i++) {
		const char *name = rcd_daemon_name((rcd_daemon_t)i);
		alive[i] = rss_daemon_check(name) > 0;
		if (alive[i])
			up_count++;
		if (up)
			cJSON_AddBoolToObject(up, name, alive[i]);
	}
	cJSON_AddNumberToObject(resp, "daemons_up", up_count);

	if (alive[RCD_D_RVD]) {
		collect_rvd(resp);
		collect_rvd_isp(resp);
	}
	if (alive[RCD_D_RSD])
		collect_rsd(resp);
	if (alive[RCD_D_RAD])
		collect_rad(resp);
	if (alive[RCD_D_ROD])
		collect_rod(resp);
	if (alive[RCD_D_RIC])
		collect_ric(resp);
	if (alive[RCD_D_RHD])
		collect_rhd(resp);

	collect_system(resp);

	/*
	 * A daemon that is not running is not running behind. Whenever it next
	 * starts -- by init, by an operator, or after a crash -- it reads the
	 * file as it is now, so there is nothing left for an apply to enact
	 * and a flag saying otherwise would outlive the condition it describes.
	 */
	for (int i = 0; i < RCD_D_COUNT; i++) {
		if (st->stale_daemon[i] && !alive[i])
			rcd_stale_clear(st, (rcd_daemon_t)i);
	}

	rcd_config_report_stale(st, resp);
	return resp;
}
