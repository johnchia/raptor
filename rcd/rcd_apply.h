/*
 * rcd_apply.h -- Making the running camera match the saved configuration
 *
 * Nothing here ever happens implicitly. `set` records that a daemon is running
 * behind; enacting that is a separate decision, taken by whoever is willing to
 * pay for it, because the price ranges from nobody noticing to every viewer
 * being dropped while the sensor comes back up.
 *
 * Two verbs, and they are not synonyms:
 *
 *   apply   -- restart whatever is running behind, and nothing else.
 *              Idempotent, and a no-op when there is no drift.
 *   restart -- restart these daemons now, whatever their config says.
 *              The operator's verb for something that is wedged.
 *
 * Both go through the same bracket, which exists for one reason: restarting
 * rvd deinitialises MI for the whole system, and any other process still
 * holding an MI reference dies with it. rad dies silently and nothing respawns
 * it. So every MI-holding daemon is told to let go first and to resume
 * afterwards, and the resume runs whatever happened in between -- a restart
 * that failed is precisely when a skipped resume leaves the camera deaf with
 * nothing left running to notice.
 */

#ifndef RCD_APPLY_H
#define RCD_APPLY_H

#include <cJSON.h>
#include <stdbool.h>

struct rcd_state;

cJSON *rcd_cmd_apply(struct rcd_state *st, const cJSON *root);
cJSON *rcd_cmd_restart(struct rcd_state *st, const cJSON *root);

/*
 * Poll `probe` until it answers true or `budget_ms` of wall time is spent,
 * writing the measured elapsed time to `waited_ms` if it is not NULL.
 *
 * Declared here only so the budget can be tested. A probe in this daemon is an
 * IPC round trip that can block for seconds, and the loop this replaced charged
 * its budget for the sleeping alone -- so the bug is invisible unless the test
 * can supply a probe that is slow rather than instant. See the comment on the
 * definition for what that cost.
 */
bool rcd_wait_until(bool (*probe)(const char *), const char *arg, unsigned int budget_ms,
		    unsigned int *waited_ms);

#endif /* RCD_APPLY_H */
