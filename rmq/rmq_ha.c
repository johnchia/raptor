/*
 * rmq_ha.c -- Home Assistant MQTT discovery
 *
 * Entity triage follows the rule that a Home Assistant entity implies live
 * state: everything here reads something the camera actually reports.
 * Categorisation decides where they land on the device page — uncategorised
 * entities form the short primary card, while `diagnostic` collapses into its
 * own section, which is what keeps the page legible as the entity count grows.
 *
 * One device, one document. The camera's settings belong to the web console,
 * and the sub-devices this once published a stream's controls on went with
 * them; what is left describes one camera and fits on one page.
 *
 * Controls all publish to the one command topic, with the JSON the bridge
 * expects carried in the entity's own command template or payload. That keeps
 * a single subscription and a single place where a payload is validated: an
 * entity here cannot widen what the camera accepts, only offer a shape of it.
 */

#include "rmq_ha.h"
#include "rmq.h"

#include <rss_common.h>

#include <netinet/in.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>

/*
 * Where an entity appears on the device page.
 *
 * No `config`: the camera's settings are the web console's, and a second
 * place to change them is a second place for them to disagree. What is left
 * here is what a dashboard is for — readings, the live picture controls, and
 * the two actions.
 */
typedef enum {
	CAT_PRIMARY = 0, /* uncategorised — the short card at the top */
	CAT_DIAGNOSTIC,
} ha_category_t;

/*
 * Sub-devices an earlier build published and this one does not. Their
 * discovery documents are retained on the broker, so Home Assistant would keep
 * the devices and every entity under them indefinitely; an empty payload is
 * how a retained discovery is withdrawn.
 *
 * Deletable once no camera in the fleet still runs a build that published
 * them — they cost one empty publish per discovery cycle until then.
 */
static const char *const retired_devices[] = {"image", "daynight", "main", "sub", NULL};

/*
 * There is deliberately no equivalent list for retired *components*.
 *
 * The two mechanisms look alike and are not. Withdrawing a sub-device is an
 * empty payload on its own retained topic, and a topic Home Assistant never
 * saw simply has nothing to remove — harmless. Withdrawing a component is an
 * empty object inside this device's document, and Home Assistant rejects the
 * whole document when it names a component it does not have. Every other
 * entity in it goes with it: the device is left holding only the stub Home
 * Assistant creates for a `via_device` target, which is a camera page with the
 * stream sub-devices under it and not one entity of its own.
 *
 * A fixed list of retired names cannot be that, and not only on a fresh
 * camera. It is re-sent on every discovery publish, so even where the entities
 * did exist the first publish removes them and every publish after it names
 * components Home Assistant no longer has — the document is fine once and
 * poison from then on, which presents as entities that were working
 * disappearing at the next reconnect.
 *
 * A removal may therefore only name a component this bridge can show it
 * published and has not already withdrawn, which is what the key set below
 * records. An entity renamed between firmware versions is outside anything
 * that record can know, and `raptorctl rmq rediscover` is the answer to it —
 * it drops the device and rebuilds it, which is what a renamed entity needs
 * anyway.
 */

/*
 * What the last document actually contained.
 *
 * A withdrawal may only name a component Home Assistant is known to have, and
 * the only thing that knows is a record of what was published. Availability is
 * not that record: a control can be absent because its daemon is down, but
 * also because the ISP has no such block — and a document withdrawing one that
 * was never published takes every other entity down with it.
 *
 * Module state rather than rmq_state, because it describes this file's own
 * output rather than anything about the camera. It starts empty after a
 * restart, which loses the ability to withdraw something that disappeared
 * across it; that is the harmless direction of the error, and the alternative
 * is guessing.
 */
#define HA_KEYS_MAX 96
#define HA_KEY_MAX  32

static char published_keys[HA_KEYS_MAX][HA_KEY_MAX];
static int published_count;

typedef struct {
	char key[HA_KEYS_MAX][HA_KEY_MAX];
	int count;
	bool overflowed;
} key_set_t;

static void key_add(key_set_t *s, const char *key)
{
	/*
	 * A key that did not fit would read as "not published this time" and
	 * be withdrawn, which is the failure this whole mechanism exists to
	 * avoid. Recorded so the withdrawal pass can stand down instead: a
	 * stale entity is a blemish, a rejected document is every entity.
	 */
	if (s->count >= HA_KEYS_MAX) {
		s->overflowed = true;
		RSS_WARN("ha: more than %d components in one document, "
			 "entity removal suspended",
			 HA_KEYS_MAX);
		return;
	}
	rss_strlcpy(s->key[s->count++], key, HA_KEY_MAX);
}

