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
#include "rmq_restart.h"

/* Derived topics are the prefix plus a suffix, so the prefix is capped low
 * enough that appending the longest suffix cannot truncate. */
#define RMQ_TOPIC_MAX  256
#define RMQ_PREFIX_MAX 192

/* However long a burst of commands runs, a change reaches flash within this. */
#define RMQ_SAVE_MAX_DELAY_MS 15000

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
	bool commands_enabled;
	int keepalive_sec;
	int reconnect_delay_ms;
	int poll_interval_sec;
	int save_debounce_ms;
	int restart_debounce_ms;

	/* Derived topics */
	char topic_status[RMQ_TOPIC_MAX];
	char topic_state[RMQ_TOPIC_MAX];
	char topic_discovery[RMQ_TOPIC_MAX];
	char topic_cmd[RMQ_TOPIC_MAX];
	char topic_result[RMQ_TOPIC_MAX];

	/* Connection */
	rmq_mqtt_t *mqtt;

	/* Which daemons were present at the last discovery publish, so
	 * entities can be removed when their daemon goes away. */
	rmq_daemons_t last_daemons;
	bool discovery_published;

	/* Config writes owed to daemons, deferred so that a burst of commands
	 * costs one flash write rather than one per command. */
	bool save_owed[RMQ_D_COUNT];
	uint64_t save_due_ms;	/* 0 = nothing owed */
	uint64_t save_first_ms; /* when the oldest owed change arrived */

	/* Restart tier: edits a daemon only sees when it re-reads the config,
	 * held until the burst ends so a section costs one write and one
	 * restart rather than one of each per key. */
	rmq_cfg_write_t cfg_writes[RMQ_CFG_PENDING_MAX];
	int cfg_write_count;
	bool restart_owed[RMQ_D_COUNT];
	uint64_t restart_due_ms;   /* 0 = nothing staged */
	uint64_t restart_first_ms; /* when the oldest staged edit arrived */
	char restart_error[160];   /* last failure, until the next apply */

	/* Control socket */
	rss_ctrl_t *ctrl;
};

typedef struct rmq_state rmq_state_t;

#endif /* RMQ_H */
