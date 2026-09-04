/*
 * rcd_state.c -- see rcd_state.h
 */

#include "rcd_state.h"
#include "rcd.h"
#include "rcd_config.h"
#include "rcd_guard.h"
#include "rcd_ipc.h"
#include "rcd_proto.h"
#include "rcd_schema.h"
#include "rcd_wifi.h"

#include <raptor_hal.h>
#include <rss_common.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The codec numbers come off rvd's wire, and rvd gets them from the HAL. They
 * used to be spelled here as 0, 1, 2, 3 beside a comment saying they mirrored
 * rss_codec_t -- which is a copy, and a copy of an enum is a copy that can
 * drift. Reordering the HAL's enum would have relabelled every stream on the
 * console with nothing failing to build.
 *
 * Naming them costs nothing: raptor_hal.h is standard headers and typedefs,
 * and it is already on rcd's include path. Including it is not linking the
 * HAL, which rcd still does not do.
 */
static const char *codec_name(int c)
{
	switch (c) {
	case RSS_CODEC_H264:
		return "h264";
	case RSS_CODEC_H265:
		return "h265";
	case RSS_CODEC_JPEG:
		return "jpeg";
	case RSS_CODEC_MJPEG:
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

/* And the same for a number, for the same reason and with a sharper edge:
 * every ISP knob's scale contains 0, so a fallback zero is not a marker a
 * subscriber can recognise but a position on the slider. */
static void copy_int(cJSON *dst, const cJSON *src, const char *key)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(src, key);
	if (cJSON_IsNumber(v))
		cJSON_AddNumberToObject(dst, key, cJSON_GetNumberValue(v));
}

/*
 * rvd: the ISP tuning, read back from the hardware rather than the config.
 *
 * A knob rvd has no reading for is absent from get-isp, and it has to stay
 * absent here. There is no number that means "no reading": 0 is a legal
 * position on all thirteen scales, and on Ingenic it is a destructive one --
 * brightness runs dark-to-light over 1..255 and 0 sits off the end of it,
 * blowing the picture to white. Filling the gap with a zero therefore does
 * not merely misreport the knob, it hands every client a plausible value to
 * echo back, and the first one that does clears the shot.
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

	cJSON *resp = rcd_ask_json("rvd", "get-isp");
	if (!resp)
		return;

	cJSON *o = cJSON_AddObjectToObject(state, "image");
	if (o) {
		for (int i = 0; isp_keys[i]; i++)
			copy_int(o, resp, isp_keys[i]);

		/*
		 * Which of them this platform can actually set, which is not
		 * what the presence of a reading says: on Ingenic every knob
		 * above is settable and most of them read back nothing, and a
		 * control offered for a block that is not there is one that
		 * does nothing, quietly.
		 */
		const cJSON *set = cJSON_GetObjectItemCaseSensitive(resp, "settable");
		if (cJSON_IsString(set) && set->valuestring)
			cJSON_AddStringToObject(o, "settable", set->valuestring);

		/*
		 * And which of them the ISP is currently running from the
		 * tuning file's own curve rather than from a value of raptor's.
		 * Those report the tuner's neutral, which is a real number and
		 * indistinguishable from a knob deliberately set there -- so
		 * the difference has to be carried separately or a subscriber
		 * showing "50" cannot say whether anyone chose it.
		 */
		const cJSON *au = cJSON_GetObjectItemCaseSensitive(resp, "auto");
		if (cJSON_IsString(au) && au->valuestring)
			cJSON_AddStringToObject(o, "auto", au->valuestring);

		/*
		 * The range each knob actually accepts on this camera, which is
		 * the hardware's and not a convention: brightness is 0..100 on
		 * one SoC and 0..255 on another, and 3DNR is eight levels. A
		 * subscriber drawing a control has no other source for it --
		 * this used to be a constant compiled into each client.
		 */
		const cJSON *caps = cJSON_GetObjectItemCaseSensitive(resp, "caps");
		if (cJSON_IsObject(caps))
			cJSON_AddItemToObject(o, "caps", cJSON_Duplicate(caps, 1));
	}

	cJSON_Delete(resp);
}

/* rvd: per-stream encoder state. Only the encoded video channels are exposed;
 * the JPEG channels carry no bitrate or GOP worth showing. */