static bool key_has(const key_set_t *s, const char *key)
{
	for (int i = 0; i < s->count; i++) {
		if (strcmp(s->key[i], key) == 0)
			return true;
	}
	return false;
}

static const char *category_name(ha_category_t c)
{
	return c == CAT_DIAGNOSTIC ? "diagnostic" : NULL;
}

/*
 * Diagnostics ship disabled. They are the majority of the entity count and
 * the minority of what anyone looks at, and every one of them costs a row in
 * the entity registry and a state write on each poll whether or not it is
 * ever read. Enabling one is two clicks; wading through a dozen to find the
 * two that matter is the cost of the other default.
 *
 * Everything settable ships enabled, including the restart tier: a control
 * nobody can find is not a safer control, and the interruption a restart costs
 * is carried in the entity's name instead.
 *
 * Home Assistant applies this only when it first creates an entity, so a
 * camera already discovered keeps whatever it has. `raptorctl rmq rediscover`
 * is what re-applies it.
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

	/*
	 * A number read repeatedly, each reading standing on its own —
	 * state_class measurement, which is what makes Home Assistant keep
	 * long-term statistics for it. Without it a reading is recorded only
	 * in short-term history and vanishes at the recorder's purge, so it
	 * cannot be graphed over the days that make an exposure or a gain
	 * curve worth looking at.
	 *
	 * Not for a string reading, and not for a binary_sensor: neither has
	 * a mean worth keeping.
	 */
	bool measurement;

	ha_category_t cat;
	rmq_daemon_t owner; /* component exists only while this daemon runs */

	/* When set, the entity is a binary_sensor rather than a sensor, and
	 * this is the Jinja expression that makes it ON. */
	const char *bin_on;
} ha_entity_t;

/*
 * Units and constraints belong in the name: it is the only string visible
 * without opening the entity, and HA has no help-text field of any kind.
 *
 * Every name stands on its own, because every entity is on the one page: a
 * bare "Bitrate" said which stream it meant only while the stream had a page
 * of its own to say it.
 */
