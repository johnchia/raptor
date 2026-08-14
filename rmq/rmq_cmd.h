/*
 * rmq_cmd.h -- Command policy and dispatch
 *
 * The command topic is the only route from the network into a daemon, so what
 * crosses it is an allowlist rather than a filter. A command the table does
 * not name is refused, and a field its entry does not name is dropped. The
 * request handed to a control socket is rebuilt from the table and never
 * relayed, which is what keeps `save` with an arbitrary `file`, `shutdown`,
 * and `config-get-section` against a section holding credentials out — by
 * construction, rather than by a rule per hazard that has to be remembered
 * each time the table grows.
 */

#ifndef RMQ_CMD_H
#define RMQ_CMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rmq_restart.h"

struct rmq_state;

/* Longest daemon request the table can produce, with room to spare. */
#define RMQ_CMD_REQ_MAX 512

typedef enum {
	RMQ_PLAN_DAEMON = 0, /* a request for one daemon's control socket */
	RMQ_PLAN_CONFIG,     /* edits to the config file, applied by the bridge */
	RMQ_PLAN_SNAPSHOT,   /* republish the picture URL; the bridge does it */
	RMQ_PLAN_SYSTEM,     /* a setting in /etc rather than in raptor.conf */
	RMQ_PLAN_REBOOT,     /* reboot the camera, once the answer is out */
} rmq_plan_kind_t;

/*
 * A validated command, ready to act on. `daemon` points into static storage.
 * Kept separate from the acting so the policy is testable without a socket or
 * a config file, which is the half worth testing.
 */
typedef struct {
	rmq_plan_kind_t kind;

	/* RMQ_PLAN_DAEMON */
	const char *daemon;
	char request[RMQ_CMD_REQ_MAX];
	bool persists; /* the daemon will dirty its config, so a save is owed */

	/*
	 * RMQ_PLAN_CONFIG. The owner is per edit rather than per command,
	 * because one command can span sections: a camera credential is one
	 * field that lands in both [rtsp] and [http], and rsd and rhd have to
	 * be restarted for their own halves of it.
	 */
	rmq_cfg_write_t writes[RMQ_CFG_SET_MAX];
	rmq_daemon_t write_owner[RMQ_CFG_SET_MAX];
	int write_count;
} rmq_cmd_plan_t;

/*
 * Validate `json` against the allowlist and build the daemon request.
 * Returns 0 on success, or -1 with `err` describing the refusal. The refusal
 * text reaches whoever sent the command, so it says what was wrong with this
 * request and nothing about what else the table holds.
 */
int rmq_cmd_plan(const char *json, rmq_cmd_plan_t *out, char *err, size_t errsz);

/* Subscribe to the command topic. Safe to call again after a reconnect. */
int rmq_cmd_subscribe(struct rmq_state *st);

/* Handle one message from the command topic: plan, dispatch, publish result. */
void rmq_cmd_handle(struct rmq_state *st, const char *topic, const uint8_t *payload, size_t len);

/*
 * Write every owed config back to disk now, and clear the debt. Called when
 * st->save_due_ms falls due, and again at shutdown so a change made moments
 * before a stop is not lost.
 */
void rmq_cmd_flush_saves(struct rmq_state *st);

#endif /* RMQ_CMD_H */
