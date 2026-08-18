/*
 * rmq_poll.c -- Asking rcd what the camera is doing
 *
 * The probing this file used to do now lives in rcd, which every client
 * shares. What is left is the part that is genuinely the bridge's: turning
 * rcd's answer into the shape the entity table reads, and adding the one
 * reading rcd cannot produce -- a URL, which needs the address this process
 * is reachable on and rcd has no socket to learn it from.
 */

#include "rmq_poll.h"
#include "rmq.h"
#include "rmq_rcd.h"

#include <rss_common.h>

#include <stdio.h>
#include <string.h>

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

static int json_int(const cJSON *o, const char *key, int fallback)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
	return cJSON_IsNumber(v) ? (int)cJSON_GetNumberValue(v) : fallback;
}

/*
 * The picture and stream URLs.
 *
 * rcd reports rhd's port and says nothing about the address, because it has no
 * way to know which of the camera's interfaces a given client can reach. This
 * process does: it is holding a socket to the broker, and whatever address that
 * socket is bound to provably routes to whoever is reading the broker.
 */
static void add_urls(struct rmq_state *st, cJSON *state)
{
	cJSON *http = cJSON_GetObjectItemCaseSensitive(state, "http");
	if (!cJSON_IsObject(http)) {
		st->http_port = 0;
		return;
	}

	st->http_port = json_int(http, "port", 0);
	st->http_tls = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(http, "tls"));

	/*
	 * Home Assistant cannot be given an MJPEG camera over discovery -- its
	 * MJPEG integration is added by hand and asks for the URL -- so the
	 * useful thing the bridge can do is say what to paste, rather than
	 * leave it to be worked out from an address the camera knows and the
	 * person reading does not.
	 */
	char path[32], url[RMQ_URL_MAX];
	snprintf(path, sizeof(path), "/mjpeg?stream=%d", st->snapshot_stream);
	if (rmq_snapshot_url(st, path, false, url, sizeof(url)) == 0)
		cJSON_AddStringToObject(http, "mjpeg_url", url);
}

cJSON *rmq_poll_state(struct rmq_state *st, rmq_daemons_t *out)
{
	memset(out, 0, sizeof(*out));

	cJSON *state = rmq_rcd_call("{\"cmd\":\"state\"}");
	if (!state) {
		/*
		 * Without rcd there is no state to publish. Reporting an empty
		 * document instead would withdraw every entity on the dashboard
		 * and read as a camera that had lost its daemons, which is a
		 * worse lie than publishing nothing.
		 */
		st->http_port = 0;
		return NULL;
	}

	/* The envelope is rcd's, not the bridge's: subscribers read a camera
	 * document, and a protocol version they cannot act on is noise in it. */
	cJSON_DeleteItemFromObjectCaseSensitive(state, "api");
	cJSON_DeleteItemFromObjectCaseSensitive(state, "status");

	const cJSON *up = cJSON_GetObjectItemCaseSensitive(state, "up");
	for (int i = 0; i < RMQ_D_COUNT; i++) {
		const cJSON *v = cJSON_GetObjectItemCaseSensitive(up, daemon_names[i]);
		out->up[i] = cJSON_IsTrue(v);
		if (out->up[i])
			out->up_count++;
	}

	add_urls(st, state);
	return state;
}
