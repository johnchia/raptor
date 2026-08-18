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

struct rcd_state;

cJSON *rcd_cmd_apply(struct rcd_state *st, const cJSON *root);
cJSON *rcd_cmd_restart(struct rcd_state *st, const cJSON *root);

#endif /* RCD_APPLY_H */
