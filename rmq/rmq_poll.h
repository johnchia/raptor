/*
 * rmq_poll.h -- The camera's state, as rcd reports it
 *
 * Raptor is modular: an install runs only the daemons it needs, and which
 * ones those are is discovered at runtime rather than configured. rcd does
 * that discovery for every client, so the bridge asks it once a cycle instead
 * of probing nine control sockets itself -- which is also what lets Home
 * Assistant entities appear and disappear with the daemons that back them.
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

struct rmq_state;

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

/* Inverse of the above. Returns RMQ_D_COUNT for a name we do not know. */
rmq_daemon_t rmq_daemon_by_name(const char *name);

typedef struct {
	bool up[RMQ_D_COUNT];
	int up_count;
} rmq_daemons_t;

/* True when the set of running daemons differs, i.e. discovery must be
 * republished because entities have appeared or vanished. */
bool rmq_daemons_differ(const rmq_daemons_t *a, const rmq_daemons_t *b);

/*
 * Ask rcd for the camera's state. Returns a cJSON object the caller must
 * delete, or NULL when rcd did not answer -- in which case there is nothing
 * to publish, and publishing an empty document instead would withdraw every
 * entity and read as a camera that had lost its daemons.
 *
 * `out` receives which daemons rcd found running.
 *
 * One document rather than a topic per value: MQTT gives no ordering across
 * topics, so a single retained publish keeps every entity mutually consistent
 * and costs one message instead of twenty.
 *
 * `st` is written as well as read: rhd's listener is cached there, because it
 * is what the picture URLs are built from and this is the one place that asks
 * for it.
 */
cJSON *rmq_poll_state(struct rmq_state *st, rmq_daemons_t *out);

#endif /* RMQ_POLL_H */
