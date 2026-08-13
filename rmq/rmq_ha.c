/*
 * rmq_ha.c -- Home Assistant MQTT discovery
 *
 * Entity triage follows the rule that a Home Assistant entity implies live
 * state: everything here reads something the camera actually reports.
 * Categorisation decides where they land on the device page — uncategorised
 * entities form the short primary card, while `config` and `diagnostic`
 * collapse into their own sections, which is what keeps the page legible as
 * the entity count grows.
 *
 * Controls all publish to the one command topic, with the JSON the bridge
 * expects carried in the entity's own command template or payload. That keeps
 * a single subscription and a single place where a payload is validated: an
 * entity here cannot widen what the camera accepts, only offer a shape of it.
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
	CAT_CONFIG,
	CAT_DIAGNOSTIC,
} ha_category_t;

static const char *category_name(ha_category_t c)
{
	return c == CAT_CONFIG ? "config" : c == CAT_DIAGNOSTIC ? "diagnostic" : NULL;
}

/*
 * Diagnostics ship disabled. They are the majority of the entity count and
 * the minority of what anyone looks at, and every one of them costs a row in
 * the entity registry and a state write on each poll whether or not it is
 * ever read. Enabling one is two clicks; wading through thirteen to find the
 * two that matter is the cost of the other default.
 *
 * Home Assistant applies this only when it first creates an entity, so a
 * camera already discovered keeps whatever it has. Clearing the discovery
 * topic and letting it republish is what re-applies it.
 */
static bool enabled_by_default(ha_category_t c)
{
	return c != CAT_DIAGNOSTIC;
}

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

	/* -- Diagnostic: video. The configured bitrate, frame rate and GOP are
	 *    controls rather than sensors, so only what cannot be set from
	 *    here is a reading: the negotiated format, and what the encoder
	 *    actually delivered against its target. -- */
	{"stream0_resolution", "Main resolution", "stream0.resolution", NULL, NULL,
	 "mdi:television", CAT_DIAGNOSTIC, RMQ_D_RVD},
	{"stream0_codec", "Main codec", "stream0.codec", NULL, NULL, "mdi:video", CAT_DIAGNOSTIC,
	 RMQ_D_RVD},
	{"stream0_avg_bitrate", "Main bitrate (actual)", "stream0.avg_bitrate", "bit/s",
	 "data_rate", NULL, CAT_DIAGNOSTIC, RMQ_D_RVD},
	{"stream1_resolution", "Sub resolution", "stream1.resolution", NULL, NULL, "mdi:television",
	 CAT_DIAGNOSTIC, RMQ_D_RVD},
	{"stream1_avg_bitrate", "Sub bitrate (actual)", "stream1.avg_bitrate", "bit/s", "data_rate",
	 NULL, CAT_DIAGNOSTIC, RMQ_D_RVD},

	/* -- Diagnostic: exposure. Raw platform units, not lux or seconds:
	 *    total_gain is the vendor's own scale (SigmaStar x1024), and
	 *    presenting it as anything else would invent precision. -- */
	{"total_gain", "Total gain (raw)", "ir.total_gain", NULL, NULL, "mdi:brightness-6",
	 CAT_DIAGNOSTIC, RMQ_D_RIC},
	{"exposure_us", "Exposure", "ir.exposure_us", "µs", NULL, "mdi:camera-iris", CAT_DIAGNOSTIC,
	 RMQ_D_RIC},
	{"ae_luma", "AE luma", "ir.ae_luma", NULL, NULL, "mdi:brightness-percent", CAT_DIAGNOSTIC,
	 RMQ_D_RIC},

	/* -- Diagnostic: audio -- */
	{"audio_codec", "Audio codec", "audio.codec", NULL, NULL, "mdi:waveform", CAT_DIAGNOSTIC,
	 RMQ_D_RAD},
	{"audio_sample_rate", "Audio sample rate", "audio.sample_rate", "Hz", NULL, "mdi:sine-wave",
	 CAT_DIAGNOSTIC, RMQ_D_RAD},

	/* -- Diagnostic: system. Owner RMQ_D_COUNT means "always present". -- */
	{"daemons_up", "Daemons running", "daemons_up", NULL, NULL, "mdi:cogs", CAT_DIAGNOSTIC,
	 RMQ_D_COUNT},
	{"uptime", "Uptime", "uptime", "s", "duration", NULL, CAT_DIAGNOSTIC, RMQ_D_COUNT},
};

