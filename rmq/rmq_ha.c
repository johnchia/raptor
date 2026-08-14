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

/*
 * Which device page an entity lands on.
 *
 * Only the two encoded streams are separate devices. They earn it by being
 * genuinely parallel — the same six entities twice, where the page title is
 * the only thing distinguishing "Bitrate" from "Bitrate". Everything else
 * describes one camera and belongs on the camera's own page, sorted by the
 * category above; splitting those out bought a tidier list at the cost of
 * making an ordinary adjustment a journey through three pages.
 */
typedef enum {
	GRP_CAMERA = 0,
	GRP_MAIN,
	GRP_SUB,
	GRP_COUNT,
} ha_group_t;

static const struct {
	const char *suffix; /* NULL: the camera itself, no via_device */
	const char *name;   /* appended to the camera's name */
} groups[GRP_COUNT] = {
	[GRP_CAMERA] = {NULL, NULL},
	[GRP_MAIN] = {"main", "Main stream"},
	[GRP_SUB] = {"sub", "Sub stream"},
};

/*
 * Sub-devices an earlier build published and this one does not. Their
 * discovery documents are retained on the broker, so Home Assistant would keep
 * the devices and every entity under them indefinitely; an empty payload is
 * how a retained discovery is withdrawn.
 *
 * Deletable once no camera in the fleet still runs a build that published
 * them — they cost one empty publish per discovery cycle until then.
 */
static const char *const retired_groups[] = {"image", "daynight", NULL};

static const char *category_name(ha_category_t c)
{
	return c == CAT_CONFIG ? "config" : c == CAT_DIAGNOSTIC ? "diagnostic" : NULL;
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
	ha_category_t cat;
	rmq_daemon_t owner; /* component exists only while this daemon runs */
	ha_group_t group;

	/* When set, the entity is a binary_sensor rather than a sensor, and
	 * this is the Jinja expression that makes it ON. */
	const char *bin_on;
} ha_entity_t;

/*
 * Units and constraints belong in the name: it is the only string visible
 * without opening the entity, and HA has no help-text field of any kind.
 *
 * A name carries whatever context its page does not. The stream pages supply
 * "Main" and "Sub", so their entities are bare; everything on the camera's own
 * page has to say what it is about, which is why the day/night entities name
 * themselves and the image ones do not need to.
 */
static const ha_entity_t entities[] = {
	/* -- Primary: what an operator actually looks at -- */
	{.key = "rtsp_clients",
	 .name = "RTSP viewers",
	 .value = "rtsp.clients",
	 .icon = "mdi:account-eye",
	 .cat = CAT_PRIMARY,
	 .owner = RMQ_D_RSD},

	/* -- Diagnostic: video. Anything settable is a control rather than a
	 *    sensor, so what is left here is only what the camera reports back
	 *    and no one sets: what the encoder actually delivered against its
	 *    target. -- */
	{.key = "stream0_avg_bitrate",
	 .name = "Bitrate (actual)",
	 .value = "stream0.avg_bitrate",
	 .unit = "bit/s",
	 .dev_class = "data_rate",
	 .cat = CAT_DIAGNOSTIC,
	 .owner = RMQ_D_RVD,
	 .group = GRP_MAIN},
	{.key = "stream1_avg_bitrate",
	 .name = "Bitrate (actual)",
	 .value = "stream1.avg_bitrate",
	 .unit = "bit/s",
	 .dev_class = "data_rate",
	 .cat = CAT_DIAGNOSTIC,
	 .owner = RMQ_D_RVD,
	 .group = GRP_SUB},

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
	 .name = "Total gain (raw)",
	 .value = "ir.total_gain",
	 .icon = "mdi:brightness-6",
	 .cat = CAT_PRIMARY,
	 .owner = RMQ_D_RIC},
	{.key = "ae_luma",
	 .name = "AE luma",
	 .value = "ir.ae_luma",
	 .icon = "mdi:brightness-percent",
	 .cat = CAT_PRIMARY,
	 .owner = RMQ_D_RIC},
	{.key = "exposure_us",
	 .name = "Exposure",
	 .value = "ir.exposure_us",
	 .unit = "µs",
	 .icon = "mdi:camera-iris",
	 .cat = CAT_DIAGNOSTIC,
	 .owner = RMQ_D_RIC},

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
	 .name = "Daemons running",
	 .value = "daemons_up",
	 .icon = "mdi:cogs",
	 .cat = CAT_DIAGNOSTIC,
	 .owner = RMQ_D_COUNT},
	{.key = "uptime",
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
} ha_ctrl_kind_t;