static const ha_entity_t entities[] = {
	/* -- Primary: what an operator actually looks at -- */
	{.key = "rtsp_clients",
	 .measurement = true,
	 .name = "RTSP viewers",
	 .value = "rtsp.clients",
	 .icon = "mdi:account-eye",
	 .cat = CAT_PRIMARY,
	 .owner = RMQ_D_RSD},
	/*
	 * Saved settings a running daemon has not read.
	 *
	 * The one thing a dashboard cannot show by itself: a dropdown that has
	 * moved looks applied whether it is or not, because Home Assistant
	 * echoes what it published. This says which of the two happened, and
	 * the Apply button beside it is what resolves it.
	 *
	 * Owned by no daemon: it describes the difference between the file and
	 * whatever is running, which outlives any one of them.
	 */
	{.key = "changes_pending",
	 .name = "Changes pending restart",
	 .bin_on = "value_json.stale | default([]) | count > 0",
	 .icon = "mdi:content-save-alert",
	 .cat = CAT_DIAGNOSTIC,
	 .owner = RMQ_D_COUNT},
	{.key = "mjpeg_clients",
	 .measurement = true,
	 .name = "MJPEG viewers",
	 .value = "http.mjpeg",
	 .icon = "mdi:account-eye-outline",
	 .cat = CAT_DIAGNOSTIC,
	 .owner = RMQ_D_RHD},
	/*
	 * The MJPEG endpoint, spelled out rather than left to be worked out.
	 *
	 * Home Assistant cannot be given an MJPEG camera over discovery — the
	 * MQTT camera platform takes an image topic and nothing else, and the
	 * MJPEG integration is added by hand and asks for a URL. So the useful
	 * thing the bridge can do is say what to paste: the camera knows its
	 * own address on the network the broker is on, and whoever is typing
	 * generally does not.
	 *
	 * Without the credential. That integration has its own username and
	 * password fields, and this value is read by a person.
	 */
	{.key = "mjpeg_url",
	 .name = "MJPEG stream",
	 .value = "http.mjpeg_url",
	 .icon = "mdi:link-variant",
	 .cat = CAT_DIAGNOSTIC,
	 .owner = RMQ_D_RHD},

	/* -- Diagnostic: video. Anything settable is a control rather than a
	 *    sensor, so what is left here is only what the camera reports back
	 *    and no one sets: what the encoder actually delivered against its
	 *    target. -- */
	{.key = "stream0_avg_bitrate",
	 .measurement = true,
	 .name = "Main stream bitrate (actual)",
	 .value = "stream0.avg_bitrate",
	 .unit = "bit/s",
	 .dev_class = "data_rate",
	 .cat = CAT_DIAGNOSTIC,
	 .owner = RMQ_D_RVD},
	{.key = "stream1_avg_bitrate",
	 .measurement = true,
	 .name = "Sub stream bitrate (actual)",
	 .value = "stream1.avg_bitrate",
	 .unit = "bit/s",
	 .dev_class = "data_rate",
	 .cat = CAT_DIAGNOSTIC,
	 .owner = RMQ_D_RVD},

	/* -- Day/night: the decision, and the exposure readings behind it.
	 *    Raw platform units, not lux or seconds: total_gain is the
	 *    vendor's own scale (SigmaStar x1024), and presenting it as
	 *    anything else would invent precision. -- */
	{.key = "ir_state",
	 .name = "Day/night",
	 .value = "ir.state",
	 .icon = "mdi:theme-light-dark",
	 .cat = CAT_PRIMARY,
	 .owner = RMQ_D_RIC},
	{.key = "total_gain",
	 .measurement = true,
	 .name = "Total gain (raw)",
	 .value = "ir.total_gain",
	 .icon = "mdi:brightness-6",
	 .cat = CAT_PRIMARY,
	 .owner = RMQ_D_RIC},
	{.key = "ae_luma",
	 .measurement = true,
	 .name = "AE luma",
	 .value = "ir.ae_luma",
	 .icon = "mdi:brightness-percent",
	 .cat = CAT_PRIMARY,
	 .owner = RMQ_D_RIC},
	{.key = "exposure_us",
	 .measurement = true,
	 .name = "Exposure",
	 .value = "ir.exposure_us",
	 .unit = "μs",
	 .icon = "mdi:camera-iris",
	 .cat = CAT_PRIMARY,
	 .owner = RMQ_D_RIC},

	/* Whether a viewer is actually challenged. Worth an entity of its own
	 * because setting a password is not the same as auth being on: rsd
	 * needs both keys and only reads them at start, so the two text
	 * controls can both look filled in while the server still lets anyone
	 * in. This is the half that says it took.
	 *
	 * Inverted on purpose: device_class lock reads ON as *unlocked*, so
	 * the expression is true when there is no auth. */
	{.key = "rtsp_auth",
	 .name = "RTSP authentication",
	 .icon = "mdi:lock",
	 .dev_class = "lock",
	 .cat = CAT_DIAGNOSTIC,
	 .owner = RMQ_D_RSD,
	 .bin_on = "not (value_json.rtsp.auth | default(false))"},

	/* -- Diagnostic: system. Owner RMQ_D_COUNT means "always present". -- */
	/* Why a control can read back its old value: the restart tier writes
	 * the file, and nothing changes until the daemon has re-read it. */
	{.key = "restart_pending",
	 .name = "Restart pending",
	 .icon = "mdi:restart-alert",
	 .cat = CAT_DIAGNOSTIC,
	 .owner = RMQ_D_COUNT,
	 .bin_on = "value_json.restart.pending | default(false)"},
	{.key = "daemons_up",
	 .measurement = true,
	 .name = "Daemons running",
	 .value = "daemons_up",
	 .icon = "mdi:cogs",
	 .cat = CAT_DIAGNOSTIC,
	 .owner = RMQ_D_COUNT},
	{.key = "uptime",
	 .measurement = true,
	 .name = "Uptime",
	 .value = "uptime",
	 .unit = "s",
	 .dev_class = "duration",
	 .cat = CAT_DIAGNOSTIC,
	 .owner = RMQ_D_COUNT},
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
	CTRL_TEXT,
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

	/* CTRL_SELECT, NULL-terminated. Required for that kind. */
	const char *const *options;

	/*
	 * Using this control costs an interruption to video or audio, which is
	 * not what a control sitting on a device page looks like it will do. It
	 * says so in its name, which is the only warning Home Assistant has
	 * anywhere to put.
	 */
	bool restarts;

	/*
	 * An [image] key this control needs the platform to have. The entity is
	 * published only while the camera lists it as settable, because an ISP
	 * block a SoC does not have still reads back a plausible number: the
	 * control would look like it worked and change nothing at all.
	 */
	const char *cap;

	/* HA device_class, NULL for none. On a button it is what decides how
	 * the action is drawn — `restart` reads as disruptive where the
	 * default reads as another press. */
	const char *dev_class;

	/* CTRL_TEXT: the value is a secret, so the field is drawn masked. It
	 * is a rendering choice and no part of the protection — what actually
	 * keeps the value out of sight is that nothing reports it. */
	bool secret;
} ha_control_t;

