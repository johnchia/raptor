/*
 * rmq_ha.h -- Home Assistant MQTT discovery
 *
 * Publishes one retained device-based discovery document
 * (<discovery_prefix>/device/<id>/config) carrying every component, rather
 * than one retained topic per entity. With ~200 configurable keys eventually
 * in scope that difference is the one between a single message and two
 * hundred sitting on the broker forever.
 *
 * Phase 2 publishes read-only sensors only: nothing here creates a control,
 * so there is no path from Home Assistant to any daemon yet.
 */

#ifndef RMQ_HA_H
#define RMQ_HA_H

#include <stdbool.h>

#include "rmq_poll.h"

struct rmq_state;

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