static void collect_rvd(cJSON *state)
{
	cJSON *resp = rcd_ask_json("rvd", "status");
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
			if (codec == RSS_CODEC_JPEG || codec == RSS_CODEC_MJPEG)
				jpeg_channels++;
			if (codec != RSS_CODEC_H264 && codec != RSS_CODEC_H265)
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
	cJSON *resp = rcd_ask_json("rsd", "config-show");
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
	cJSON *resp = rcd_ask_json("rhd", "status");
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
	cJSON *resp = rcd_ask_json("ric", "status");
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

	cJSON *th = rcd_ask_json("ric", "get-thresholds");
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
	cJSON *resp = rcd_ask_json("rad", "status");
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
	cJSON *resp = rcd_ask_json("rod", "config-show");
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

/*
 * How much of this camera's RAM is left, from /proc/meminfo.
 *
 * MemAvailable rather than MemFree, and the difference is the whole point on a
 * camera: the frame buffers and the page cache make MemFree look alarming on a
 * box that is perfectly healthy, where MemAvailable is the kernel's own
 * estimate of what a new allocation could actually get. MemFree is the
 * fallback only for a kernel too old to publish it, and the client is told
 * which it got rather than left to guess -- an operator watching for a leak is
 * watching a number whose meaning has to be stable.
 *
 * Absent, like every other reading here, when the file cannot be read.
 */
static void collect_memory(cJSON *state)
{
	int len = 0;
	char *mi = rss_read_file("/proc/meminfo", &len);
	long total = 0, avail = 0, free_kb = 0;
	cJSON *o;

	if (!mi)
		return;

	for (const char *l = mi; *l;) {
		const char *nl = strchr(l, '\n');

		if (!strncmp(l, "MemTotal:", 9))
			total = strtol(l + 9, NULL, 10);
		else if (!strncmp(l, "MemAvailable:", 13))
			avail = strtol(l + 13, NULL, 10);
		else if (!strncmp(l, "MemFree:", 8))
			free_kb = strtol(l + 8, NULL, 10);
		if (!nl)
			break;
		l = nl + 1;
	}
	free(mi);

	if (total <= 0)
		return;
	o = cJSON_AddObjectToObject(state, "mem");
	if (!o)
		return;
	cJSON_AddNumberToObject(o, "total_kb", (double)total);
	cJSON_AddNumberToObject(o, "avail_kb", (double)(avail > 0 ? avail : free_kb));
	cJSON_AddStringToObject(o, "avail_from", avail > 0 ? "MemAvailable" : "MemFree");
}

static void collect_system(cJSON *state)
{
	int len = 0;
	char *up = rss_read_file("/proc/uptime", &len);
	if (up) {
		double secs = strtod(up, NULL);
		/* At the top level, where it has always been read from. */
		cJSON_AddNumberToObject(state, "uptime", (double)(long)secs);
		free(up);
	}
	collect_memory(state);

	/*
	 * Whether this camera still has to be told which network to join --
	 * the fact setup mode turns on, reported rather than inferred from a
	 * file somebody greps. See rcd_wifi.h.
	 *
	 * Always present, unlike every other object here, because those
	 * describe daemons that may not be running and this describes the
	 * camera. A client that finds it missing is talking to an older rcd,
	 * not to a camera with nothing to say.
	 */
	cJSON *o = cJSON_AddObjectToObject(state, "system");
	if (o)
		cJSON_AddBoolToObject(o, "provisioned", rcd_wifi_provisioned());
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
	bool cleared = false;
	for (int i = 0; i < RCD_D_COUNT; i++) {
		if (st->stale_daemon[i] && !alive[i]) {
			rcd_stale_clear(st, (rcd_daemon_t)i);
			cleared = true;
		}
	}

	/*
	 * Written out, not just dropped from memory. /run is where the drift
	 * record lives across an rcd restart, so leaving the file saying what
	 * this poll has just decided is over means the next rcd start reads
	 * the drift back and offers to enact it -- against a daemon that read
	 * the file on its own way up and is not behind at all.
	 *
	 * A read verb writing a file is worth saying out loud. What `state`
	 * reports is drawn from who answered, and this is the same discovery
	 * being recorded rather than a second one: the alternative is not a
	 * `state` that changes nothing, it is a `state` whose answer disagrees
	 * with the file it was derived from.
	 */
	if (cleared)
		rcd_stale_save(st);

	rcd_config_report_stale(st, resp);
	rcd_guard_report(st, resp);
	return resp;
}
