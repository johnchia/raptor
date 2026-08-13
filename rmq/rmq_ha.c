/*
 * rmq_ha.c -- Home Assistant MQTT discovery
 *
 * Entity triage follows the rule that a Home Assistant entity implies live
 * state: everything here reads something the camera actually reports, and
 * every one of them is a sensor. Categorisation decides where they land on
 * the device page — uncategorised entities form the short primary card, while
 * `config` and `diagnostic` collapse into their own sections, which is what
 * keeps the page legible as the entity count grows.
 */

#include "rmq_ha.h"
#include "rmq.h"

#include <rss_common.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>

/* Where an entity appears on the device page. */
typedef enum {
	CAT_PRIMARY = 0, /* uncategorised — the short card at the top */
	CAT_DIAGNOSTIC,
} ha_category_t;

typedef struct {
	const char *key;       /* component key + unique_id suffix */
	const char *name;      /* entity name, shown under the device name */
	const char *value;     /* value_template path into the state document */
	const char *unit;      /* NULL for none */
	const char *dev_class; /* HA device_class, NULL for none */
	const char *icon;      /* NULL for none */
	ha_category_t cat;
	rmq_daemon_t owner; /* component exists only while this daemon runs */
} ha_entity_t;

/*
 * Units and constraints belong in the name: it is the only string visible
 * without opening the entity, and HA has no help-text field of any kind.
 */
static const ha_entity_t entities[] = {
	/* -- Primary: what an operator actually looks at -- */
	{"ir_state", "Day/night", "ir.state", NULL, NULL, "mdi:theme-light-dark", CAT_PRIMARY,
	 RMQ_D_RIC},
	{"rtsp_clients", "RTSP viewers", "rtsp.clients", NULL, NULL, "mdi:account-eye", CAT_PRIMARY,
	 RMQ_D_RSD},

	/* -- Diagnostic: video -- */
	{"stream0_resolution", "Main resolution", "stream0.resolution", NULL, NULL,
	 "mdi:television", CAT_DIAGNOSTIC, RMQ_D_RVD},
	{"stream0_codec", "Main codec", "stream0.codec", NULL, NULL, "mdi:video", CAT_DIAGNOSTIC,
	 RMQ_D_RVD},
	{"stream0_bitrate", "Main bitrate (target)", "stream0.bitrate", "bit/s", "data_rate", NULL,
	 CAT_DIAGNOSTIC, RMQ_D_RVD},
	{"stream0_avg_bitrate", "Main bitrate (actual)", "stream0.avg_bitrate", "bit/s",
	 "data_rate", NULL, CAT_DIAGNOSTIC, RMQ_D_RVD},
	{"stream0_fps", "Main frame rate", "stream0.fps", "fps", NULL, "mdi:filmstrip",
	 CAT_DIAGNOSTIC, RMQ_D_RVD},
	{"stream0_gop", "Main GOP", "stream0.gop", NULL, NULL, "mdi:key-variant", CAT_DIAGNOSTIC,
	 RMQ_D_RVD},
	{"stream1_resolution", "Sub resolution", "stream1.resolution", NULL, NULL, "mdi:television",
	 CAT_DIAGNOSTIC, RMQ_D_RVD},
	{"stream1_bitrate", "Sub bitrate (target)", "stream1.bitrate", "bit/s", "data_rate", NULL,
	 CAT_DIAGNOSTIC, RMQ_D_RVD},
	{"stream1_avg_bitrate", "Sub bitrate (actual)", "stream1.avg_bitrate", "bit/s", "data_rate",
	 NULL, CAT_DIAGNOSTIC, RMQ_D_RVD},

	/* -- Diagnostic: exposure. Raw platform units, not lux or seconds:
	 *    total_gain is the vendor's own scale (SigmaStar x1024), and
	 *    presenting it as anything else would invent precision. -- */
	{"ir_mode", "IR-cut mode", "ir.mode", NULL, NULL, "mdi:cog", CAT_DIAGNOSTIC, RMQ_D_RIC},
	{"total_gain", "Total gain (raw)", "ir.total_gain", NULL, NULL, "mdi:brightness-6",
	 CAT_DIAGNOSTIC, RMQ_D_RIC},
	{"exposure_us", "Exposure", "ir.exposure_us", "µs", NULL, "mdi:camera-iris", CAT_DIAGNOSTIC,
	 RMQ_D_RIC},
	{"ae_luma", "AE luma", "ir.ae_luma", NULL, NULL, "mdi:brightness-percent", CAT_DIAGNOSTIC,
	 RMQ_D_RIC},

	/* -- Diagnostic: system. Owner RMQ_D_COUNT means "always present". -- */
	{"daemons_up", "Daemons running", "daemons_up", NULL, NULL, "mdi:cogs", CAT_DIAGNOSTIC,
	 RMQ_D_COUNT},
	{"uptime", "Uptime", "uptime", "s", "duration", NULL, CAT_DIAGNOSTIC, RMQ_D_COUNT},
};

