/*
 * rmq_mqtt.c -- MQTT client, implemented on libmosquitto
 *
 * A thin adapter, not a wrapper for its own sake: callers see the seam in
 * rmq_mqtt.h and nothing of the library, so publish/subscribe semantics stay
 * described in one place and the daemon keeps a plain select() main loop.
 *
 * What the library is relied on for, beyond framing: QoS 1 and 2 delivery with
 * retransmission, and a keepalive timed against CLOCK_BOOTTIME. Both matter
 * here — this platform boots with an invalid clock and then steps it, so a
 * wall-clock keepalive stalls for the duration of every backward step.
 */

#include "rmq_mqtt.h"

#include <rss_common.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <mosquitto.h>

/*
 * MQTT treats keepalive 0 as "disabled", libmosquitto rejects anything below
 * five seconds outright. A configured 0 therefore becomes the shortest the
 * library will accept rather than failing the connection.
 */
#define KEEPALIVE_MIN 5

struct rmq_mqtt {
	struct mosquitto *mosq;
	bool connected;
	bool connack_seen;
	int last_connack;

	/* An unreachable broker is worth reporting on every retry, because it
	 * may come back. A library without TLS will not, so say so once. */
	bool tls_unavailable_logged;

	rmq_mqtt_on_message_fn on_message;
	void *user;
};

/* mosquitto_lib_init/cleanup are process-global and not reference counted. */
static int lib_refs;

static uint64_t now_ms(void)
{
	return (uint64_t)(rss_timestamp_us() / 1000);
}

/* ------------------------------------------------------------------ */
/* Callbacks                                                           */
/* ------------------------------------------------------------------ */

static void cb_connect(struct mosquitto *mosq, void *obj, int rc)
{
	(void)mosq;
	rmq_mqtt_t *m = obj;

	/* rc is the CONNACK return code, which is what rmq_connack_t names. */
	m->last_connack = rc;
	m->connack_seen = true;
	m->connected = (rc == RMQ_CONNACK_ACCEPTED);
}

static void cb_disconnect(struct mosquitto *mosq, void *obj, int rc)
{
	(void)mosq;
	rmq_mqtt_t *m = obj;

	m->connected = false;
	if (rc != 0)
		RSS_DEBUG("mqtt: broker closed the connection: %s", mosquitto_strerror(rc));
}

static void cb_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg)
{
	(void)mosq;
	rmq_mqtt_t *m = obj;

	if (!m->on_message || !msg || !msg->topic)
		return;

	size_t len = msg->payloadlen > 0 ? (size_t)msg->payloadlen : 0;
	m->on_message(msg->topic, msg->payload, len, m->user);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

rmq_mqtt_t *rmq_mqtt_new(void)
{
	rmq_mqtt_t *m = calloc(1, sizeof(*m));
	if (!m)
		return NULL;

	if (lib_refs == 0)
		mosquitto_lib_init();
	lib_refs++;

	m->last_connack = -1;
	return m;
}

void rmq_mqtt_free(rmq_mqtt_t *m)
{
	if (!m)
		return;

	if (m->mosq) {
		if (m->connected)
			mosquitto_disconnect(m->mosq);
		mosquitto_destroy(m->mosq);
	}

	if (--lib_refs == 0)
		mosquitto_lib_cleanup();

	free(m);
}

void rmq_mqtt_set_message_cb(rmq_mqtt_t *m, rmq_mqtt_on_message_fn cb, void *user)
{
	if (!m)
		return;
	m->on_message = cb;
	m->user = user;
}

/* ------------------------------------------------------------------ */
/* Connect                                                             */
/* ------------------------------------------------------------------ */

/*
 * Establish that the broker is there, on our own timeout.
 *
 * mosquitto_connect() connects with a blocking socket, so an unreachable
 * broker costs the kernel's full SYN timeout — roughly two minutes, during
 * which this daemon answers nothing on its control socket and never rechecks
 * its shutdown flag. The library does have a non-blocking connect, but only
 * mosquitto_loop_forever() can finish one: it leaves the client in
 * mosq_cs_connect_pending, a state the sync loop reads as "nothing to do" and
 * no public call clears. Probing first bounds the wait and leaves the
 * library's blocking connect with what is by then a local operation.
 */
static int probe_reachable(const char *host, int port, int timeout_ms)
{
	char portstr[16];
	snprintf(portstr, sizeof(portstr), "%d", port);

	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	struct addrinfo *res = NULL;
	int gai = getaddrinfo(host, portstr, &hints, &res);
	if (gai != 0) {
		RSS_WARN("mqtt: cannot resolve %s: %s", host, gai_strerror(gai));
		return -1;
	}

	int saved_errno = 0;
	int ok = -1;

	for (struct addrinfo *ai = res; ai && ok < 0; ai = ai->ai_next) {
		int fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
		if (fd < 0) {
			saved_errno = errno;
			continue;
		}

		int flags = fcntl(fd, F_GETFL, 0);
		if (flags >= 0)
			fcntl(fd, F_SETFL, flags | O_NONBLOCK);

		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
			ok = 0;
			close(fd);
			continue;
		}

		if (errno != EINPROGRESS) {
			saved_errno = errno;
			close(fd);
			continue;
		}

		struct pollfd pfd = {.fd = fd, .events = POLLOUT};
		int pr = poll(&pfd, 1, timeout_ms);
		if (pr > 0) {
			/* A non-blocking connect reports its result here, not
			 * through errno, which still reads EINPROGRESS. */
			int soerr = 0;
			socklen_t slen = sizeof(soerr);
			if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) == 0 && soerr == 0)
				ok = 0;
			else
				saved_errno = soerr ? soerr : EIO;
		} else {
			saved_errno = (pr == 0) ? ETIMEDOUT : errno;
		}

		close(fd);
	}

	freeaddrinfo(res);

	if (ok < 0)
		RSS_WARN("mqtt: %s:%d unreachable: %s", host, port,
			 strerror(saved_errno ? saved_errno : EHOSTUNREACH));

	return ok;
}

