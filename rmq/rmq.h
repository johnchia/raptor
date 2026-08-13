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

/* Derived topics are the prefix plus a suffix, so the prefix is capped low
 * enough that appending the longest suffix cannot truncate. */
#define RMQ_TOPIC_MAX  256
#define RMQ_PREFIX_MAX 192

/* Named (not just typedef'd) so headers can forward-declare it. */
struct rmq_state {
	/* Config */
	rss_config_t *cfg;
	const char *config_path;
	volatile sig_atomic_t *running;

	char host[128];
	int port;
	char client_id[64];
	char username[64];
	char password[128];
	char topic_prefix[RMQ_PREFIX_MAX];
	char discovery_prefix[64];
	char device_name[64];
	char model[64];
	bool use_tls;
	bool ha_discovery;
	int keepalive_sec;
	int reconnect_delay_ms;
	int poll_interval_sec;

	/* Derived topics */
	char topic_status[RMQ_TOPIC_MAX];
	char topic_state[RMQ_TOPIC_MAX];
	char topic_discovery[RMQ_TOPIC_MAX];

	/* Connection */
	rmq_mqtt_t *mqtt;

	/* Which daemons were present at the last discovery publish, so
	 * entities can be removed when their daemon goes away. */
	rmq_daemons_t last_daemons;
	bool discovery_published;

	/* Control socket */
	rss_ctrl_t *ctrl;
};

typedef struct rmq_state rmq_state_t;

#endif /* RMQ_H */