/* Membership in the comma-terminated list rvd reports. Empty means the camera
 * said nothing, which is treated as "everything" — an older rvd that does not
 * publish the list should not lose every image control. */
static bool isp_settable(const struct rmq_state *st, const char *key)
{
	if (st->isp_settable[0] == '\0')
		return true;

	char needle[48];
	snprintf(needle, sizeof(needle), ",%s,", key);
	return strstr(st->isp_settable, needle) != NULL;
}

/* The camera's own bound for a knob, if it published one. Leaves the caller's
 * fallback in place otherwise, so an older rvd keeps the compiled-in guess
 * rather than losing the control. */
static void isp_caps_range(const struct rmq_state *st, const char *key, int *lo, int *hi)
{
	if (!key)
		return;

	for (int i = 0; i < st->isp_caps_count; i++) {
		if (strcmp(st->isp_caps[i].key, key) != 0)
			continue;
		*lo = st->isp_caps[i].min;
		*hi = st->isp_caps[i].max;
		return;
	}
}

bool rmq_ha_note_camera(struct rmq_state *st, const cJSON *state)
{
	bool changed = false;

	/* Whether there is a picture to offer at all. A change here adds or
	 * removes the image component, so it belongs among the facts that
	 * force a discovery republish. */
	int jpeg = 0;
	const cJSON *jc = cJSON_GetObjectItemCaseSensitive(state, "jpeg");
	const cJSON *jn = jc ? cJSON_GetObjectItemCaseSensitive(jc, "channels") : NULL;
	if (cJSON_IsNumber(jn))
		jpeg = jn->valueint;

	if (jpeg != st->jpeg_channels) {
		st->jpeg_channels = jpeg;
		changed = true;
	}

	char settable[sizeof(st->isp_settable)] = "";
	const cJSON *image = cJSON_GetObjectItemCaseSensitive(state, "image");
	const cJSON *s = image ? cJSON_GetObjectItemCaseSensitive(image, "settable") : NULL;
	if (cJSON_IsString(s) && s->valuestring)
		rss_strlcpy(settable, s->valuestring, sizeof(settable));

	/*
	 * The ranges, cached the same way and compared the same way: a change
	 * here changes what every image entity offers, so it has to force a
	 * discovery republish just as the settable list does. A tuning reload
	 * can move them -- the neutral of a knob whose baseline is learned
	 * comes from the binary.
	 */
	{
		const cJSON *caps = image ? cJSON_GetObjectItemCaseSensitive(image, "caps") : NULL;
		const cJSON *k;
		int n = 0;

		cJSON_ArrayForEach(k, caps)
		{
			const cJSON *lo = cJSON_GetObjectItemCaseSensitive(k, "min");
			const cJSON *hi = cJSON_GetObjectItemCaseSensitive(k, "max");

			if (n >= (int)(sizeof(st->isp_caps) / sizeof(st->isp_caps[0])))
				break;
			if (!k->string || !cJSON_IsNumber(lo) || !cJSON_IsNumber(hi))
				continue;
			if (strcmp(st->isp_caps[n].key, k->string) != 0 ||
			    st->isp_caps[n].min != lo->valueint ||
			    st->isp_caps[n].max != hi->valueint)
				changed = true;
			rss_strlcpy(st->isp_caps[n].key, k->string, sizeof(st->isp_caps[n].key));
			st->isp_caps[n].min = lo->valueint;
			st->isp_caps[n].max = hi->valueint;
			n++;
		}
		if (n != st->isp_caps_count)
			changed = true;
		st->isp_caps_count = n;
	}

	if (strcmp(settable, st->isp_settable) != 0) {
		rss_strlcpy(st->isp_settable, settable, sizeof(st->isp_settable));
		changed = true;
	}

	return changed;
}

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
/*
 * Four families where every member has the same shape and only the name, the
 * icon and a bound differ. Written out longhand they were forty near-identical
 * blocks, which is forty chances for one of them to point at the wrong key.
 */