int rmq_mqtt_connect(rmq_mqtt_t *m, const rmq_mqtt_opts_t *opts)
{
	if (!m || !opts || !opts->host || !opts->client_id)
		return -1;

	/* Each attempt gets a fresh client. The previous one may still hold a
	 * half-closed socket, and no session state is wanted across attempts. */
	if (m->mosq) {
		mosquitto_destroy(m->mosq);
		m->mosq = NULL;
	}
	m->connected = false;
	m->connack_seen = false;
	m->last_connack = -1;

	int timeout_ms = opts->connect_timeout_ms > 0 ? opts->connect_timeout_ms : 10000;

	if (probe_reachable(opts->host, opts->port, timeout_ms) < 0)
		return -1;

	/* clean_session: every reconnect republishes the full state, retained,
	 * so a queued backlog would deliver nothing but stale readings. */
	m->mosq = mosquitto_new(opts->client_id, true, m);
	if (!m->mosq) {
		RSS_ERROR("mqtt: client alloc failed: %s", strerror(errno));
		return -1;
	}

	mosquitto_int_option(m->mosq, MOSQ_OPT_PROTOCOL_VERSION, MQTT_PROTOCOL_V311);
	mosquitto_connect_callback_set(m->mosq, cb_connect);
	mosquitto_disconnect_callback_set(m->mosq, cb_disconnect);
	mosquitto_message_callback_set(m->mosq, cb_message);

	if (opts->username && opts->username[0]) {
		const char *pw = (opts->password && opts->password[0]) ? opts->password : NULL;
		if (mosquitto_username_pw_set(m->mosq, opts->username, pw) != MOSQ_ERR_SUCCESS) {
			RSS_ERROR("mqtt: failed to set credentials");
			goto fail;
		}
	}

	if (opts->will_topic) {
		const char *wp = opts->will_payload ? opts->will_payload : "";
		if (mosquitto_will_set(m->mosq, opts->will_topic, (int)strlen(wp), wp,
				       opts->will_qos, opts->will_retain) != MOSQ_ERR_SUCCESS) {
			RSS_ERROR("mqtt: failed to set will on %s", opts->will_topic);
			goto fail;
		}
	}

	if (opts->use_tls) {
		int rc = mosquitto_tls_set(m->mosq, NULL, "/etc/ssl/certs", NULL, NULL, NULL);
		if (rc != MOSQ_ERR_SUCCESS) {
			/* Fail rather than fall back. A config asking for TLS must
			 * never quietly put credentials on the wire in the clear,
			 * and images have shipped libmosquitto built without any
			 * TLS backend at all. */
			if (!m->tls_unavailable_logged) {
				RSS_FATAL("mqtt: tls requested but unavailable (%s) — rebuild "
					  "mosquitto with a TLS backend, or set [mqtt] tls = false",
					  mosquitto_strerror(rc));
				m->tls_unavailable_logged = true;
			}
			goto fail;
		}
	}

	int ka = opts->keepalive_sec < KEEPALIVE_MIN ? KEEPALIVE_MIN : opts->keepalive_sec;

	int rc = mosquitto_connect(m->mosq, opts->host, opts->port, ka);
	if (rc != MOSQ_ERR_SUCCESS) {
		RSS_WARN("mqtt: connect to %s:%d failed: %s", opts->host, opts->port,
			 rc == MOSQ_ERR_ERRNO ? strerror(errno) : mosquitto_strerror(rc));
		goto fail;
	}

	/*
	 * mosquitto_connect() returns once CONNECT is away; the broker's CONNACK
	 * arrives at the callback from the loop. Callers publish as soon as this
	 * returns, so wait for it here — the header promises a connected client,
	 * not a pending one.
	 */
	uint64_t deadline = now_ms() + (uint64_t)timeout_ms;
	while (!m->connack_seen && now_ms() < deadline) {
		if (mosquitto_loop(m->mosq, 100, 1) != MOSQ_ERR_SUCCESS)
			break;
	}

	if (!m->connected) {
		if (m->connack_seen)
			RSS_WARN("mqtt: broker rejected connection: %s",
				 mosquitto_connack_string(m->last_connack));
		else
			RSS_WARN("mqtt: no CONNACK from %s:%d within %d ms", opts->host, opts->port,
				 timeout_ms);
		goto fail;
	}

	RSS_INFO("mqtt: connected to %s:%d as '%s'", opts->host, opts->port, opts->client_id);
	return 0;

fail:
	mosquitto_destroy(m->mosq);
	m->mosq = NULL;
	m->connected = false;
	return -1;
}

