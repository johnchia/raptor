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
 * Home Assistant has no sections within a device — only the three categories
 * above, which is not enough shape for sixty-odd entities. So the groups that
 * are really separate subjects become separate devices, each linked back to
 * the camera with `via_device`, and each published as its own discovery
 * document. The camera's own page keeps what describes the camera.
 */
typedef enum {
	GRP_CAMERA = 0,
	GRP_IMAGE,
	GRP_DAYNIGHT,
	GRP_MAIN,
	GRP_SUB,
	GRP_COUNT,
} ha_group_t;

static const struct {
	const char *suffix; /* NULL: the camera itself, no via_device */
	const char *name;   /* appended to the camera's name */
} groups[GRP_COUNT] = {
	[GRP_CAMERA] = {NULL, NULL},
	[GRP_IMAGE] = {"image", "Image"},
	[GRP_DAYNIGHT] = {"daynight", "Day/night"},
	[GRP_MAIN] = {"main", "Main stream"},
	[GRP_SUB] = {"sub", "Sub stream"},
};

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
 * Restart-tier controls ship disabled too, for a different reason — see
 * ha_control_t below — so the call sites combine the two rather than this
 * deciding on its own.
 *
 * Home Assistant applies either only when it first creates an entity, so a
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
	ha_group_t group;

	/* When set, the entity is a binary_sensor rather than a sensor, and
	 * this is the Jinja expression that makes it ON. */
	const char *bin_on;
} ha_entity_t;

/*
 * Units and constraints belong in the name: it is the only string visible
 * without opening the entity, and HA has no help-text field of any kind.
 *
 * Names are not prefixed with their group — the group is the device page the
 * entity sits on, and Home Assistant shows that above it already.
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
	 .name = "State",
	 .value = "ir.state",
	 .icon = "mdi:theme-light-dark",
	 .cat = CAT_PRIMARY,
	 .owner = RMQ_D_RIC,
	 .group = GRP_DAYNIGHT},
	{.key = "total_gain",
	 .name = "Total gain (raw)",
	 .value = "ir.total_gain",
	 .icon = "mdi:brightness-6",
	 .cat = CAT_DIAGNOSTIC,
	 .owner = RMQ_D_RIC,
	 .group = GRP_DAYNIGHT},
	{.key = "exposure_us",
	 .name = "Exposure",
	 .value = "ir.exposure_us",
	 .unit = "µs",
	 .icon = "mdi:camera-iris",
	 .cat = CAT_DIAGNOSTIC,
	 .owner = RMQ_D_RIC,
	 .group = GRP_DAYNIGHT},
	{.key = "ae_luma",
	 .name = "AE luma",
	 .value = "ir.ae_luma",
	 .icon = "mdi:brightness-percent",
	 .cat = CAT_DIAGNOSTIC,
	 .owner = RMQ_D_RIC,
	 .group = GRP_DAYNIGHT},

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
	const char *const *options; /* CTRL_SELECT, NULL-terminated */

	/*
	 * Restart-tier controls ship disabled whatever their category. Using
	 * one costs an interruption to video or audio, which is not what a
	 * control sitting on the device page looks like it will do, so it is
	 * enabled deliberately or not at all.
	 */
	bool restarts;
} ha_control_t;

static const char *const opt_daynight[] = {"auto", "day", "night", NULL};
static const char *const opt_trigger[] = {"luma", "gain", "adc", "photo", NULL};
static const char *const opt_vcodec[] = {"h264", "h265", NULL};
static const char *const opt_acodec[] = {"pcmu", "pcma", "l16", "aac", "opus", NULL};
static const char *const opt_arate[] = {"8000", "16000", "32000", "48000", NULL};

/*
 * A list rather than a pair of width/height boxes: the sensor supports a
 * handful of modes, and a free-typed size the ISP cannot produce fails at
 * pipeline init — after the restart, with no picture left to explain it.
 *
 * A camera running a size that is not on this list shows the control blank
 * until it is set, since Home Assistant has no option matching the state. The
 * list is therefore the common set rather than a claim about any one sensor;
 * the schema in Phase 5 is where it stops being guesswork.
 */
static const char *const opt_resolution[] = {"1920x1080", "1280x720", "1024x576",
					     "800x448",	  "640x360",  "640x480",
					     "480x270",	  "320x240",  NULL};

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
	 .cat = CAT_CONFIG,                                                                        \
	 .owner = RMQ_D_RVD,                                                                       \
	 .group = GRP_IMAGE,                                                                       \
	 .value = "image." k,                                                                      \
	 .cmd_tpl = "{\"cmd\":\"" cmd "\",\"value\":{{ value | int }}}",                           \
	 .min = 0,                                                                                 \
	 .max = hi,                                                                                \
	 .step = 1}

/* [ircut] thresholds ric applies and records live. */
#define IRC_LIVE(k, nm, ic, hi)                                                                    \
	{.key = "ircut_" k,                                                                        \
	 .name = nm,                                                                               \
	 .kind = CTRL_NUMBER,                                                                      \
	 .icon = ic,                                                                               \
	 .cat = CAT_CONFIG,                                                                        \
	 .owner = RMQ_D_RIC,                                                                       \
	 .group = GRP_DAYNIGHT,                                                                    \
	 .value = "ir." k,                                                                         \
	 .cmd_tpl = "{\"cmd\":\"ircut-threshold\",\"key\":\"" k "\","                              \
		    "\"value\":{{ value | int }}}",                                                \
	 .min = 0,                                                                                 \
	 .max = hi,                                                                                \
	 .step = 1}