/* [image]: a live rvd command, a reading back from the ISP, a ceiling. */
#define ISP_NUM(k, nm, ic, hi)                                                                     \
	{.key = "image_" k,                                                                        \
	 .name = nm,                                                                               \
	 .kind = CTRL_NUMBER,                                                                      \
	 .icon = ic,                                                                               \
	 .cat = CAT_PRIMARY,                                                                       \
	 .owner = RMQ_D_RVD,                                                                       \
	 .cap = k,                                                                                 \
	 .value = "image." k,                                                                      \
	 .cmd_tpl = "{\"cmd\":\"set\",\"section\":\"image\",\"key\":\"" k "\","                    \
		    "\"value\":{{ value | int }}}",                                                \
	 .min = 0,                                                                                 \
	 .max = hi,                                                                                \
	 .step = 1}

/*
 * [image] keys some parts take only while the ISP channel is being created.
 * SigmaStar's SetChnParam carries orientation and the 3DNR level together and
 * a running channel refuses all three, so these go through the file and a
 * restart instead — where they are applied at channel creation, which is the
 * one moment the SDK accepts them.
 *
 * Restart tier on every platform, not only the ones that need it. Orientation
 * is set once when the camera is mounted and noise reduction is not far
 * behind, so a uniform behaviour is worth more here than saving a restart on
 * the parts that would take it live.
 */
#define IMG_CFG_NUM(k, nm, ic, hi)                                                                 \
	{.key = "image_" k,                                                                        \
	 .name = nm,                                                                               \
	 .kind = CTRL_NUMBER,                                                                      \
	 .icon = ic,                                                                               \
	 .cat = CAT_PRIMARY,                                                                       \
	 .owner = RMQ_D_RVD,                                                                       \
	 .cap = k,                                                                                 \
	 .value = "image." k,                                                                      \
	 .cmd_tpl = "{\"cmd\":\"set\",\"section\":\"image\",\"key\":\"" k "\","                    \
		    "\"value\":{{ value | int }}}",                                                \
	 .min = 0,                                                                                 \
	 .max = hi,                                                                                \
	 .step = 1,                                                                                \
	 .restarts = true}

#define IMG_CFG_SW(k, nm, ic)                                                                      \
	{.key = "image_" k,                                                                        \
	 .name = nm,                                                                               \
	 .kind = CTRL_SWITCH,                                                                      \
	 .icon = ic,                                                                               \
	 .cat = CAT_PRIMARY,                                                                       \
	 .owner = RMQ_D_RVD,                                                                       \
	 .cap = k,                                                                                 \
	 .value = "image." k,                                                                      \
	 .payload = "{\"cmd\":\"set\",\"section\":\"image\",\"key\":\"" k "\",\"value\":1}",       \
	 .payload_off = "{\"cmd\":\"set\",\"section\":\"image\",\"key\":\"" k "\","                \
			"\"value\":0}",                                                            \
	 .restarts = true}

/*
 * What a dashboard can do to the camera.
 *
 * Every setting that only ever gets typed once — geometry, codecs, ports,
 * credentials, the day/night wiring, the audio front end — is the web
 * console's, and none of it is here. Two interfaces offering the same key is
 * two answers to the question of what the camera is set to, and the loser is
 * whoever reads the stale one.
 *
 * What survives is what a dashboard is actually for: the picture controls
 * somebody adjusts while looking at the picture, and the two actions that
 * finish a job started elsewhere.
 */
