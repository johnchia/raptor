/*
 * rmq_mdns.c -- one-shot DNS-SD browse over multicast DNS
 *
 * Answers one question: what on this network offers a given service? A PTR
 * query names the instances, an SRV query turns each instance into a host and
 * port, and an A query turns that host into an address. The three are chained
 * rather than issued together because each one's question is built out of the
 * previous one's answer.
 *
 * Built on libmdnsd, which is already on the camera doing the advertising
 * half. Reusing it keeps a second DNS codec out of raptor. The socket is ours
 * because mdnsd's multicast helper lives in the daemon's own sources and is
 * not part of the installed library.
 *
 * Discovery is never load-bearing: every caller has a configured value to fall
 * back on, and a network with nothing listening must behave exactly like one
 * that was never asked. So the browse is bounded by the caller's timeout and
 * reports whatever resolved by then, rather than waiting for completeness.
 */

#include "rmq_mdns.h"

#include <rss_common.h>

#include <string.h>

#ifdef RMQ_HAS_MDNS

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <libmdnsd/mdnsd.h>

#define MDNS_GROUP "224.0.0.251"
#define MDNS_PORT  5353

/* The meta-query. Its PTR answers name service *types* rather than instances,
 * so they have no SRV or TXT to chase and asking for one is 16 wasted queries
 * on a typical network. */
#define MDNS_ENUM_TYPE "_services._dns-sd._udp.local."

/* Browse state threaded through the answer callbacks. */
typedef struct {
	mdns_daemon_t *d;
	rmq_mdns_service_t *out;
	size_t max;
	size_t count;
	bool enumerating; /* browsing MDNS_ENUM_TYPE, so do not chase */
} browse_t;

/* ------------------------------------------------------------------ */
/* Socket                                                              */
/* ------------------------------------------------------------------ */