static const int entity_count = (int)(sizeof(entities) / sizeof(entities[0]));

/* ------------------------------------------------------------------ */
/* Controls                                                            */
/* ------------------------------------------------------------------ */

typedef enum {
	CTRL_NUMBER = 0,
	CTRL_SELECT,
	CTRL_SWITCH,
	CTRL_BUTTON,
} ha_ctrl_kind_t;

typedef struct {
	const char *key;
	const char *name;
	ha_ctrl_kind_t kind;
	const char *icon;
	ha_category_t cat;
	rmq_daemon_t owner;

	/* Where the current value sits in the state document, so the control
	 * shows what the camera is rather than what it was last told. NULL
	 * for a button, which has no state to show. */
	const char *value;

	/* What gets published. Number and select build their payload from a
	 * template; switch and button have no template support in Home
	 * Assistant, so their payloads are the literal documents to send. */
	const char *cmd_tpl;
	const char *payload;	 /* switch on, or button press */
	const char *payload_off; /* switch off */

	int min, max, step; /* CTRL_NUMBER */
	const char *unit;
	const char *const *options; /* CTRL_SELECT, NULL-terminated */
} ha_control_t;

static const char *const opt_daynight[] = {"auto", "day", "night", NULL};

/*
 * Ranges here are the ones rmq_cmd.c enforces. They are repeated rather than
 * shared because these are a different thing: what the interface offers, not
 * what the camera accepts. A control that stops short of the permitted range
 * is a usability choice; one that runs past it produces rejections.
 *
 * A box accepts a value only on the min + n*step grid, so `min` is chosen to
 * put the values anyone actually types on it — 3 Mbit/s, 25 fps, a GOP of 30.
 * That is why the floors here are round numbers rather than the true minimum
 * rmq_cmd.c would allow.
 */