static const ha_control_t controls[] = {
	/* ---- The camera itself ---- */
	{.key = "osd_enabled",
	 .name = "OSD",
	 .kind = CTRL_SWITCH,
	 .icon = "mdi:format-text",
	 .cat = CAT_PRIMARY,
	 .owner = RMQ_D_ROD,
	 .value = "osd.enabled",
	 .payload = "{\"cmd\":\"action\",\"action\":\"osd-enable\"}",
	 .payload_off = "{\"cmd\":\"action\",\"action\":\"osd-disable\"}"},

	/*
	 * Enact the saved settings, and nothing else.
	 *
	 * Kept although the settings themselves went to the console, because a
	 * saved edit is not an applied one wherever it was made: the console,
	 * raptorctl and the restart-tier switches below all leave the camera
	 * holding changes nothing has read yet. "Changes pending restart" is
	 * how that shows, and this is what resolves it.
	 *
	 * A no-op when nothing is pending, so it is safe to press twice.
	 */
	{.key = "apply",
	 .name = "Apply pending changes",
	 .kind = CTRL_BUTTON,
	 .icon = "mdi:content-save-cog",
	 .cat = CAT_PRIMARY,
	 .owner = RMQ_D_COUNT,
	 .payload = "{\"cmd\":\"apply\"}"},
	/* device_class restart is what makes Home Assistant draw it as the
	 * disruptive action it is rather than as another button. */
	{.key = "reboot",
	 .name = "Reboot camera",
	 .kind = CTRL_BUTTON,
	 .cat = CAT_PRIMARY,
	 .owner = RMQ_D_COUNT,
	 .dev_class = "restart",
	 .payload = "{\"cmd\":\"reboot\"}"},
	{.key = "request_idr",
	 .name = "Request keyframe",
	 .kind = CTRL_BUTTON,
	 .icon = "mdi:image-refresh",
	 .cat = CAT_DIAGNOSTIC,
	 .owner = RMQ_D_RVD,
	 .payload = "{\"cmd\":\"action\",\"action\":\"request-idr\",\"channel\":0}"},

	/* ---- Image: the ISP tuning ----
	 *
	 * Every one is live and persists, because tuning is done by looking at
	 * the picture — a restart between adjustments would make it guesswork.
	 * They read back from the ISP rather than the config, so a platform
	 * that ignores a block shows it unchanged rather than pretending.
	 */
	ISP_NUM("brightness", "Brightness", "mdi:brightness-6", 255),
	ISP_NUM("contrast", "Contrast", "mdi:contrast-circle", 255),
	ISP_NUM("saturation", "Saturation", "mdi:palette", 255),
	ISP_NUM("sharpness", "Sharpness", "mdi:triangle-outline", 255),
	ISP_NUM("hue", "Hue", "mdi:palette-swatch", 255),
	ISP_NUM("sinter", "Spatial noise reduction", "mdi:blur", 255),
	ISP_NUM("ae_comp", "AE compensation", "mdi:brightness-auto", 255),
	ISP_NUM("max_again", "Max analog gain", "mdi:signal", 160),
	ISP_NUM("max_dgain", "Max digital gain", "mdi:signal-variant", 160),
	ISP_NUM("dpc_strength", "Dead pixel correction", "mdi:grain", 255),
	ISP_NUM("drc_strength", "Dynamic range compression", "mdi:gradient-vertical", 255),
	ISP_NUM("defog_strength", "Defog", "mdi:weather-fog", 255),
	ISP_NUM("highlight_depress", "Highlight depression", "mdi:white-balance-sunny", 255),
	ISP_NUM("backlight_comp", "Backlight compensation", "mdi:brightness-4", 10),

	IMG_CFG_NUM("temper", "Temporal noise reduction", "mdi:blur-linear", 255),
	IMG_CFG_SW("hflip", "Flip horizontally", "mdi:flip-horizontal"),
	IMG_CFG_SW("vflip", "Flip vertically", "mdi:flip-vertical"),
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
			  ha_category_t cat, bool enabled, const char *platform)
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
	if (!enabled)
		cJSON_AddBoolToObject(c, "en", false);

	return c;
}

/* Build one sensor entry. */
static cJSON *make_component(struct rmq_state *st, const ha_entity_t *e)
{
	cJSON *c = make_common(st, e->key, e->name, e->icon, e->cat, enabled_by_default(e->cat),
			       e->bin_on ? "binary_sensor" : "sensor");
	if (!c)
		return NULL;

	char tpl[192];
	if (e->bin_on)
		snprintf(tpl, sizeof(tpl), "{{ 'ON' if %s else 'OFF' }}", e->bin_on);
	else
		snprintf(tpl, sizeof(tpl), "{{ value_json.%s | default('') }}", e->value);

	cJSON_AddStringToObject(c, "stat_t", st->topic_state);
	cJSON_AddStringToObject(c, "val_tpl", tpl);

	if (e->unit)
		cJSON_AddStringToObject(c, "unit_of_meas", e->unit);
	if (e->dev_class)
		cJSON_AddStringToObject(c, "dev_cla", e->dev_class);
	if (e->measurement)
		cJSON_AddStringToObject(c, "stat_cla", "measurement");

	return c;
}

