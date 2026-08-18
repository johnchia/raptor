/*
 * rcd_guard.h -- Confirm-or-revert: a change that puts itself back
 *
 * Some settings are delivered over the connection they are about to break. An
 * address, a wifi passphrase, the camera's name: the client sends it, the
 * camera enacts it, and if the value was wrong the client is no longer talking
 * to anything. On a camera up a pole that is a trip with a ladder.
 *
 * So a key may declare a window. rcd snapshots the guarded keys, writes the
 * new value, and arms a timer; the client's job is to re-reach the camera --
 * at its new address, by its new name -- and say `confirm`. Silence is a
 * failure: when the timer expires the snapshot is written back and enacted, so
 * the camera returns to the last state somebody proved they could reach.
 *
 *     Steady ------ set(guarded key) ------> Armed ---- confirm ----> Steady
 *        ^                                     |
 *        |                                     +-- timer expires --+
 *        +------- snapshot re-enacted -------- Reverting <---------+
 *                                                 ^
 *                                                 +-- cancel
 *
 * Three properties are the whole of it:
 *
 * - **The guard arms on `set`, not on `apply`.** A provider's store is the
 *   value, so a provider-backed key is in force the moment `set` returns and
 *   there is no later step to hang a guard on. This is the same reason those
 *   keys report tier "live" -- see rcd_schema.c.
 *
 * - **The snapshot covers every guarded key, not the edited ones.** Guarded
 *   providers share files: a batch that turns DHCP off and sets an address
 *   has to be undone as a batch, and a key the request did not name may still
 *   have been rewritten by a provider that owns the same stanza.
 *
 * - **A reboot inside the window reverts.** The snapshot is on flash, because
 *   restoring it has to survive losing power. The deadline is in /run, which
 *   is a tmpfs -- so its absence means the camera has rebooted since the guard
 *   was armed, and a camera that rebooted did not confirm. rcd puts the
 *   snapshot back on the way up. That is what makes power-cycling a stranded
 *   camera a recovery instead of a commitment.
 *
 * A second guarded `set` inside an open window keeps the original snapshot and
 * only pushes the deadline out. The guard protects the state that was last
 * confirmed reachable, and two edits into the dark are still one experiment.
 */

#ifndef RCD_GUARD_H
#define RCD_GUARD_H

#include <stdbool.h>
#include <stdint.h>

#include <cJSON.h>

#include "rcd_schema.h"

struct rcd_state;

/*
 * The window for a change to the camera's name. Long enough for a console to
 * notice, re-resolve and come back; short enough that nobody waits it out by
 * accident.
 */
#define RCD_GUARD_NAME_SEC 90

/* Guarded keys one snapshot can hold. The table has one today; the network
 * and wifi sections are five more. */
#define RCD_GUARD_MAX 12

/* What to put back, on flash: reverting has to survive the power going out. */
#define RCD_GUARD_RECORD_NAME "rcd.guard"

/* When to put it back, on a tmpfs: gone after a reboot, which is the signal. */
#define RCD_GUARD_ARMED_PATH "/run/rss/rcd.armed"

typedef struct {
	char section[RCD_SECT_MAX];
	char key[RCD_KEY_MAX];
	char prev[RCD_VAL_MAX];
	bool had; /* the store held a value; otherwise there is nothing to go
		   * back to and the new one stands */
} rcd_guard_snap_t;

/*
 * Snapshot every guarded key and start the clock, before the first write.
 *
 * Called with the longest window the request's keys ask for. Failing to
 * persist the snapshot is not a refusal: the edit the caller asked for still
 * happens, and the log says the guard is not covering it -- a camera that
 * cannot write /etc is in trouble the guard was never going to fix.
 */
void rcd_guard_arm(struct rcd_state *st, int window_sec);

/* Seconds left, or 0 when nothing is armed. */
int rcd_guard_remaining(const struct rcd_state *st);

/* Add the "guard" object to a reply, and only when one is armed: a client
 * that never sees it has nothing to confirm. */
void rcd_guard_report(const struct rcd_state *st, cJSON *out);

/* Put the snapshot back and disarm. Safe with nothing armed. */
void rcd_guard_revert(struct rcd_state *st, const char *why);

/* Disarm and keep the new values. */
void rcd_guard_confirm(struct rcd_state *st);

/* Called from the serve loop; reverts when the window has run out. */
void rcd_guard_tick(struct rcd_state *st, uint64_t now_ms);

/*
 * Read the record at startup. A snapshot with no deadline beside it in /run is
 * one that outlived a reboot, and is put back immediately -- see above.
 */
void rcd_guard_load(struct rcd_state *st);

cJSON *rcd_cmd_confirm(struct rcd_state *st, const cJSON *root);
cJSON *rcd_cmd_cancel(struct rcd_state *st, const cJSON *root);

#endif /* RCD_GUARD_H */
