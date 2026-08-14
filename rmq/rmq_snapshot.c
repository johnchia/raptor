/*
 * rmq_snapshot.c -- see rmq_snapshot.h
 */

#include "rmq_snapshot.h"
#include "rmq.h"

#include <rss_common.h>
#include <rss_net.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <cJSON.h>

/* How long to wait before trying again when there is no URL to build yet. */
#define RETRY_MS 5000

static uint64_t now_ms(void)
{
	return (uint64_t)(rss_timestamp_us() / 1000);
}

/*
 * The camera's address on the interface that reaches the broker.
 *
 * Taken from the connected MQTT socket rather than from an interface list,
 * because it is the one address known to route to a reader of these topics.
 * A camera with more than one interface has no other way to tell which of its
 * addresses is the useful one, and the guess an interface list invites is
 * wrong precisely on the cameras where it matters.
 */
static int local_addr(const struct rmq_state *st, char *out, size_t outsz)
{
	int fd = rmq_mqtt_get_fd(st->mqtt);
	if (fd < 0)
		return -1;

	struct sockaddr_storage ss;
	socklen_t len = sizeof(ss);
	if (getsockname(fd, (struct sockaddr *)&ss, &len) < 0)
		return -1;

	/* "???" is what the formatter yields for a family it does not know,
	 * and a URL built around it would resolve to nothing. */
	rss_addr_str(&ss, out, outsz);
	return strcmp(out, "???") == 0 ? -1 : 0;
}

/*
 * The [http] credential, rendered as a URL userinfo field.
 *
 * Only the RFC 3986 unreserved set is embedded, which is exactly what the
 * credential control admits (see V_CRED in rmq_cmd.c) — so anything else came
 * from a hand-edited config and would need percent-encoding to survive being
 * part of a URL. Refusing is the honest failure: an escaping guess that turns
 * out wrong is a 401 with nothing to explain it.
 */
static bool userinfo(const struct rmq_state *st, char *out, size_t outsz)
{
	out[0] = '\0';
	if (!st->http_user[0] || !st->http_pass[0])
		return false;

	for (const char *s = st->http_user; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (!isalnum(c) && c != '-' && c != '_' && c != '.' && c != '~')
			return false;
	}
	for (const char *s = st->http_pass; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (!isalnum(c) && c != '-' && c != '_' && c != '.' && c != '~')
			return false;
	}

	snprintf(out, outsz, "%s:%s@", st->http_user, st->http_pass);
	return true;
}

void rmq_http_creds(struct rmq_state *st, rss_config_t *cfg)
{
	rss_strlcpy(st->http_user, rss_config_get_str(cfg, "http", "username", ""),
		    sizeof(st->http_user));
	rss_strlcpy(st->http_pass, rss_config_get_str(cfg, "http", "password", ""),
		    sizeof(st->http_pass));
}

int rmq_snapshot_url(const struct rmq_state *st, const char *path, bool with_auth, char *out,
		     size_t outsz)
{
	if (st->http_port <= 0)
		return -1;

	char addr[INET6_ADDRSTRLEN];
	if (local_addr(st, addr, sizeof(addr)) < 0)
		return -1;

	char auth[136] = "";
	if (with_auth)
		userinfo(st, auth, sizeof(auth));

	/* An IPv6 literal is bracketed, or its colons read as the port. */
	const char *open = strchr(addr, ':') ? "[" : "";
	const char *close = open[0] ? "]" : "";

	snprintf(out, outsz, "http%s://%s%s%s%s:%d%s", st->http_tls ? "s" : "", auth, open, addr,
		 close, st->http_port, path);
	return 0;
}

bool rmq_snapshot_due(const struct rmq_state *st, uint64_t now)
{
	if (!st->snapshot_enabled)
		return false;

	/* Zero means nothing has been published on this connection yet. One
	 * document goes out as soon as the bridge is up whatever the interval
	 * says, because an image entity with no URL behind it reads as broken
	 * rather than as waiting — and because the address in it is the one
	 * this connection is using, which a reconnect may have changed. */
	return st->snapshot_next_ms == 0 || now >= st->snapshot_next_ms;
}

int rmq_snapshot_publish(struct rmq_state *st)
{
	/*
	 * Rescheduled before the attempt, not after it, so a camera that
	 * cannot name a URL retries on its own clock rather than on every pass
	 * of the serve loop.
	 *
	 * A failure is retried on a fixed delay even when no interval is
	 * configured, because the usual reason for one is a poll that has not
	 * happened yet — rhd's listener is learned there, and a reconnect can
	 * fall between two of them. Leaving that case waiting for a command
	 * would mean an image entity with no URL until someone noticed.
	 */
	st->snapshot_next_ms = st->snapshot_interval_sec > 0
				       ? now_ms() + (uint64_t)st->snapshot_interval_sec * 1000
				       : UINT64_MAX;

	char path[64], snap[RMQ_URL_MAX], mjpeg[RMQ_URL_MAX];

	/*
	 * The still carries the credential and the stream does not. Home
	 * Assistant's image entity has no field for one, so the URL is the
	 * only place it can go; its MJPEG camera integration asks for the
	 * username and password itself, and this value is read by a person.
	 *
	 * What that costs is the [http] password sitting in a retained message
	 * on the broker, and it is a real cost — stated in raptor.conf next to
	 * the setting that turns the picture on, so it is chosen rather than
	 * discovered. On the usual bridge it grants nothing new: the command
	 * topic can already set that password to whatever it likes, so a client
	 * that can read this is one that could replace it. With `commands =
	 * false` that is no longer true, and this is the one thing the broker
	 * learns that a read-only bridge would not otherwise tell it.
	 */
	snprintf(path, sizeof(path), "/snap.jpg?stream=%d", st->snapshot_stream);
	if (rmq_snapshot_url(st, path, true, snap, sizeof(snap)) < 0) {
		RSS_DEBUG("snapshot: no URL yet — is rhd running?");
		st->snapshot_next_ms = now_ms() + RETRY_MS;
		return -1;
	}

	snprintf(path, sizeof(path), "/mjpeg?stream=%d", st->snapshot_stream);
	if (rmq_snapshot_url(st, path, false, mjpeg, sizeof(mjpeg)) < 0) {
		st->snapshot_next_ms = now_ms() + RETRY_MS;
		return -1;
	}

	cJSON *doc = cJSON_CreateObject();
	if (!doc)
		return -1;
	cJSON_AddStringToObject(doc, "snapshot", snap);
	cJSON_AddStringToObject(doc, "mjpeg", mjpeg);

	char *payload = cJSON_PrintUnformatted(doc);
	cJSON_Delete(doc);
	if (!payload)
		return -1;

	/*
	 * Retained, so a dashboard opened later has a URL to fetch rather than
	 * an empty tile until the next refresh falls due.
	 */
	int rc = rmq_mqtt_publish(st->mqtt, st->topic_snapshot, payload, strlen(payload), 1, true);
	free(payload);

	if (rc == 0)
		RSS_INFO("snapshot: published %s", mjpeg);
	return rc;
}