/* Build one control entry. */
static cJSON *make_control(struct rmq_state *st, const ha_control_t *ct)
{
	static const char *const platforms[] = {"number", "select", "switch", "button", "text"};

	/*
	 * The restart tier says so where it will be read. Home Assistant has no
	 * help text and no warning on a control, so the name is the only place
	 * left to put the one thing an operator wants to know before touching
	 * it: that this one interrupts the video.
	 */
	char name[96];
	if (ct->restarts)
		snprintf(name, sizeof(name), "%s (restart)", ct->name);
	else
		rss_strlcpy(name, ct->name, sizeof(name));

	cJSON *c = make_common(st, ct->key, name, ct->icon, ct->cat, enabled_by_default(ct->cat),
			       platforms[ct->kind]);
	if (!c)
		return NULL;

	cJSON_AddStringToObject(c, "cmd_t", st->topic_cmd);
	if (ct->dev_class)
		cJSON_AddStringToObject(c, "dev_cla", ct->dev_class);

	if (ct->value) {
		char tpl[192];
		switch (ct->kind) {
		case CTRL_SWITCH:
			/* Rendered to a fixed pair rather than left as Jinja's
			 * "True"/"False", so what the switch compares against
			 * is stated once here and once in stat_on/stat_off
			 * below rather than depending on Jinja's spelling. */
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
	case CTRL_NUMBER: {
		int lo = ct->min, hi = ct->max;

		/* The camera's answer beats the table's guess where there is
		 * one; the table stays as the fallback for a camera too old to
		 * publish caps at all. */
		isp_caps_range(st, ct->cap, &lo, &hi);
		cJSON_AddNumberToObject(c, "min", lo);
		cJSON_AddNumberToObject(c, "max", hi);
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
	}
	case CTRL_TEXT:
		/* A length cap and nothing else. What the value may contain is
		 * enforced where it is applied, not here: a pattern in the
		 * discovery document is a convenience for the person typing,
		 * never the check that matters. */
		cJSON_AddNumberToObject(c, "max", ct->max);
		if (ct->secret)
			cJSON_AddStringToObject(c, "mode", "password");
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
		/*
		 * Home Assistant defaults state_on to payload_on, which here is
		 * a command document the camera has no reason to ever echo. Left
		 * to the default the reported state matches neither, the entity
		 * sits in `unknown` forever, and Home Assistant draws it as a
		 * pair of lightning-bolt buttons rather than a toggle — having
		 * no position it could honestly show.
		 */
		cJSON_AddStringToObject(c, "stat_on", "ON");
		cJSON_AddStringToObject(c, "stat_off", "OFF");
		break;
	case CTRL_BUTTON:
		cJSON_AddStringToObject(c, "pl_prs", ct->payload);
		break;
	}

	return c;
}

static int publish_device(struct rmq_state *st, const rmq_daemons_t *now)
{
	key_set_t present = {0};
	cJSON *root = cJSON_CreateObject();
	if (!root)
		return -1;

	/* Device identity. All components collapse under this on one page. */
	cJSON *dev = cJSON_AddObjectToObject(root, "dev");
	cJSON_AddStringToObject(dev, "ids", st->client_id);
	cJSON_AddStringToObject(dev, "name", st->device_name);

	/*
	 * rhd's own page, which Home Assistant renders as "Visit device" on
	 * the device page — the one place a link to the camera itself belongs,
	 * and where the MJPEG stream is. Left out rather than pointing at a
	 * port nothing is listening on when rhd is absent, and without the
	 * credential because a person is going to click it.
	 */
	char url[RMQ_URL_MAX];
	if (rmq_snapshot_url(st, "/", false, url, sizeof(url)) == 0)
		cJSON_AddStringToObject(dev, "cu", url);

	cJSON_AddStringToObject(dev, "mf", "Raptor");
	/*
	 * The model field is the line Home Assistant prints under the device
	 * name, and every raptor camera would otherwise print the same word
	 * there. The address is what tells two of them apart — and it is the
	 * thing anyone reading the page next wants anyway, since the console
	 * is a click away from it.
	 */
	char subtitle[INET6_ADDRSTRLEN];
	if (st->subtitle[0])
		cJSON_AddStringToObject(dev, "mdl", st->subtitle);
	else if (rmq_local_addr(st, subtitle, sizeof(subtitle)) == 0)
		cJSON_AddStringToObject(dev, "mdl", subtitle);
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

		if (!owner_available(e->owner, now))
			continue;

		cJSON *c = make_component(st, e);
		if (c) {
			cJSON_AddItemToObject(cmps, e->key, c);
			key_add(&present, e->key);
			published++;
		}
	}

	/*
	 * Controls only exist while commands are accepted. A read-only bridge
	 * that still advertised them would offer a slider that silently does
	 * nothing, which is worse than not offering it.
	 */
	/*
	 * The picture. Its own platform rather than a row in either table: an
	 * image component carries a URL topic and nothing else, with no state
	 * template, no unit and no command.
	 *
	 * The URL is what travels, not the JPEG — Home Assistant fetches from
	 * rhd, so the broker carries a couple of hundred bytes on a refresh
	 * rather than the whole frame, and the picture is as fresh as the
	 * fetch rather than as fresh as the last publish. It needs rhd for the
	 * same reason.
	 *
	 * Offered when the camera can actually serve one: rhd listening and
	 * rvd encoding JPEG. Both are asked rather than assumed, so a camera
	 * with [jpeg] off gets no tile instead of a tile that 404s, and
	 * neither has to be restated in [mqtt].
	 */
	bool offer_picture =
		st->snapshot_enabled && st->jpeg_channels > 0 && owner_available(RMQ_D_RHD, now);
	cJSON *img = offer_picture ? make_common(st, "picture", "Picture", "mdi:camera",
						 CAT_PRIMARY, true, "image")
				   : NULL;
	if (img) {
		cJSON_AddStringToObject(img, "url_t", st->topic_snapshot);
		cJSON_AddStringToObject(img, "url_tpl", "{{ value_json.snapshot }}");
		cJSON_AddItemToObject(cmps, "picture", img);
		key_add(&present, "picture");
		published++;
	}

	for (int i = 0; i < control_count; i++) {
		const ha_control_t *ct = &controls[i];

		bool live = st->commands_enabled && owner_available(ct->owner, now);

		/* An ISP block this part does not have. The daemon is up and
		 * the command would be accepted; it is the silicon underneath
		 * that has nothing to change. */
		if (ct->cap && !isp_settable(st, ct->cap))
			live = false;

		if (!live)
			continue;

		cJSON *c = make_control(st, ct);
		if (c) {
			cJSON_AddItemToObject(cmps, ct->key, c);
			key_add(&present, ct->key);
			published++;
		}
	}

	/*
	 * Everything the last document carried and this one does not. An empty
	 * object is how Home Assistant is told to drop an entity, and without
	 * it a daemon that stopped leaves dead rows showing their last value
	 * forever.
	 *
	 * Driven by what was published rather than by why it is gone, which is
	 * what makes it safe: a component absent because its daemon is down and
	 * one absent because the silicon never had it are the same thing from
	 * here, and only the first was ever in a document.
	 */
	for (int i = 0; !present.overflowed && i < published_count; i++) {
		if (!key_has(&present, published_keys[i]))
			cJSON_AddItemToObject(cmps, published_keys[i], cJSON_CreateObject());
	}

	char *payload = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!payload)
		return -1;

	int rc = rmq_mqtt_publish(st->mqtt, st->topic_discovery, payload, strlen(payload), 1, true);

	if (rc < 0) {
		RSS_WARN("ha: discovery not published, %zu bytes", strlen(payload));
	} else {
		RSS_INFO("ha: discovery published, %d entities, %zu bytes", published,
			 strlen(payload));

		/* Only once it is on the broker. A document that failed to
		 * publish did not change what Home Assistant holds, and
		 * recording it would lose the withdrawal for anything dropped
		 * by the attempt. */
		memcpy(published_keys, present.key, sizeof(published_keys));
		published_count = present.count;
	}
	free(payload);

	return rc;
}

int rmq_ha_publish_discovery(struct rmq_state *st, const rmq_daemons_t *now)
{
	int rc = publish_device(st, now);

	for (int i = 0; retired_devices[i]; i++) {
		char topic[RMQ_TOPIC_MAX];
		snprintf(topic, sizeof(topic), "%s/device/%s_%s/config", st->discovery_prefix,
			 st->client_id, retired_devices[i]);
		rmq_mqtt_publish(st->mqtt, topic, "", 0, 1, true);
	}

	return rc;
}

int rmq_ha_clear_discovery(struct rmq_state *st)
{
	int rc = rmq_mqtt_publish(st->mqtt, st->topic_discovery, "", 0, 1, true) < 0 ? -1 : 0;

	/*
	 * Home Assistant now holds nothing for this device, so neither does the
	 * record of what it holds. Without this the next document would
	 * withdraw whatever the last one carried and is no longer offered —
	 * naming components that were just deleted, which is precisely what
	 * costs the whole document.
	 */
	published_count = 0;
	return rc;
}