void rmq_mqtt_disconnect(rmq_mqtt_t *m)
{
	if (!m || !m->mosq)
		return;

	if (m->connected)
		mosquitto_disconnect(m->mosq);
	m->connected = false;
}

bool rmq_mqtt_connected(const rmq_mqtt_t *m)
{
	return m && m->connected;
}

int rmq_mqtt_last_connack(const rmq_mqtt_t *m)
{
	return m ? m->last_connack : -1;
}

/* ------------------------------------------------------------------ */
/* Traffic                                                             */
/* ------------------------------------------------------------------ */

int rmq_mqtt_publish(rmq_mqtt_t *m, const char *topic, const void *payload, size_t len, uint8_t qos,
		     bool retain)
{
	if (!m || !m->mosq || !m->connected || !topic || len > INT_MAX)
		return -1;

	int rc = mosquitto_publish(m->mosq, NULL, topic, (int)len, payload, qos, retain);
	if (rc != MOSQ_ERR_SUCCESS) {
		RSS_WARN("mqtt: publish to %s failed: %s", topic, mosquitto_strerror(rc));
		if (rc == MOSQ_ERR_NO_CONN)
			m->connected = false;
		return -1;
	}

	return 0;
}

int rmq_mqtt_subscribe(rmq_mqtt_t *m, const char *filter, uint8_t qos)
{
	if (!m || !m->mosq || !m->connected || !filter)
		return -1;

	int rc = mosquitto_subscribe(m->mosq, NULL, filter, qos);
	if (rc != MOSQ_ERR_SUCCESS) {
		RSS_WARN("mqtt: subscribe to %s failed: %s", filter, mosquitto_strerror(rc));
		if (rc == MOSQ_ERR_NO_CONN)
			m->connected = false;
		return -1;
	}

	return 0;
}

int rmq_mqtt_loop(rmq_mqtt_t *m, int timeout_ms)
{
	if (!m || !m->mosq || !m->connected)
		return -1;

	int rc = mosquitto_loop(m->mosq, timeout_ms, 1);
	if (rc != MOSQ_ERR_SUCCESS) {
		if (rc != MOSQ_ERR_CONN_LOST && rc != MOSQ_ERR_NO_CONN)
			RSS_WARN("mqtt: loop failed: %s",
				 rc == MOSQ_ERR_ERRNO ? strerror(errno) : mosquitto_strerror(rc));
		m->connected = false;
		return -1;
	}

	/* A clean close by the broker is reported to the disconnect callback
	 * rather than as a loop error, so re-read the flag the callback sets. */
	return m->connected ? 0 : -1;
}

int rmq_mqtt_get_fd(const rmq_mqtt_t *m)
{
	return (m && m->mosq) ? mosquitto_socket(m->mosq) : -1;
}
