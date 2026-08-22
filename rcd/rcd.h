/*
 * rcd.h -- RCD (Raptor Config Daemon) internal state
 *
 * One process owns the configuration: it validates every edit against a table,
 * writes raptor.conf, and sequences the daemon restarts that some edits need.
 * Everything else -- the MQTT bridge, the CLI, a web UI -- is a transport onto
 * this daemon's control socket, and none of them repeats the policy.
 *
 * The single owner is not about the file. rss_config_save already edits
 * surgically under a flock, so several writers cannot corrupt it. It is about
 * the restart: rvd deinitialises MI for the whole system, every other daemon
 * holding an MI reference has to be told to let go first, and two processes
 * running that sequence at once is a camera that does not come back.
 */

#ifndef RCD_H
#define RCD_H

#include <rss_common.h>
#include <rss_ipc.h>

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>

#include "rcd_guard.h"
#include "rcd_schema.h"

/* The protocol version every response carries. Bumped when a field changes
 * meaning or disappears -- never for one that is merely added. */
#define RCD_API_VERSION 1

/*
 * Saved edits a running daemon has not read yet.
 *
 * Held in memory and mirrored to /run so an rcd restart does not forget them.
 * /run is a tmpfs, which is exactly the lifetime wanted: after a reboot every
 * daemon has re-read the file, so there is no drift left to remember.
 */
#define RCD_STALE_MAX  64
#define RCD_STALE_PATH "/run/rss/rcd.stale"

typedef struct {
	char section[RCD_SECT_MAX];
	char key[RCD_KEY_MAX];
} rcd_keyref_t;

struct rcd_state {
	const char *config_path;
	volatile sig_atomic_t *running;
	rss_ctrl_t *ctrl;

	/* Keys written to the file whose owning daemon is still running with
	 * the old value, and the daemons those keys belong to. */
	rcd_keyref_t stale[RCD_STALE_MAX];
	int stale_count;
	bool stale_daemon[RCD_D_COUNT];

	/*
	 * The last apply failure, carried until the next apply clears it. A
	 * resume that failed is the one state nothing else reveals: audio
	 * simply stops while every audio setting still reads correctly.
	 */
	char apply_error[160];

	/*
	 * Which [image] keys this SoC has a setter for, as rvd reports them:
	 * ",brightness,contrast,...," -- comma-delimited and comma-terminated so
	 * a membership test is a search for ",key," and cannot match "again"
	 * inside "max_again". Empty means rvd has not answered yet, which is not
	 * the same as nothing being settable, so nothing is hidden until it has.
	 *
	 * Not invalidated on a restart: the table behind it is compile-time
	 * constant for the SoC, so the only reason to ask again is that the
	 * first attempt found rvd down.
	 */
	char isp_settable[320];

	/*
	 * Live edits a daemon has applied and not yet written to flash. The
	 * value is already in effect; only the record of it is owed.
	 */
	bool save_owed[RCD_D_COUNT];
	uint64_t save_due_ms;	/* 0 = nothing owed */
	uint64_t save_first_ms; /* when the oldest owed change arrived */

	/* An apply is running. It blocks for as long as the slowest daemon
	 * takes to come back, so a second one is refused rather than queued. */
	bool applying;

	/*
	 * Confirm-or-revert: what the guarded keys held before the change
	 * nobody has confirmed yet, and when it goes back. See rcd_guard.h.
	 */
	rcd_guard_snap_t guard[RCD_GUARD_MAX];
	int guard_count;
	uint64_t guard_deadline_ms; /* 0 = nothing armed */
	int guard_window_sec;

	/*
	 * A wifi credential moved in the change now armed, so the guard may
	 * confirm itself from the radio -- see RCD_GUARD_WIFI_DWELL_SEC.
	 *
	 * Decided when the guard is armed rather than looked up later: the
	 * snapshot covers every guarded key whether or not it was edited, so
	 * by the time the window is running there is nothing left to say
	 * which of them the operator actually touched.
	 */
	bool guard_wifi_moved;
	uint64_t guard_wifi_ask_ms; /* next time to ask the radio; 0 = not yet */

	/*
	 * Reverts that did not fully land. A store that could not be written
	 * back, or an interface that would not come up on the old stanza, is
	 * retried rather than forgotten -- so this counts the attempts and
	 * stops them, because a failure that will never clear should not log
	 * forever. See rcd_guard.c.
	 */
	int guard_retries;
};

typedef struct rcd_state rcd_state_t;

/* Record that `section`/`key` was written and its owner has not read it. */
void rcd_stale_add(rcd_state_t *st, const char *section, const char *key, rcd_daemon_t owner);

/* Forget every key owned by `d`, which has just re-read the file. */
void rcd_stale_clear(rcd_state_t *st, rcd_daemon_t d);

/* Forget one key, whoever owns it: what was written has been taken back, so
 * there is nothing left for an apply to enact. */
void rcd_stale_forget(rcd_state_t *st, const char *section, const char *key);

/* Load and store the drift record. Failure is not fatal: an unreadable or
 * unwritable /run costs the memory of a pending restart, not the edit. */
void rcd_stale_load(rcd_state_t *st);
void rcd_stale_save(const rcd_state_t *st);

#endif /* RCD_H */
