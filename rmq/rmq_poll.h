/*
 * rmq_poll.h -- Daemon discovery and state collection
 *
 * Raptor is modular: an install runs only the daemons it needs, and which
 * ones those are is discovered at runtime rather than configured. So the
 * bridge probes the control sockets each cycle and reports on what answered,
 * which is also what lets Home Assistant entities appear and disappear with
 * the daemons that back them.
 *
 * The control protocol is strictly request/response with no event push
 * (raptor-ipc/src/rss_ctrl.c), so state has to be polled — that is a property
 * of the protocol, not a choice made here.
 */

#ifndef RMQ_POLL_H
#define RMQ_POLL_H

#include <stdbool.h>
#include <stdint.h>

#include <cJSON.h>

/* Daemons the bridge knows how to ask for state. The order is the order they
 * appear in diagnostics. */
typedef enum {
	RMQ_D_RVD = 0,
	RMQ_D_RSD,
	RMQ_D_RAD,
	RMQ_D_ROD,
	RMQ_D_RIC,
	RMQ_D_RMR,
	RMQ_D_RMD,
	RMQ_D_RHD,
	RMQ_D_RWD,
	RMQ_D_COUNT,
} rmq_daemon_t;

const char *rmq_daemon_name(rmq_daemon_t d);

typedef struct {
	bool up[RMQ_D_COUNT];
	int up_count;
} rmq_daemons_t;

/* True when the set of running daemons differs, i.e. discovery must be
 * republished because entities have appeared or vanished. */
bool rmq_daemons_differ(const rmq_daemons_t *a, const rmq_daemons_t *b);

/*
 * Probe every daemon and collect state into one JSON document. Returns a
 * cJSON object the caller must delete, or NULL on allocation failure.
 * `out` receives which daemons answered.
 *
 * One document rather than a topic per value: MQTT gives no ordering across
 * topics, so a single retained publish keeps every entity mutually consistent
 * and costs one message instead of twenty.
 */
cJSON *rmq_poll_state(rmq_daemons_t *out);

#endif /* RMQ_POLL_H */
