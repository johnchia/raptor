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
 * Take the geometry the resolution list is derived from out of a freshly
 * collected state document. Returns true when it differs from what the last
 * discovery was built with, i.e. that discovery now describes sizes the camera
 * no longer has and must be republished.
 *
 * Called before discovery rather than after, so the first cycle — where the
 * geometry arrives and discovery is published together — needs no second pass.
 */
bool rmq_ha_note_geometry(struct rmq_state *st, const cJSON *state);

/*
 * Publish (or republish) the discovery document for the daemons currently
 * running. `previous` is the set published last time so components whose
 * daemon has gone are emitted as empty objects, which is how HA is told to
 * remove an entity. Pass NULL on first publish.
 *
 * Returns 0 on success.
 */
int rmq_ha_publish_discovery(struct rmq_state *st, const rmq_daemons_t *now,
			     const rmq_daemons_t *previous);

/* Remove the whole device from HA — an empty payload on the discovery topic. */
int rmq_ha_clear_discovery(struct rmq_state *st);

#endif /* RMQ_HA_H */