static int mdns_open(void)
{
	struct sockaddr_in sin;
	struct ip_mreq mreq;
	int sd, on = 1;
	unsigned char ttl = 255, loop = 1;

	sd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sd < 0)
		return -1;

	/*
	 * mdnsd already holds 5353 doing the advertising half, and responses
	 * to our queries are multicast to that port rather than back to
	 * whatever we sent from -- so binding it is not optional, and sharing
	 * it is how mDNS is meant to work. Every socket joined to the group
	 * gets its own copy of each datagram.
	 */
	setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
	setsockopt(sd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(MDNS_PORT);
	sin.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(sd, (struct sockaddr *)&sin, sizeof(sin)) < 0)
		goto fail;

	memset(&mreq, 0, sizeof(mreq));
	mreq.imr_multiaddr.s_addr = inet_addr(MDNS_GROUP);
	mreq.imr_interface.s_addr = htonl(INADDR_ANY);
	if (setsockopt(sd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
		goto fail;

	/* 255 is required by the spec, not a reach: responders drop mDNS that
	 * arrives with any other TTL. Loopback stays on so a responder on this
	 * same camera is still heard. */
	setsockopt(sd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
	setsockopt(sd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

	fcntl(sd, F_SETFL, O_NONBLOCK);
	return sd;

fail:
	close(sd);
	return -1;
}

/* ------------------------------------------------------------------ */
/* Answers                                                             */
/* ------------------------------------------------------------------ */

static rmq_mdns_service_t *entry_for(browse_t *b, const char *instance)
{
	rmq_mdns_service_t *e;
	size_t i;

	for (i = 0; i < b->count; i++) {
		if (!strcmp(b->out[i].instance, instance))
			return &b->out[i];
	}
	if (b->count >= b->max)
		return NULL;

	e = &b->out[b->count++];
	memset(e, 0, sizeof(*e));
	rss_strlcpy(e->instance, instance, sizeof(e->instance));
	return e;
}

/*
 * TXT rdata is a run of length-prefixed strings. Flattened to one printable
 * line for display only -- nothing parses this back, so non-printable bytes
 * become dots rather than being escaped.
 */
static void txt_flatten(char *dst, size_t dstsz, const unsigned char *rdata, size_t rdlen)
{
	size_t i = 0, o = 0;

	if (dstsz == 0)
		return;
	dst[0] = '\0';
	if (!rdata)
		return;

	while (i < rdlen) {
		size_t n = rdata[i++], k;

		if (n == 0 || i + n > rdlen)
			break;
		if (o && o + 1 < dstsz)
			dst[o++] = ' ';
		for (k = 0; k < n && o + 1 < dstsz; k++) {
			unsigned char c = rdata[i + k];

			dst[o++] = isprint(c) ? (char)c : '.';
		}
		i += n;
	}
	dst[o] = '\0';
}

static int on_a(mdns_answer_t *a, void *arg)
{
	browse_t *b = arg;
	size_t i;

	if (!a->name || a->ttl == 0)
		return 0;

	RSS_DEBUG("mdns: A %s -> %s", a->name, inet_ntoa(a->ip));

	/* Several instances can share one host, so this is not a lookup that
	 * stops at the first match. */
	for (i = 0; i < b->count; i++) {
		rmq_mdns_service_t *e = &b->out[i];

		if (strcmp(e->host, a->name))
			continue;
		inet_ntop(AF_INET, &a->ip, e->addr, sizeof(e->addr));
	}
	return 0;
}

static int on_srv(mdns_answer_t *a, void *arg)
{
	browse_t *b = arg;
	rmq_mdns_service_t *e;

	if (!a->name || !a->rdname || a->ttl == 0)
		return 0;

	e = entry_for(b, a->name);
	if (!e)
		return 0;

	RSS_DEBUG("mdns: SRV %s -> %s:%u", a->name, a->rdname, (unsigned)a->srv.port);

	e->port = a->srv.port;
	rss_strlcpy(e->host, a->rdname, sizeof(e->host));

	if (!mdnsd_has_query(b->d, e->host))
		mdnsd_query(b->d, e->host, QTYPE_A, on_a, b);
	return 0;
}

static int on_txt(mdns_answer_t *a, void *arg)
{
	browse_t *b = arg;
	rmq_mdns_service_t *e;

	if (!a->name || a->ttl == 0)
		return 0;

	e = entry_for(b, a->name);
	if (!e)
		return 0;

	txt_flatten(e->txt, sizeof(e->txt), a->rdata, a->rdlen);
	return 0;
}

static int on_ptr(mdns_answer_t *a, void *arg)
{
	browse_t *b = arg;
	rmq_mdns_service_t *e;

	if (!a->rdname || a->ttl == 0)
		return 0;

	RSS_DEBUG("mdns: PTR %s -> %s (from %s)", a->name, a->rdname, inet_ntoa(a->ip));

	e = entry_for(b, a->rdname);
	if (!e) /* table full; keep the ones already being resolved */
		return 0;

	if (b->enumerating) /* a type name, with nothing behind it to ask for */
		return 0;

	/*
	 * mdnsd tracks queries by name alone, so this guard covers the SRV and
	 * the TXT together -- asking after the first is registered would find
	 * a query already there and skip the second.
	 */
	if (!mdnsd_has_query(b->d, e->instance)) {
		mdnsd_query(b->d, e->instance, QTYPE_SRV, on_srv, b);
		mdnsd_query(b->d, e->instance, QTYPE_TXT, on_txt, b);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Browse                                                              */
/* ------------------------------------------------------------------ */

static long elapsed_ms(const struct timespec *since)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - since->tv_sec) * 1000L + (now.tv_nsec - since->tv_nsec) / 1000000L;
}

static void pump_out(browse_t *b, int sd)
{
	struct sockaddr_in to;
	struct message m;
	struct in_addr ip;
	unsigned short port;

	while (mdnsd_out(b->d, &m, &ip, &port)) {
		int len = message_packet_len(&m);

		memset(&to, 0, sizeof(to));
		to.sin_family = AF_INET;
		to.sin_port = port;
		to.sin_addr = ip;

		if (sendto(sd, message_packet(&m), (size_t)len, 0, (struct sockaddr *)&to,
			   sizeof(to)) != len)
			RSS_DEBUG("mdns: send failed: %s", strerror(errno));
	}
}

int rmq_mdns_browse(const char *type, unsigned timeout_ms, rmq_mdns_service_t *out, size_t max)
{
	unsigned char buf[MAX_PACKET_LEN];
	struct timespec start;
	browse_t b;
	size_t i;
	int sd, rc;

	if (!type || !out || max == 0)
		return -1;

	memset(&b, 0, sizeof(b));
	b.out = out;
	b.max = max;
	b.enumerating = strcmp(type, MDNS_ENUM_TYPE) == 0;

	sd = mdns_open();
	if (sd < 0) {
		RSS_WARN("mdns: cannot open multicast socket: %s", strerror(errno));
		return -1;
	}

	b.d = mdnsd_new(QCLASS_IN, MAX_PACKET_LEN);
	if (!b.d) {
		close(sd);
		return -1;
	}

	mdnsd_query(b.d, type, QTYPE_PTR, on_ptr, &b);
	clock_gettime(CLOCK_MONOTONIC, &start);

	for (;;) {
		struct timeval tv, *nap;
		fd_set fds;
		long left = (long)timeout_ms - elapsed_ms(&start);

		if (left <= 0)
			break;

		/* Wake for whichever comes first: the caller's deadline, or
		 * the retransmit mdnsd wants next. */
		tv.tv_sec = left / 1000;
		tv.tv_usec = (left % 1000) * 1000;
		nap = mdnsd_sleep(b.d);
		if (nap && (nap->tv_sec < tv.tv_sec ||
			    (nap->tv_sec == tv.tv_sec && nap->tv_usec < tv.tv_usec)))
			tv = *nap;

		FD_ZERO(&fds);
		FD_SET(sd, &fds);
		if (select(sd + 1, &fds, NULL, NULL, &tv) < 0) {
			if (errno == EINTR)
				continue;
			RSS_WARN("mdns: select failed: %s", strerror(errno));
			break;
		}

		if (FD_ISSET(sd, &fds)) {
			struct sockaddr_in from;
			socklen_t slen = sizeof(from);
			ssize_t n;

			while ((n = recvfrom(sd, buf, sizeof(buf), 0, (struct sockaddr *)&from,
					     &slen)) > 0) {
				struct message m;

				memset(&m, 0, sizeof(m));
				if (message_parse(&m, buf) == 0)
					mdnsd_in(b.d, &m, from.sin_addr, from.sin_port);
				slen = sizeof(from);
			}
		}

		pump_out(&b, sd);
	}

	for (i = 0; i < b.count; i++)
		b.out[i].resolved = b.out[i].addr[0] != '\0' && b.out[i].port != 0;

	rc = (int)b.count;

	mdnsd_shutdown(b.d);
	pump_out(&b, sd); /* the goodbye packets mdnsd_shutdown queues */
	mdnsd_free(b.d);
	close(sd);

	return rc;
}

int rmq_mdns_find_broker(char *host, size_t hostsz, int *port, unsigned timeout_ms)
{
	rmq_mdns_service_t svc[8];
	int n, i;

	if (!host || hostsz == 0)
		return -1;

	n = rmq_mdns_browse(RMQ_MDNS_MQTT_TYPE, timeout_ms, svc, sizeof(svc) / sizeof(svc[0]));
	if (n <= 0)
		return -1;

	for (i = 0; i < n; i++) {
		if (!svc[i].resolved)
			continue;
		rss_strlcpy(host, svc[i].addr, hostsz);
		if (port)
			*port = svc[i].port;
		RSS_INFO("mdns: broker %s at %s:%u", svc[i].instance, svc[i].addr,
			 (unsigned)svc[i].port);
		return 0;
	}

	RSS_INFO("mdns: %d broker(s) announced, none resolved in time", n);
	return -1;
}

bool rmq_mdns_available(void)
{
	return true;
}

#else /* !RMQ_HAS_MDNS */

/*
 * Built without libmdnsd. The stubs report "found nothing", which is the same
 * answer an empty network gives, so callers need no build-time conditional --
 * they already have to handle discovery coming up empty.
 *
 * They do need to be able to say which of the two it was, though, or a camera
 * that cannot ask is indistinguishable from a network with nothing on it.
 * rmq_mdns_available() is that, and nothing else.
 */

int rmq_mdns_browse(const char *type, unsigned timeout_ms, rmq_mdns_service_t *out, size_t max)
{
	(void)type;
	(void)timeout_ms;
	(void)out;
	(void)max;
	return -1;
}

int rmq_mdns_find_broker(char *host, size_t hostsz, int *port, unsigned timeout_ms)
{
	(void)host;
	(void)hostsz;
	(void)port;
	(void)timeout_ms;
	return -1;
}

bool rmq_mdns_available(void)
{
	return false;
}

#endif /* RMQ_HAS_MDNS */
