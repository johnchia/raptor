/*
 * rmq_mdns.h -- one-shot DNS-SD browse over multicast DNS
 */

#ifndef RMQ_MDNS_H
#define RMQ_MDNS_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A DNS-SD instance name is capped at 255 on the wire. Nothing this is
 * pointed at comes close, and a name that does not fit is one this camera
 * cannot connect to anyway, so the shorter bound is the useful one. */
#define RMQ_MDNS_NAME_MAX 128
#define RMQ_MDNS_TXT_MAX  256

/* The service brokers advertise themselves as. Overridable at build time so the
 * not-found path can be exercised on a network that does have a broker. */
#ifndef RMQ_MDNS_MQTT_TYPE
#define RMQ_MDNS_MQTT_TYPE "_mqtt._tcp.local."
#endif

typedef struct {
	char instance[RMQ_MDNS_NAME_MAX]; /* "nas._mqtt._tcp.local." */
	char host[RMQ_MDNS_NAME_MAX];	  /* SRV target, "nas.local." */
	char addr[INET_ADDRSTRLEN];	  /* from A, empty until it arrives */
	char txt[RMQ_MDNS_TXT_MAX];	  /* flattened, printable, for display */
	uint16_t port;
	/* Both an address and a port arrived, so this entry can be connected
	 * to. A half-resolved entry is normal mid-browse and is still
	 * reported, because knowing a broker exists is worth something even
	 * when the browse timed out before its address landed. */
	bool resolved;
} rmq_mdns_service_t;

/*
 * Browse for `type` (a fully-qualified DNS-SD type, trailing dot included)
 * for at most `timeout_ms`, filling up to `max` entries.
 *
 * Returns the number found, or -1 if the browse could not be started. Zero is
 * an ordinary answer: it means nothing replied, not that anything failed.
 */
int rmq_mdns_browse(const char *type, unsigned timeout_ms, rmq_mdns_service_t *out, size_t max);

/*
 * Convenience over the above: the first connectable MQTT broker on the
 * network. Writes a dotted-quad address rather than the advertised hostname,
 * because the camera has no mDNS resolver in NSS and so cannot look a .local
 * name back up.
 *
 * Returns 0 on success, -1 if nothing was found -- which callers must treat
 * as "use the configured broker", never as an error.
 */
int rmq_mdns_find_broker(char *host, size_t hostsz, int *port, unsigned timeout_ms);

#endif /* RMQ_MDNS_H */