static const int entity_count = (int)(sizeof(entities) / sizeof(entities[0]));

static bool entity_available(const ha_entity_t *e, const rmq_daemons_t *d)
{
	if (e->owner == RMQ_D_COUNT)
		return true;
	return d->up[e->owner];
}

/* Build one component entry. */
static cJSON *make_component(struct rmq_state *st, const ha_entity_t *e)
{
	cJSON *c = cJSON_CreateObject();
	if (!c)
		return NULL;

	char uid[128];
	snprintf(uid, sizeof(uid), "%s_%s", st->client_id, e->key);

	char tpl[128];
	snprintf(tpl, sizeof(tpl), "{{ value_json.%s | default('') }}", e->value);

	cJSON_AddStringToObject(c, "p", "sensor");
	cJSON_AddStringToObject(c, "uniq_id", uid);
	cJSON_AddStringToObject(c, "name", e->name);
	cJSON_AddStringToObject(c, "stat_t", st->topic_state);
	cJSON_AddStringToObject(c, "val_tpl", tpl);

	if (e->unit)
		cJSON_AddStringToObject(c, "unit_of_meas", e->unit);
	if (e->dev_class)
		cJSON_AddStringToObject(c, "dev_cla", e->dev_class);
	if (e->icon)
		cJSON_AddStringToObject(c, "ic", e->icon);
	if (e->cat == CAT_DIAGNOSTIC)
		cJSON_AddStringToObject(c, "ent_cat", "diagnostic");

	return c;
}

int rmq_ha_publish_discovery(struct rmq_state *st, const rmq_daemons_t *now,
			     const rmq_daemons_t *previous)
{
	cJSON *root = cJSON_CreateObject();
	if (!root)
		return -1;

	/* Device identity. All components collapse under this on one page. */
	cJSON *dev = cJSON_AddObjectToObject(root, "dev");
	cJSON_AddStringToObject(dev, "ids", st->client_id);
	cJSON_AddStringToObject(dev, "name", st->device_name);
	cJSON_AddStringToObject(dev, "mf", "Raptor");
	cJSON_AddStringToObject(dev, "mdl", st->model);
	if (&rss_build_hash)
		cJSON_AddStringToObject(dev, "sw", rss_build_hash);

	cJSON *origin = cJSON_AddObjectToObject(root, "o");
	cJSON_AddStringToObject(origin, "name", "rmq");
	if (&rss_build_hash)
		cJSON_AddStringToObject(origin, "sw", rss_build_hash);

	/* Availability comes from the same retained topic the Last Will sets,
	 * so every entity greys out together when the camera dies rather than
	 * showing values that stopped being true. */
	cJSON_AddStringToObject(root, "avty_t", st->topic_status);
	cJSON_AddStringToObject(root, "pl_avail", "online");
	cJSON_AddStringToObject(root, "pl_not_avail", "offline");
	cJSON_AddStringToObject(root, "stat_t", st->topic_state);
	cJSON_AddNumberToObject(root, "qos", 1);

	cJSON *cmps = cJSON_AddObjectToObject(root, "cmps");
	if (!cmps) {
		cJSON_Delete(root);
		return -1;
	}

	int published = 0;
	for (int i = 0; i < entity_count; i++) {
		const ha_entity_t *e = &entities[i];

		if (entity_available(e, now)) {
			cJSON *c = make_component(st, e);
			if (c) {
				cJSON_AddItemToObject(cmps, e->key, c);
				published++;
			}
			continue;
		}

		/* Present last time but not now: an empty component object is
		 * how HA is told to delete the entity. Without this a stopped
		 * daemon leaves dead rows showing their last value forever. */
		if (previous && entity_available(e, previous))
			cJSON_AddItemToObject(cmps, e->key, cJSON_CreateObject());
	}

	char *payload = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!payload)
		return -1;

	int rc = rmq_mqtt_publish(st->mqtt, st->topic_discovery, payload, strlen(payload), 1, true);
	RSS_INFO("ha: discovery published, %d entities, %zu bytes", published, strlen(payload));
	free(payload);

	return rc;
}

int rmq_ha_clear_discovery(struct rmq_state *st)
{
	return rmq_mqtt_publish(st->mqtt, st->topic_discovery, "", 0, 1, true);
}