static const ha_control_t controls[] = {
	{.key = "ircut_mode",
	 .name = "Day/night mode",
	 .kind = CTRL_SELECT,
	 .icon = "mdi:theme-light-dark",
	 .cat = CAT_PRIMARY,
	 .owner = RMQ_D_RIC,
	 .value = "ir.mode",
	 .cmd_tpl = "{\"cmd\":\"ircut-mode\",\"value\":\"{{ value }}\"}",
	 .options = opt_daynight},

	{.key = "osd_enabled",
	 .name = "OSD",
	 .kind = CTRL_SWITCH,
	 .icon = "mdi:format-text",
	 .cat = CAT_PRIMARY,
	 .owner = RMQ_D_ROD,
	 .value = "osd.enabled",
	 .payload = "{\"cmd\":\"osd-enable\"}",
	 .payload_off = "{\"cmd\":\"osd-disable\"}"},

	{.key = "stream0_bitrate_set",
	 .name = "Main bitrate",
	 .kind = CTRL_NUMBER,
	 .cat = CAT_CONFIG,
	 .owner = RMQ_D_RVD,
	 .value = "stream0.bitrate",
	 .cmd_tpl = "{\"cmd\":\"set-bitrate\",\"channel\":0,\"value\":{{ value | int }}}",
	 .min = 100000,
	 .max = 50000000,
	 .step = 100000,
	 .unit = "bit/s"},
	{.key = "stream1_bitrate_set",
	 .name = "Sub bitrate",
	 .kind = CTRL_NUMBER,
	 .cat = CAT_CONFIG,
	 .owner = RMQ_D_RVD,
	 .value = "stream1.bitrate",
	 .cmd_tpl = "{\"cmd\":\"set-bitrate\",\"channel\":1,\"value\":{{ value | int }}}",
	 .min = 100000,
	 .max = 50000000,
	 .step = 100000,
	 .unit = "bit/s"},

	{.key = "stream0_fps_set",
	 .name = "Main frame rate",
	 .kind = CTRL_NUMBER,
	 .icon = "mdi:filmstrip",
	 .cat = CAT_CONFIG,
	 .owner = RMQ_D_RVD,
	 .value = "stream0.fps",
	 .cmd_tpl = "{\"cmd\":\"set-fps\",\"channel\":0,\"value\":{{ value | int }}}",
	 .min = 5,
	 .max = 60,
	 .step = 5,
	 .unit = "fps"},
	{.key = "stream1_fps_set",
	 .name = "Sub frame rate",
	 .kind = CTRL_NUMBER,
	 .icon = "mdi:filmstrip",
	 .cat = CAT_CONFIG,
	 .owner = RMQ_D_RVD,
	 .value = "stream1.fps",
	 .cmd_tpl = "{\"cmd\":\"set-fps\",\"channel\":1,\"value\":{{ value | int }}}",
	 .min = 5,
	 .max = 60,
	 .step = 5,
	 .unit = "fps"},

	{.key = "stream0_gop_set",
	 .name = "Main GOP",
	 .kind = CTRL_NUMBER,
	 .icon = "mdi:key-variant",
	 .cat = CAT_CONFIG,
	 .owner = RMQ_D_RVD,
	 .value = "stream0.gop",
	 .cmd_tpl = "{\"cmd\":\"set-gop\",\"channel\":0,\"value\":{{ value | int }}}",
	 .min = 5,
	 .max = 300,
	 .step = 5},
	{.key = "stream1_gop_set",
	 .name = "Sub GOP",
	 .kind = CTRL_NUMBER,
	 .icon = "mdi:key-variant",
	 .cat = CAT_CONFIG,
	 .owner = RMQ_D_RVD,
	 .value = "stream1.gop",
	 .cmd_tpl = "{\"cmd\":\"set-gop\",\"channel\":1,\"value\":{{ value | int }}}",
	 .min = 5,
	 .max = 300,
	 .step = 5},

	/* Two gain stages, deliberately both exposed: volume is the digital
	 * trim and gain the analog front end, and only the second one can
	 * rescue a quiet microphone. */
	{.key = "audio_volume_set",
	 .name = "Mic volume",
	 .kind = CTRL_NUMBER,
	 .icon = "mdi:microphone",
	 .cat = CAT_CONFIG,
	 .owner = RMQ_D_RAD,
	 .value = "audio.volume",
	 .cmd_tpl = "{\"cmd\":\"set-volume\",\"value\":{{ value | int }}}",
	 .min = 0,
	 .max = 100,
	 .step = 1},
	{.key = "audio_gain_set",
	 .name = "Mic gain (analog step)",
	 .kind = CTRL_NUMBER,
	 .icon = "mdi:microphone-settings",
	 .cat = CAT_CONFIG,
	 .owner = RMQ_D_RAD,
	 .value = "audio.gain",
	 .cmd_tpl = "{\"cmd\":\"set-gain\",\"value\":{{ value | int }}}",
	 .min = 0,
	 .max = 31,
	 .step = 1},

	{.key = "request_idr",
	 .name = "Request keyframe",
	 .kind = CTRL_BUTTON,
	 .icon = "mdi:image-refresh",
	 .cat = CAT_DIAGNOSTIC,
	 .owner = RMQ_D_RVD,
	 .payload = "{\"cmd\":\"request-idr\",\"channel\":0}"},
};

static const int control_count = (int)(sizeof(controls) / sizeof(controls[0]));

static bool owner_available(rmq_daemon_t owner, const rmq_daemons_t *d)
{
	if (owner == RMQ_D_COUNT)
		return true;
	return d->up[owner];
}

/* The fields every component carries, whatever it is. */
static cJSON *make_common(struct rmq_state *st, const char *key, const char *name, const char *icon,
			  ha_category_t cat, const char *platform)
{
	cJSON *c = cJSON_CreateObject();
	if (!c)
		return NULL;

	char uid[128];
	snprintf(uid, sizeof(uid), "%s_%s", st->client_id, key);

	cJSON_AddStringToObject(c, "p", platform);
	cJSON_AddStringToObject(c, "uniq_id", uid);
	cJSON_AddStringToObject(c, "name", name);
	if (icon)
		cJSON_AddStringToObject(c, "ic", icon);
	if (category_name(cat))
		cJSON_AddStringToObject(c, "ent_cat", category_name(cat));
	if (!enabled_by_default(cat))
		cJSON_AddBoolToObject(c, "en", false);

	return c;
}

