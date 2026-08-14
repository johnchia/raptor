/*
 * rmq_restart.h -- Config writes that only take effect on a daemon restart
 *
 * The live tier changes a running daemon. This tier changes the file the
 * daemon read at startup, so the daemon has to be restarted to see it — a
 * heavier operation with one specific danger: restarting rvd deinitialises MI
 * for the whole system, and any other process still holding MI references dies
 * with it. rad dies silently and nothing respawns it.
 *
 * So a restart is bracketed rather than issued. Every MI-holding daemon is
 * told to release first and to resume afterwards, and the resume is placed so
 * that it runs whatever happened in between — a restart that failed or timed
 * out is precisely when a skipped resume would leave the camera deaf with
 * nothing left running to notice.
 */

#ifndef RMQ_RESTART_H
#define RMQ_RESTART_H

#include <stdbool.h>
#include <stdint.h>

#include <cJSON.h>

#include "rmq_poll.h"

struct rmq_state;

/* However long a burst of edits runs, it is applied within this. */
#define RMQ_RESTART_MAX_DELAY_MS 30000

/* Keys one command may carry, and keys a whole burst may accumulate. */
#define RMQ_CFG_SET_MAX	    16
#define RMQ_CFG_PENDING_MAX 40

/* Bounds on one rendered edit. Every value fits: the longest is a credential,
 * capped at 63 by the table entry that admits it. */
#define RMQ_CFG_SECT_MAX    24
#define RMQ_CFG_KEY_MAX	    32
#define RMQ_CFG_VAL_MAX	    64

/* One edit, already validated and rendered to the string the file holds. */
typedef struct {
	char section[RMQ_CFG_SECT_MAX];
	char key[RMQ_CFG_KEY_MAX];
	char value[RMQ_CFG_VAL_MAX];
} rmq_cfg_write_t;

/*
 * Stage one edit and arm the debounce. The value is already rendered and the
 * owner already resolved by the policy in rmq_cmd.c — nothing here re-checks
 * either, so nothing here may be called with anything that table did not
 * produce. Staging the same key twice replaces it rather than queueing it,
 * which is what makes a burst cost one write.
 */
void rmq_restart_stage(struct rmq_state *st, const rmq_cfg_write_t *w, rmq_daemon_t owner);

/* True once the debounce window has closed on the staged edits. */
bool rmq_restart_due(const struct rmq_state *st, uint64_t now_ms);

/* Write every staged edit to the config file, then restart what owns them. */
void rmq_restart_apply(struct rmq_state *st);

/*
 * Write the staged edits and stop there, leaving the daemons alone. Used at
 * shutdown: an edit made moments before a stop must not be lost, but rmq
 * cannot tell its own restart from the whole camera going down, and bouncing
 * the video pipeline on the way out is the worse of the two mistakes. The
 * daemons read the new value when they next start.
 */
void rmq_restart_flush_writes(struct rmq_state *st);

/* Add the restart tier's own state to the state document. */
void rmq_restart_report(const struct rmq_state *st, cJSON *state);

#endif /* RMQ_RESTART_H */
