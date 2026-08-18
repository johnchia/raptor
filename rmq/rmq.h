/*
 * rmq.h -- RMQ (Raptor MQTT bridge) internal state
 */

#ifndef RMQ_H
#define RMQ_H

#include <rss_common.h>
#include <rss_ipc.h>

#include <stdbool.h>
#include <stdint.h>

#include "rmq_mqtt.h"
#include "rmq_poll.h"
#include "rmq_snapshot.h"
#include "rmq_system.h"

/* Derived topics are the prefix plus a suffix, so the prefix is capped low
 * enough that appending the longest suffix cannot truncate. */
#define RMQ_TOPIC_MAX  256
#define RMQ_PREFIX_MAX 192

/*
 * Where to look for a broker when the config names none, and how long to wait.
 * The wait is spent once at startup and delays nothing else, but it is time the
 * camera is not bridging, so it is short enough to be unnoticed on a network
 * that answers and to be tolerable on one that never will.
 */
#define RMQ_MDNS_DISCOVER_MS 3000
#define RMQ_BROKER_FALLBACK  "127.0.0.1"
#define RMQ_BROKER_PORT	     1883

/* The retained availability payloads, which the Last Will also carries — so
 * they are shared rather than private to whoever publishes them. */
#define RMQ_STATUS_ONLINE  "online"
#define RMQ_STATUS_OFFLINE "offline"

/* Encoded streams the entity table describes: main and sub. A camera running
 * more is reported in the state document all the same; it just gets no
 * entities for them. */
#define RMQ_STREAM_COUNT 2

/* Named (not just typedef'd) so headers can forward-declare it. */
struct rmq_state {
	/* Config */
	rss_config_t *cfg;
	const char *config_path;
	volatile sig_atomic_t *running;

	char host[128];
	int port;
	/* `host` came from mDNS rather than the config file. Recorded so the
	 * status output can say where the broker address came from -- an
	 * address nobody configured is otherwise indistinguishable from a
	 * typo'd one. */
	bool host_discovered;
	char client_id[64];
	char username[64];
	char password[128];
	char topic_prefix[RMQ_PREFIX_MAX];
	char discovery_prefix[64];
	char device_name[64];
	/* The line under the device name, or empty to use the camera's own
	 * address — see [mqtt] device_subtitle. */
	char subtitle[64];
	bool use_tls;
	bool ha_discovery;
	bool commands_enabled;
	int keepalive_sec;
	int reconnect_delay_ms;
	int poll_interval_sec;

	/*
	 * Whether a saved edit should be enacted without being asked.
	 *
	 * Off by default, and that is the point: restarting rvd stops capture
	 * for tens of seconds, and nobody moving a dropdown in a dashboard
	 * asked for that. The Apply button is how it gets asked for. This key
	 * exists for installations that would rather not be asked -- and for
	 * anyone who relied on the old behaviour, where every restart-tier
	 * edit bounced its daemon a few seconds later with no way to decline.
	 */
	bool auto_apply;

	/* The picture. On by default, and withheld when the camera cannot
	 * serve one rather than when a key says not to: what it costs is a URL
	 * on the broker and one JPEG encode per fetch, which is not worth
	 * asking permission for. The key remains an opt-out, because the URL
	 * carries the [http] credential when one is set. */
	bool snapshot_enabled;
	int snapshot_stream;	   /* which JPEG ring, 0 = main and 1 = sub */
	int snapshot_interval_sec; /* 0 = once per broker connection */
	uint64_t snapshot_next_ms;

	/*
	 * rhd's listener, as of the last poll, and the credential it demands.
	 * Together they are the picture URL. Port 0 means rhd is not answering
	 * and there is no URL to name.
	 *
	 * The credential is here rather than read at use because rmq holds a
	 * config it loaded at startup: the copy goes stale exactly when the
	 * password is changed over MQTT, so the command path refreshes these
	 * two from the file after any write that rcd accepted.
	 */
	int http_port;
	bool http_tls;
	char http_user[64];
	char http_pass[64];

	/* Derived topics */
	char topic_status[RMQ_TOPIC_MAX];
	char topic_state[RMQ_TOPIC_MAX];
	char topic_discovery[RMQ_TOPIC_MAX];
	char topic_cmd[RMQ_TOPIC_MAX];
	char topic_result[RMQ_TOPIC_MAX];
	char topic_snapshot[RMQ_TOPIC_MAX];

	/* Connection */
	rmq_mqtt_t *mqtt;

	/* Which daemons were present at the last discovery publish, so
	 * entities can be removed when their daemon goes away. */
	rmq_daemons_t last_daemons;
	bool discovery_published;

	/* What the camera said about itself at the last poll, for the entity
	 * definitions that depend on it. Cached rather than asked for at
	 * publish time so that what an entity offers and what it reports come
	 * from the same document, and cannot disagree. */
	int jpeg_channels;	/* rvd's JPEG channels; 0 = no picture to offer */
	char isp_settable[320]; /* ",key,key," — see rvd's get-isp */

	/* Control socket */
	rss_ctrl_t *ctrl;
};

typedef struct rmq_state rmq_state_t;

#endif /* RMQ_H */