typedef struct {
	const char *key;
	const char *name;
	ha_ctrl_kind_t kind;
	const char *icon;
	ha_category_t cat;
	rmq_daemon_t owner;
	ha_group_t group;

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

	/* CTRL_SELECT, NULL-terminated. NULL itself means the options are
	 * derived at publish time — see res_build below. */
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
} ha_control_t;

static const char *const opt_daynight[] = {"auto", "day", "night", NULL};
static const char *const opt_trigger[] = {"luma", "gain", "adc", "photo", NULL};
static const char *const opt_vcodec[] = {"h264", "h265", NULL};
static const char *const opt_acodec[] = {"pcmu", "pcma", "l16", "aac", "opus", NULL};
static const char *const opt_arate[] = {"8000", "16000", "32000", "48000", NULL};

/*
 * The resolution list, derived from the sensor rather than written down.
 *
 * A sensor's sizes are its own. 2560x1920 is 4:3 and 1920x1080 is 16:9, and a
 * fixed list written for one of them crops or stretches on the other — which
 * is what the list here used to do on every 4:3 camera in the fleet. So the
 * ladder is the native size and fractions of it: the aspect ratio survives
 * whatever the sensor is, and the sizes anyone recognises fall out of the
 * arithmetic anyway, since two thirds of 1080p is 720p.
 *
 * Fractions rather than real modes because nothing on the camera enumerates
 * modes: rvd's get-enc-caps carries feature flags and no geometry at all. What
 * the native size does buy is the ceiling — the scaler will produce any size
 * below it, and cannot invent one above.
 *
 * A list rather than a pair of width/height boxes, because a free-typed size
 * the ISP cannot produce fails at pipeline init: after the restart, with no
 * picture left to explain it.
 */
#define RES_OPT_MAX 10

typedef struct {
	int w[RES_OPT_MAX], h[RES_OPT_MAX];
	char text[RES_OPT_MAX][16];
	const char *opt[RES_OPT_MAX + 1]; /* NULL-terminated, for the entity */
	int count;
} res_list_t;

/*
 * Encoders want even dimensions and the hardware scaler is happiest on a
 * multiple of 8. Nearest rather than down, so half of 1080 is 544 and not 536
 * — both are legal and the first is the one encoders are built around.
 */
static int align8(int v)
{
	return ((v + 4) / 8) * 8;
}

/* Insert descending by area: Home Assistant shows options in the order given,
 * and a list that is not sorted reads as a list that is not thought about. */
static void res_add(res_list_t *l, int w, int h)
{
	if (w < 160 || h < 120 || l->count >= RES_OPT_MAX)
		return;

	for (int i = 0; i < l->count; i++) {
		if (l->w[i] == w && l->h[i] == h)
			return;
	}

	int at = l->count;
	while (at > 0 && l->w[at - 1] * l->h[at - 1] < w * h) {
		l->w[at] = l->w[at - 1];
		l->h[at] = l->h[at - 1];
		rss_strlcpy(l->text[at], l->text[at - 1], sizeof(l->text[at]));
		at--;
	}

	l->w[at] = w;
	l->h[at] = h;
	snprintf(l->text[at], sizeof(l->text[at]), "%dx%d", w, h);
	l->count++;
}

