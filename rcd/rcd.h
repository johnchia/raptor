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
	 * Live edits a daemon has applied and not yet written to flash. The
	 * value is already in effect; only the record of it is owed.
	 */
	bool save_owed[RCD_D_COUNT];
	uint64_t save_due_ms;	/* 0 = nothing owed */
	uint64_t save_first_ms; /* when the oldest owed change arrived */

	/* An apply is running. It blocks for as long as the slowest daemon
	 * takes to come back, so a second one is refused rather than queued. */
	bool applying;
};

typedef struct rcd_state rcd_state_t;

/* Record that `section`/`key` was written and its owner has not read it. */
void rcd_stale_add(rcd_state_t *st, const char *section, const char *key, rcd_daemon_t owner);

/* Forget every key owned by `d`, which has just re-read the file. */
void rcd_stale_clear(rcd_state_t *st, rcd_daemon_t d);

/* Load and store the drift record. Failure is not fatal: an unreadable or
 * unwritable /run costs the memory of a pending restart, not the edit. */
void rcd_stale_load(rcd_state_t *st);
void rcd_stale_save(const rcd_state_t *st);

#endif /* RCD_H */
