/*
 * rmq_ha.h -- Home Assistant MQTT discovery
 *
 * Publishes one retained device-based discovery document
 * (<discovery_prefix>/device/<id>/config) carrying every component, rather
 * than one retained topic per entity. With ~200 configurable keys eventually
 * in scope that difference is the one between a single message and two
 * hundred sitting on the broker forever.
 */

#ifndef RMQ_HA_H
#define RMQ_HA_H

#include <stdbool.h>

#include <cJSON.h>

#include "rmq_poll.h"

struct rmq_state;

/*
 * Take what the entity definitions depend on — the sensor geometry behind the
 * resolution list, and which ISP keys this platform can set — out of a freshly
 * collected state document. Returns true when any of it differs from what the
 * last discovery was built with, i.e. that discovery now describes a camera
 * this one is not and must be republished.
 *
 * Called before discovery rather than after, so the first cycle — where the
 * camera's answers and the discovery document arrive together — needs no
 * second pass.
 */
bool rmq_ha_note_camera(struct rmq_state *st, const cJSON *state);

/*
 * Publish (or republish) the discovery document for the daemons currently
 * running. Anything the last document carried and this one does not is emitted
 * as an empty object, which is how HA is told to remove an entity — tracked
 * from what was published rather than inferred from which daemons are up,
 * because HA rejects a whole document that withdraws a component it never had.
 *
 * Returns 0 on success.
 */
int rmq_ha_publish_discovery(struct rmq_state *st, const rmq_daemons_t *now);

/* Remove the whole device from HA — an empty payload on the discovery topic. */
int rmq_ha_clear_discovery(struct rmq_state *st);

#endif /* RMQ_HA_H */