static void res_build(res_list_t *l, const struct rmq_state *st)
{
	/* Halves, thirds and quarters, plus the two ratios that land on sizes
	 * people ask for by name. Native is added unrounded: that one size is
	 * the sensor's own and not ours to tidy. */
	static const struct {
		int num, den;
	} fracs[] = {{3, 4}, {2, 3}, {1, 2}, {1, 3}, {1, 4}};

	memset(l, 0, sizeof(*l));

	int w = st->sensor_width, h = st->sensor_height;
	if (w > 0 && h > 0) {
		res_add(l, w, h);
		for (size_t i = 0; i < sizeof(fracs) / sizeof(fracs[0]); i++)
			res_add(l, align8(w * fracs[i].num / fracs[i].den),
				align8(h * fracs[i].num / fracs[i].den));
	}

	/*
	 * Whatever the streams are actually running, which need not be one of
	 * ours: a size set from the config file or by raptorctl is just as
	 * real. Home Assistant matches state against the option list exactly,
	 * so without this the control shows blank on the camera it describes.
	 */
	for (int i = 0; i < RMQ_STREAM_COUNT; i++) {
		int sw = 0, sh = 0;
		if (sscanf(st->stream_res[i], "%dx%d", &sw, &sh) == 2)
			res_add(l, sw, sh);
	}

	for (int i = 0; i < l->count; i++)
		l->opt[i] = l->text[i];
	l->opt[l->count] = NULL;
}

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