/* Build one sensor entry. */
static cJSON *make_component(struct rmq_state *st, const ha_entity_t *e)
{
	cJSON *c = make_common(st, e->key, e->name, e->icon, e->cat, "sensor");
	if (!c)
		return NULL;

	char tpl[128];
	snprintf(tpl, sizeof(tpl), "{{ value_json.%s | default('') }}", e->value);

	cJSON_AddStringToObject(c, "stat_t", st->topic_state);
	cJSON_AddStringToObject(c, "val_tpl", tpl);

	if (e->unit)
		cJSON_AddStringToObject(c, "unit_of_meas", e->unit);
	if (e->dev_class)
		cJSON_AddStringToObject(c, "dev_cla", e->dev_class);

	return c;
}

/* Build one control entry. */
static cJSON *make_control(struct rmq_state *st, const ha_control_t *ct)
{
	static const char *const platforms[] = {"number", "select", "switch", "button"};

	cJSON *c = make_common(st, ct->key, ct->name, ct->icon, ct->cat, platforms[ct->kind]);
	if (!c)
		return NULL;

	cJSON_AddStringToObject(c, "cmd_t", st->topic_cmd);

	if (ct->value) {
		char tpl[192];
		switch (ct->kind) {
		case CTRL_SWITCH:
			/* Rendered to the payloads Home Assistant compares
			 * against by default, rather than shipping state_on /
			 * state_off to match Jinja's "True"/"False". */
			snprintf(tpl, sizeof(tpl),
				 "{{ 'ON' if (value_json.%s | default(false)) "
				 "else 'OFF' }}",
				 ct->value);
			break;
		case CTRL_NUMBER:
			/* "None" is the reset payload: it puts the entity in
			 * unknown rather than failing validation, which is
			 * what an absent reading should look like. */
			snprintf(tpl, sizeof(tpl), "{{ value_json.%s | default('None') }}",
				 ct->value);
			break;
		default:
			snprintf(tpl, sizeof(tpl), "{{ value_json.%s | default('') }}", ct->value);
			break;
		}
		cJSON_AddStringToObject(c, "stat_t", st->topic_state);
		cJSON_AddStringToObject(c, "val_tpl", tpl);
	}

	if (ct->cmd_tpl)
		cJSON_AddStringToObject(c, "cmd_tpl", ct->cmd_tpl);

	switch (ct->kind) {
	case CTRL_NUMBER:
		cJSON_AddNumberToObject(c, "min", ct->min);
		cJSON_AddNumberToObject(c, "max", ct->max);
		cJSON_AddNumberToObject(c, "step", ct->step);
		/*
		 * Always a box, never a slider. Home Assistant's slider offers
		 * no text entry at all, so a value has to be dragged to — which
		 * is hopeless for bitrate's three orders of magnitude and merely
		 * annoying for the rest. A box still shows the current value and
		 * still enforces min/max/step.
		 */
		cJSON_AddStringToObject(c, "mode", "box");
		if (ct->unit)
			cJSON_AddStringToObject(c, "unit_of_meas", ct->unit);
		break;
	case CTRL_SELECT: {
		cJSON *opts = cJSON_AddArrayToObject(c, "ops");
		for (int i = 0; opts && ct->options[i]; i++)
			cJSON_AddItemToArray(opts, cJSON_CreateString(ct->options[i]));
		break;
	}
	case CTRL_SWITCH:
		cJSON_AddStringToObject(c, "pl_on", ct->payload);
		cJSON_AddStringToObject(c, "pl_off", ct->payload_off);
		break;
	case CTRL_BUTTON:
		cJSON_AddStringToObject(c, "pl_prs", ct->payload);
		break;
	}

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

		if (owner_available(e->owner, now)) {
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
		if (previous && owner_available(e->owner, previous))
			cJSON_AddItemToObject(cmps, e->key, cJSON_CreateObject());
	}

	/*
	 * Controls only exist while commands are accepted. A read-only bridge
	 * that still advertised them would offer a slider that silently does
	 * nothing, which is worse than not offering it.
	 */
	for (int i = 0; i < control_count; i++) {
		const ha_control_t *ct = &controls[i];
		bool live = st->commands_enabled && owner_available(ct->owner, now);

		if (live) {
			cJSON *c = make_control(st, ct);
			if (c) {
				cJSON_AddItemToObject(cmps, ct->key, c);
				published++;
			}
			continue;
		}

		if (previous && st->commands_enabled && owner_available(ct->owner, previous))
			cJSON_AddItemToObject(cmps, ct->key, cJSON_CreateObject());
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
