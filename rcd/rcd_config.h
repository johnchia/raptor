/*
 * rcd_config.h -- Reading and writing the configuration
 *
 * `set` changes the desired state and never restarts anything. A key with a
 * live command is applied to the running daemon as well, because tuning by eye
 * cannot wait for a restart between adjustments; a key without one is written
 * to the file and its owner is recorded as running behind. Either way the
 * reply says which happened, per edit, so nothing about the outcome has to be
 * inferred from a table the caller cannot see.
 *
 * Enacting the difference is `apply`, and it is somebody's explicit decision.
 */

#ifndef RCD_CONFIG_H
#define RCD_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include <cJSON.h>

#include "rcd_schema.h"

struct rcd_state;

/* Keys one request may carry. A form submits a page, not a section tree. */
#define RCD_EDITS_MAX 32

/*
 * How long a burst of live edits may go unsaved.
 *
 * Only the flash write is deferred, never the effect: a live key is already in
 * the running daemon when this is pending, so nothing observable waits on it.
 * Dragging a slider is tens of commands, and saving on each one would write
 * flash tens of times for one adjustment.
 */
#define RCD_SAVE_DEBOUNCE_MS 2000
#define RCD_SAVE_MAX_MS	     15000

/*
 * One validated edit: what the file will hold, and the same value in the form
 * a live command wants.
 *
 * `reset` is the edit that carries no value at all -- put this key back to its
 * default -- and the default is never written, because rcd does not have one:
 * every default is the argument at its own read site inside the owning daemon.
 * What a reset does is take the key out, so that read site answers again.
 */
typedef struct {
	const struct rcd_key *k;
	char rendered[RCD_VAL_MAX];
	double num;
	bool is_num;
	bool reset;
} rcd_edit_t;

/*
 * Validate the edits a `set` request carries. Touches nothing: no socket, no
 * config file, no state. Returns NULL with `count` edits rendered, or the
 * refusal, which the caller deletes.
 *
 * Split out from the acting so the policy is testable on its own, which is the
 * half worth testing -- this is the only route from a network transport into
 * the configuration, so a refusal is as much the contract as an acceptance.
 */
cJSON *rcd_set_validate(const cJSON *root, rcd_edit_t *out, int *count);

/*
 * Whether this camera has the control at all, as opposed to having it set to
 * something. True for everything rcd cannot answer for, including every key
 * while rvd is down.
 */
bool rcd_key_available(struct rcd_state *st, const struct rcd_key *k);

/*
 * The same for an action: validate the arguments and render the daemon request
 * the table would send, without sending it. `daemon` receives the owner.
 */
cJSON *rcd_action_validate(const cJSON *root, char *req, size_t reqsz, const char **daemon);

cJSON *rcd_cmd_get(struct rcd_state *st, const cJSON *root);
cJSON *rcd_cmd_set(struct rcd_state *st, const cJSON *root);

/*
 * The camera's account, which raptor.conf holds as two.
 *
 * [rtsp] and [http] each carry their own username and password and nothing
 * makes them agree -- but a camera with two passwords is a camera whose second
 * password is the one nobody remembers, so this writes both from one value.
 * A command of its own rather than an edits array because either field may be
 * given alone, which a caller building a fixed array cannot express.
 */
cJSON *rcd_cmd_credentials(struct rcd_state *st, const cJSON *root);
cJSON *rcd_cmd_action(struct rcd_state *st, const cJSON *root);

/* Report what has been written and not yet read, and what enacting it costs.
 * Added to both `pending` and `state`, so one poll answers both. */
void rcd_config_report_stale(const struct rcd_state *st, cJSON *out);

/*
 * Provider-backed keys whose store is written and whose value is not yet in
 * force -- the system's half of the drift `apply` exists to settle. Kept in
 * the same stale list as everything else, so an rcd restart does not forget
 * that an interface is still running on the old address.
 *
 * `rcd_enact_owed` fills `out` with them and returns how many; `rcd_enact_done`
 * forgets them, and is called only for the ones that were actually enacted.
 */
int rcd_enact_owed(const struct rcd_state *st, const struct rcd_key **out, int max);
void rcd_enact_done(struct rcd_state *st);

/* Whether a deferred save has fallen due, and the write itself. `flush` is
 * also called at shutdown, so a change made moments before a stop is kept. */
bool rcd_save_due(const struct rcd_state *st, uint64_t now_ms);
void rcd_save_flush(struct rcd_state *st);

#endif /* RCD_CONFIG_H */