bool rmq_ha_note_camera(struct rmq_state *st, const cJSON *state)
{
	int w = 0, h = 0;
	const cJSON *sensor = cJSON_GetObjectItemCaseSensitive(state, "sensor");
	if (cJSON_IsObject(sensor)) {
		const cJSON *jw = cJSON_GetObjectItemCaseSensitive(sensor, "width");
		const cJSON *jh = cJSON_GetObjectItemCaseSensitive(sensor, "height");
		if (cJSON_IsNumber(jw) && cJSON_IsNumber(jh)) {
			w = jw->valueint;
			h = jh->valueint;
		}
	}

	bool changed = (w != st->sensor_width || h != st->sensor_height);
	st->sensor_width = w;
	st->sensor_height = h;

	for (int i = 0; i < RMQ_STREAM_COUNT; i++) {
		char key[16], res[16] = "";
		snprintf(key, sizeof(key), "stream%d", i);

		const cJSON *s = cJSON_GetObjectItemCaseSensitive(state, key);
		const cJSON *r = s ? cJSON_GetObjectItemCaseSensitive(s, "resolution") : NULL;
		if (cJSON_IsString(r) && r->valuestring)
			rss_strlcpy(res, r->valuestring, sizeof(res));

		if (strcmp(res, st->stream_res[i]) != 0) {
			rss_strlcpy(st->stream_res[i], res, sizeof(st->stream_res[i]));
			changed = true;
		}
	}

	char settable[sizeof(st->isp_settable)] = "";
	const cJSON *image = cJSON_GetObjectItemCaseSensitive(state, "image");
	const cJSON *s = image ? cJSON_GetObjectItemCaseSensitive(image, "settable") : NULL;
	if (cJSON_IsString(s) && s->valuestring)
		rss_strlcpy(settable, s->valuestring, sizeof(settable));

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
#define ISP_NUM(k, nm, cmd, ic, hi)                                                                \
	{.key = "image_" k,                                                                        \
	 .name = nm,                                                                               \
	 .kind = CTRL_NUMBER,                                                                      \
	 .icon = ic,                                                                               \
	 .cat = CAT_PRIMARY,                                                                       \
	 .owner = RMQ_D_RVD,                                                                       \
	 .cap = k,                                                                                 \
	 .value = "image." k,                                                                      \
	 .cmd_tpl = "{\"cmd\":\"" cmd "\",\"value\":{{ value | int }}}",                           \
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
	 .cmd_tpl = "{\"cmd\":\"config-set\",\"section\":\"image\",\"key\":\"" k "\","             \
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
	 .payload = "{\"cmd\":\"config-set\",\"section\":\"image\",\"key\":\"" k "\","             \
		    "\"value\":1}",                                                                \
	 .payload_off = "{\"cmd\":\"config-set\",\"section\":\"image\",\"key\":\"" k "\","         \
			"\"value\":0}",                                                            \
	 .restarts = true}

/* [ircut] thresholds ric applies and records live. */
#define IRC_LIVE(k, nm, ic, hi)                                                                    \
	{.key = "ircut_" k,                                                                        \
	 .name = nm,                                                                               \
	 .kind = CTRL_NUMBER,                                                                      \
	 .icon = ic,                                                                               \
	 .cat = CAT_CONFIG,                                                                        \
	 .owner = RMQ_D_RIC,                                                                       \
	 .value = "ir." k,                                                                         \
	 .cmd_tpl = "{\"cmd\":\"ircut-threshold\",\"key\":\"" k "\","                              \
		    "\"value\":{{ value | int }}}",                                                \
	 .min = 0,                                                                                 \
	 .max = hi,                                                                                \
	 .step = 1}

/*
 * [ircut] keys ric reads only at startup: a config write and a restart. ric
 * reports none of them back, so the control is write-only and shows blank
 * until it is set — which is honest, and the alternative would be echoing the
 * value we sent as though the daemon had confirmed it.
 */
#define IRC_CFG(k, nm, ic, lo, hi)                                                                 \
	{.key = "ircut_" k,                                                                        \
	 .name = nm,                                                                               \
	 .kind = CTRL_NUMBER,                                                                      \
	 .icon = ic,                                                                               \
	 .cat = CAT_CONFIG,                                                                        \
	 .owner = RMQ_D_RIC,                                                                       \
	 .cmd_tpl = "{\"cmd\":\"config-set\",\"section\":\"ircut\",\"key\":\"" k "\","             \
		    "\"value\":{{ value | int }}}",                                                \
	 .min = lo,                                                                                \
	 .max = hi,                                                                                \
	 .step = 1,                                                                                \
	 .restarts = true}

/*
 * One encoded stream. `n` is a string so it can be pasted into both the
 * component key and the channel number inside the command template — the
 * concatenation puts the digit into the JSON unquoted, which is what the
 * command expects.
 */
#define STREAM_CTRLS(n, grp)                                                                       \
	{.key = "stream" n "_bitrate_set",                                                         \
	 .name = "Bitrate",                                                                        \
	 .kind = CTRL_NUMBER,                                                                      \
	 .cat = CAT_CONFIG,                                                                        \
	 .owner = RMQ_D_RVD,                                                                       \
	 .group = grp,                                                                             \
	 .value = "stream" n ".bitrate",                                                           \
	 .cmd_tpl = "{\"cmd\":\"set-bitrate\",\"channel\":" n ",\"value\":{{ value | int }}}",     \
	 .min = 100000,                                                                            \
	 .max = 50000000,                                                                          \
	 .step = 100000,                                                                           \
	 .unit = "bit/s"},                                                                         \
		{.key = "stream" n "_fps_set",                                                     \
		 .name = "Frame rate",                                                             \
		 .kind = CTRL_NUMBER,                                                              \
		 .icon = "mdi:filmstrip",                                                          \
		 .cat = CAT_CONFIG,                                                                \
		 .owner = RMQ_D_RVD,                                                               \
		 .group = grp,                                                                     \
		 .value = "stream" n ".fps",                                                       \
		 .cmd_tpl = "{\"cmd\":\"set-fps\",\"channel\":" n ",\"value\":{{ value | int }}}", \
		 .min = 5,                                                                         \
		 .max = 60,                                                                        \
		 .step = 5,                                                                        \
		 .unit = "fps"},                                                                   \
		{.key = "stream" n "_gop_set",                                                     \
		 .name = "GOP",                                                                    \
		 .kind = CTRL_NUMBER,                                                              \
		 .icon = "mdi:key-variant",                                                        \
		 .cat = CAT_CONFIG,                                                                \
		 .owner = RMQ_D_RVD,                                                               \
		 .group = grp,                                                                     \
		 .value = "stream" n ".gop",                                                       \
		 .cmd_tpl = "{\"cmd\":\"set-gop\",\"channel\":" n ",\"value\":{{ value | int }}}", \
		 .min = 5,                                                                         \
		 .max = 300,                                                                       \
		 .step = 5},                                                                       \
		{.key = "stream" n "_resolution_set",                                              \
		 .name = "Resolution",                                                             \
		 .kind = CTRL_SELECT,                                                              \
		 .icon = "mdi:television",                                                         \
		 .cat = CAT_CONFIG,                                                                \
		 .owner = RMQ_D_RVD,                                                               \
		 .group = grp,                                                                     \
		 .value = "stream" n ".resolution",                                                \
		 .cmd_tpl = "{\"cmd\":\"config-set\",\"section\":\"stream" n "\",\"values\":"      \
			    "{\"width\":{{ value.split('x')[0] | int }},"                          \
			    "\"height\":{{ value.split('x')[1] | int }}}}", /* .options left NULL: \
									       derived from the    \
									       sensor at publish   \
									       time. */            \
		 .restarts = true},                                                                \
	{                                                                                          \
		.key = "stream" n "_codec_set", .name = "Codec", .kind = CTRL_SELECT,              \
		.icon = "mdi:video", .cat = CAT_CONFIG, .owner = RMQ_D_RVD, .group = grp,          \
		.value = "stream" n ".codec",                                                      \
		.cmd_tpl =                                                                         \
			"{\"cmd\":\"config-set\",\"section\":\"stream" n "\",\"key\":\"codec\","   \
			"\"value\":\"{{ value }}\"}",                                              \
		.options = opt_vcodec, .restarts = true                                            \
	}

static const ha_control_t controls[] = {
	/* ---- The camera itself ---- */
	{.key = "osd_enabled",
	 .name = "OSD",
	 .kind = CTRL_SWITCH,
	 .icon = "mdi:format-text",
	 .cat = CAT_PRIMARY,
	 .owner = RMQ_D_ROD,
	 .value = "osd.enabled",
	 .payload = "{\"cmd\":\"osd-enable\"}",
	 .payload_off = "{\"cmd\":\"osd-disable\"}"},

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
	{.key = "audio_codec_set",
	 .name = "Audio codec",
	 .kind = CTRL_SELECT,
	 .icon = "mdi:waveform",
	 .cat = CAT_CONFIG,
	 .owner = RMQ_D_RAD,
	 .value = "audio.codec",
	 .cmd_tpl = "{\"cmd\":\"config-set\",\"section\":\"audio\",\"key\":\"codec\","
		    "\"value\":\"{{ value }}\"}",
	 .options = opt_acodec,
	 .restarts = true},
	{.key = "audio_sample_rate_set",
	 .name = "Audio sample rate",
	 .kind = CTRL_SELECT,
	 .icon = "mdi:sine-wave",
	 .cat = CAT_CONFIG,
	 .owner = RMQ_D_RAD,
	 .value = "audio.sample_rate",
	 .cmd_tpl = "{\"cmd\":\"config-set\",\"section\":\"audio\",\"key\":\"sample_rate\","
		    "\"value\":\"{{ value }}\"}",
	 .options = opt_arate,
	 .restarts = true},
	{.key = "rtsp_port_set",
	 .name = "RTSP port",
	 .kind = CTRL_NUMBER,
	 .icon = "mdi:lan-connect",
	 .cat = CAT_CONFIG,
	 .owner = RMQ_D_RSD,
	 .value = "rtsp.port",
	 .cmd_tpl = "{\"cmd\":\"config-set\",\"section\":\"rtsp\",\"key\":\"port\","
		    "\"value\":{{ value | int }}}",
	 .min = 1,
	 .max = 65535,
	 .step = 1,
	 .restarts = true},
	/* Paired with the camera component below, which shows what it takes. */
	{.key = "snapshot",
	 .name = "Take snapshot",
	 .kind = CTRL_BUTTON,
	 .icon = "mdi:camera",
	 .cat = CAT_PRIMARY,
	 .owner = RMQ_D_RVD,
	 .payload = "{\"cmd\":\"snapshot\"}"},
	{.key = "request_idr",
	 .name = "Request keyframe",
	 .kind = CTRL_BUTTON,
	 .icon = "mdi:image-refresh",
	 .cat = CAT_DIAGNOSTIC,
	 .owner = RMQ_D_RVD,
	 .payload = "{\"cmd\":\"request-idr\",\"channel\":0}"},

	/* ---- Image: the ISP tuning ----
	 *
	 * Every one is live and persists, because tuning is done by looking at
	 * the picture — a restart between adjustments would make it guesswork.
	 * They read back from the ISP rather than the config, so a platform
	 * that ignores a block shows it unchanged rather than pretending.
	 */
	ISP_NUM("brightness", "Brightness", "set-brightness", "mdi:brightness-6", 255),
	ISP_NUM("contrast", "Contrast", "set-contrast", "mdi:contrast-circle", 255),
	ISP_NUM("saturation", "Saturation", "set-saturation", "mdi:palette", 255),
	ISP_NUM("sharpness", "Sharpness", "set-sharpness", "mdi:triangle-outline", 255),
	ISP_NUM("hue", "Hue", "set-hue", "mdi:palette-swatch", 255),
	ISP_NUM("sinter", "Spatial noise reduction", "set-sinter", "mdi:blur", 255),
	ISP_NUM("ae_comp", "AE compensation", "set-ae-comp", "mdi:brightness-auto", 255),
	ISP_NUM("max_again", "Max analog gain", "set-max-again", "mdi:signal", 160),
	ISP_NUM("max_dgain", "Max digital gain", "set-max-dgain", "mdi:signal-variant", 160),
	ISP_NUM("dpc_strength", "Dead pixel correction", "set-dpc", "mdi:grain", 255),
	ISP_NUM("drc_strength", "Dynamic range compression", "set-drc", "mdi:gradient-vertical",
		255),
	ISP_NUM("defog_strength", "Defog", "set-defog-strength", "mdi:weather-fog", 255),
	ISP_NUM("highlight_depress", "Highlight depression", "set-highlight-depress",
		"mdi:white-balance-sunny", 255),
	ISP_NUM("backlight_comp", "Backlight compensation", "set-backlight-comp",
		"mdi:brightness-4", 10),

	IMG_CFG_NUM("temper", "Temporal noise reduction", "mdi:blur-linear", 255),
	IMG_CFG_SW("hflip", "Flip horizontally", "mdi:flip-horizontal"),
	IMG_CFG_SW("vflip", "Flip vertically", "mdi:flip-vertical"),

	/* ---- Day/night ---- */
	{.key = "ircut_mode",
	 .name = "Day/night mode",
	 .kind = CTRL_SELECT,
	 .icon = "mdi:theme-light-dark",
	 .cat = CAT_CONFIG,
	 .owner = RMQ_D_RIC,
	 .value = "ir.mode",
	 .cmd_tpl = "{\"cmd\":\"ircut-mode\",\"value\":\"{{ value }}\"}",
	 .options = opt_daynight},
	/* ric reports the trigger it settled on, which need not be the one in
	 * the file: a platform that reports no EV demotes `photo` to `luma` at
	 * startup rather than running blind. Showing the setting rather than
	 * the outcome would hide exactly that. */
	{.key = "ircut_trigger",
	 .name = "Day/night trigger",
	 .kind = CTRL_SELECT,
	 .icon = "mdi:target",
	 .cat = CAT_CONFIG,
	 .owner = RMQ_D_RIC,
	 .value = "ir.trigger",
	 .cmd_tpl = "{\"cmd\":\"config-set\",\"section\":\"ircut\",\"key\":\"trigger\","
		    "\"value\":\"{{ value }}\"}",
	 .options = opt_trigger,
	 .restarts = true},

	/* The luma thresholds are live: ric applies and records them itself. */
	IRC_LIVE("night_luma", "Night luma threshold", "mdi:brightness-3", 255),
	IRC_LIVE("night_gain", "Night gain threshold", "mdi:signal", 1000000),
	IRC_LIVE("day_gain_pct", "Day gain (% of night)", "mdi:percent", 100),
	IRC_LIVE("hysteresis_sec", "Hysteresis", "mdi:timer-sand", 300),
	IRC_LIVE("poll_interval_ms", "Poll interval", "mdi:timer", 60000),

	/*
	 * How the board is wired, which ric reads only at startup. These would
	 * normally come from /etc/thingino.json; on an OpenIPC base there is no
	 * such file, so this is where a board gets described.
	 *
	 * The ADC and photo trigger calibration is deliberately not here. Both
	 * are commissioning for a trigger most boards do not use, and eight
	 * entities is a lot of page to charge every camera for that. They stay
	 * writable over MQTT — `config-set` on the [ircut] section reaches them
	 * by name, and rmq_cmd.c still bounds them.
	 */
	IRC_CFG("gpio_ircut", "IR-cut GPIO", "mdi:chip", -1, 127),
	IRC_CFG("gpio_ircut2", "IR-cut GPIO (H-bridge)", "mdi:chip", -1, 127),
	IRC_CFG("gpio_irled", "IR 850nm GPIO", "mdi:led-on", -1, 127),
	IRC_CFG("gpio_irled2", "IR 940nm GPIO", "mdi:led-on", -1, 127),
	IRC_CFG("pulse_ms", "H-bridge pulse width", "mdi:pulse", 1, 1000),

	/* ---- Main stream ---- */
	STREAM_CTRLS("0", GRP_MAIN),
	/* ---- Sub stream ---- */
	STREAM_CTRLS("1", GRP_SUB),
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

	return c;
}

/* Build one control entry. */
static cJSON *make_control(struct rmq_state *st, const ha_control_t *ct, const res_list_t *res)
{
	static const char *const platforms[] = {"number", "select", "switch", "button"};

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
		const char *const *choices = ct->options ? ct->options : res->opt;
		cJSON *opts = cJSON_AddArrayToObject(c, "ops");
		for (int i = 0; opts && choices[i]; i++)
			cJSON_AddItemToArray(opts, cJSON_CreateString(choices[i]));
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

/* The discovery topic for one group. */
static void group_topic(struct rmq_state *st, ha_group_t g, char *out, size_t outsz)
{
	if (!groups[g].suffix)
		rss_strlcpy(out, st->topic_discovery, outsz);
	else
		snprintf(out, outsz, "%s/device/%s_%s/config", st->discovery_prefix, st->client_id,
			 groups[g].suffix);
}

static int publish_group(struct rmq_state *st, ha_group_t g, const rmq_daemons_t *now,
			 const rmq_daemons_t *previous)
{
	cJSON *root = cJSON_CreateObject();
	if (!root)
		return -1;

	/* Device identity. All components collapse under this on one page. */
	cJSON *dev = cJSON_AddObjectToObject(root, "dev");
	if (groups[g].suffix) {
		char ids[192], name[128];
		snprintf(ids, sizeof(ids), "%s_%s", st->client_id, groups[g].suffix);
		snprintf(name, sizeof(name), "%s %s", st->device_name, groups[g].name);
		cJSON_AddStringToObject(dev, "ids", ids);
		cJSON_AddStringToObject(dev, "name", name);
		/*
		 * Linked to the camera rather than free-standing, so Home
		 * Assistant nests it under the camera and an area assigned
		 * once covers the lot.
		 */
		cJSON_AddStringToObject(dev, "via_device", st->client_id);
	} else {
		cJSON_AddStringToObject(dev, "ids", st->client_id);
		cJSON_AddStringToObject(dev, "name", st->device_name);
	}
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

		if (e->group != g)
			continue;

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
	/*
	 * The picture. Its own platform rather than a row in either table:
	 * a camera component carries an image topic and nothing else, with no
	 * state template, no unit and no command.
	 */
	if (g == GRP_CAMERA && st->snapshot_enabled) {
		cJSON *cam = make_common(st, "snapshot_image", "Snapshot", NULL, CAT_PRIMARY, true,
					 "camera");
		if (cam) {
			cJSON_AddStringToObject(cam, "t", st->topic_snapshot);
			cJSON_AddItemToObject(cmps, "snapshot_image", cam);
			published++;
		}
	}

	res_list_t res;
	res_build(&res, st);

	for (int i = 0; i < control_count; i++) {
		const ha_control_t *ct = &controls[i];

		if (ct->group != g)
			continue;

		bool live = st->commands_enabled && owner_available(ct->owner, now);

		/* A derived list with nothing in it means the camera reported no
		 * geometry at all. Offering an empty dropdown would be worse
		 * than offering nothing, so the control waits for a poll that
		 * knows something. */
		if (ct->kind == CTRL_SELECT && !ct->options && res.count == 0)
			live = false;

		/* An ISP block this part does not have. The daemon is up and
		 * the command would be accepted; it is the silicon underneath
		 * that has nothing to change. */
		if (ct->cap && !isp_settable(st, ct->cap))
			live = false;

		/* No picture to take one of. */
		if (strcmp(ct->key, "snapshot") == 0 && !st->snapshot_enabled)
			live = false;

		if (live) {
			cJSON *c = make_control(st, ct, &res);
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

	char topic[RMQ_TOPIC_MAX];
	group_topic(st, g, topic, sizeof(topic));

	int rc = rmq_mqtt_publish(st->mqtt, topic, payload, strlen(payload), 1, true);
	RSS_INFO("ha: %s discovery published, %d entities, %zu bytes",
		 groups[g].name ? groups[g].name : "camera", published, strlen(payload));
	free(payload);

	return rc;
}

int rmq_ha_publish_discovery(struct rmq_state *st, const rmq_daemons_t *now,
			     const rmq_daemons_t *previous)
{
	/*
	 * One document per group, each its own retained topic. The camera goes
	 * first so Home Assistant has the device the others point at before
	 * they arrive — out of order it still resolves, but only after a retry.
	 */
	int rc = 0;
	for (int g = 0; g < GRP_COUNT; g++) {
		if (publish_group(st, (ha_group_t)g, now, previous) < 0)
			rc = -1;
	}

	for (int i = 0; retired_groups[i]; i++) {
		char topic[RMQ_TOPIC_MAX];
		snprintf(topic, sizeof(topic), "%s/device/%s_%s/config", st->discovery_prefix,
			 st->client_id, retired_groups[i]);
		rmq_mqtt_publish(st->mqtt, topic, "", 0, 1, true);
	}

	return rc;
}

int rmq_ha_clear_discovery(struct rmq_state *st)
{
	int rc = 0;
	for (int g = 0; g < GRP_COUNT; g++) {
		char topic[RMQ_TOPIC_MAX];
		group_topic(st, (ha_group_t)g, topic, sizeof(topic));
		if (rmq_mqtt_publish(st->mqtt, topic, "", 0, 1, true) < 0)
			rc = -1;
	}
	return rc;
}
