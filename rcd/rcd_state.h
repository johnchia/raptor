/*
 * rcd_state.h -- What the camera is doing right now
 *
 * Raptor is modular: an install runs only the daemons it needs, and which ones
 * those are is discovered at runtime rather than configured. So rcd probes the
 * control sockets and reports on what answered, which is also what lets a
 * client's controls appear and disappear with the daemons behind them.
 *
 * The control protocol is strictly request/response with no event push
 * (raptor-ipc/src/rss_ctrl.c), so state has to be polled -- that is a property
 * of the protocol, not a choice made here. One document rather than a call per
 * value: everything in it is then mutually consistent, and it costs one round
 * trip per daemon instead of one per reading.
 */

#ifndef RCD_STATE_H
#define RCD_STATE_H

#include <cJSON.h>

struct rcd_state;

cJSON *rcd_cmd_state(struct rcd_state *st, const cJSON *root);

#endif /* RCD_STATE_H */
