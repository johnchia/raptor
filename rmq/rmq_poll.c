/*
 * rmq_poll.c -- Daemon discovery and state collection
 */

#include "rmq_poll.h"

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

/* rvd: per-stream encoder state. Only the encoded video channels are
 * exposed; the JPEG channels carry no bitrate or GOP worth showing. */
static void collect_rvd(cJSON *state)
{
	cJSON *resp = ask("rvd", "{\"cmd\":\"status\"}");
	if (!resp)
		return;

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

/* rsd: viewer count and listening port. */
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
		}
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

cJSON *rmq_poll_state(rmq_daemons_t *out)
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

	if (out->up[RMQ_D_RVD])
		collect_rvd(state);
	if (out->up[RMQ_D_RSD])
		collect_rsd(state);
	if (out->up[RMQ_D_RIC])
		collect_ric(state);

	collect_system(state);

	return state;
}