/* [ircut] keys ric reads only at startup: a config write and a restart. */
#define IRC_CFG_(k, nm, ic, lo, hi, val)                                                           \
	{.key = "ircut_" k,                                                                        \
	 .name = nm,                                                                               \
	 .kind = CTRL_NUMBER,                                                                      \
	 .icon = ic,                                                                               \
	 .cat = CAT_CONFIG,                                                                        \
	 .owner = RMQ_D_RIC,                                                                       \
	 .group = GRP_DAYNIGHT,                                                                    \
	 .value = val,                                                                             \
	 .cmd_tpl = "{\"cmd\":\"config-set\",\"section\":\"ircut\",\"key\":\"" k "\","             \
		    "\"value\":{{ value | int }}}",                                                \
	 .min = lo,                                                                                \
	 .max = hi,                                                                                \
	 .step = 1,                                                                                \
	 .restarts = true}

/* ric reports these back, so the control can show one. */
#define IRC_CFGV(k, nm, ic, lo, hi) IRC_CFG_(k, nm, ic, lo, hi, "ir." k)
/* ric reports these nowhere: write-only, and honest about it. */
#define IRC_CFG(k, nm, ic, lo, hi) IRC_CFG_(k, nm, ic, lo, hi, NULL)

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
			    "\"height\":{{ value.split('x')[1] | int }}}}",                        \
		 .options = opt_resolution,                                                        \
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
	ISP_NUM("temper", "Temporal noise reduction", "set-temper", "mdi:blur-linear", 255),
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

	{.key = "image_hflip",
	 .name = "Flip horizontally",
	 .kind = CTRL_SWITCH,
	 .icon = "mdi:flip-horizontal",
	 .cat = CAT_CONFIG,
	 .owner = RMQ_D_RVD,
	 .group = GRP_IMAGE,
	 .value = "image.hflip",
	 .payload = "{\"cmd\":\"set-hflip\",\"value\":1}",
	 .payload_off = "{\"cmd\":\"set-hflip\",\"value\":0}"},
	{.key = "image_vflip",
	 .name = "Flip vertically",
	 .kind = CTRL_SWITCH,
	 .icon = "mdi:flip-vertical",
	 .cat = CAT_CONFIG,
	 .owner = RMQ_D_RVD,
	 .group = GRP_IMAGE,
	 .value = "image.vflip",
	 .payload = "{\"cmd\":\"set-vflip\",\"value\":1}",
	 .payload_off = "{\"cmd\":\"set-vflip\",\"value\":0}"},

	/* ---- Day/night ---- */
	{.key = "ircut_mode",
	 .name = "Mode",
	 .kind = CTRL_SELECT,
	 .icon = "mdi:theme-light-dark",
	 .cat = CAT_PRIMARY,
	 .owner = RMQ_D_RIC,
	 .group = GRP_DAYNIGHT,
	 .value = "ir.mode",
	 .cmd_tpl = "{\"cmd\":\"ircut-mode\",\"value\":\"{{ value }}\"}",
	 .options = opt_daynight},
	{.key = "ircut_trigger",
	 .name = "Trigger",
	 .kind = CTRL_SELECT,
	 .icon = "mdi:target",
	 .cat = CAT_CONFIG,
	 .owner = RMQ_D_RIC,
	 .group = GRP_DAYNIGHT,
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
	 * Wiring and calibration, which ric reads only at startup. The GPIO
	 * pins would normally come from /etc/thingino.json; on an OpenIPC base
	 * there is no such file, so this is where a board gets described.
	 */
	IRC_CFG("gpio_ircut", "IR-cut GPIO", "mdi:chip", -1, 127),
	IRC_CFG("gpio_ircut2", "IR-cut GPIO (H-bridge)", "mdi:chip", -1, 127),
	IRC_CFG("gpio_irled", "IR 850nm GPIO", "mdi:led-on", -1, 127),
	IRC_CFG("gpio_irled2", "IR 940nm GPIO", "mdi:led-on", -1, 127),
	IRC_CFG("pulse_ms", "H-bridge pulse width", "mdi:pulse", 1, 1000),
	IRC_CFG("adc_channel", "ADC channel", "mdi:tune-vertical", 0, 7),
	IRC_CFG("adc_night", "ADC night threshold", "mdi:weather-night", 0, 4095),
	IRC_CFG("adc_day", "ADC day threshold", "mdi:weather-sunny", 0, 4095),
	IRC_CFGV("photo_ev_night", "Photo EV night", "mdi:weather-night", 0, 10000000),
	IRC_CFGV("photo_ev_deep", "Photo EV deep night", "mdi:weather-night-partly-cloudy", 0,
		 10000000),
	IRC_CFGV("photo_ev_day", "Photo EV day", "mdi:weather-sunny", 0, 10000000),
	IRC_CFGV("photo_rgain_rec", "Photo R-gain baseline", "mdi:alpha-r-circle", 0, 8192),
	IRC_CFGV("photo_bgain_rec", "Photo B-gain baseline", "mdi:alpha-b-circle", 0, 8192),

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
static cJSON *make_control(struct rmq_state *st, const ha_control_t *ct)
{
	static const char *const platforms[] = {"number", "select", "switch", "button"};

	cJSON *c = make_common(st, ct->key, ct->name, ct->icon, ct->cat,
			       enabled_by_default(ct->cat) && !ct->restarts, platforms[ct->kind]);
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
	for (int i = 0; i < control_count; i++) {
		const ha_control_t *ct = &controls[i];

		if (ct->group != g)
			continue;

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
