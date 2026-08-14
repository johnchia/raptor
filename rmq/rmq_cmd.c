/*
 * rmq_cmd.c -- Command policy and dispatch
 */

#include "rmq_cmd.h"
#include "rmq.h"
#include "rmq_restart.h"

#include <rss_common.h>
#include <rss_ipc.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cJSON.h>

#define CTRL_TIMEOUT_MS 2000
#define RESP_MAX	4096

/* A command is a few dozen bytes; anything approaching this is not one. */
#define PAYLOAD_MAX 4096

/* Echoed verbatim into the result, so it is bounded like any other input. */
#define NONCE_MAX 64

/* ------------------------------------------------------------------ */
/* Argument descriptors                                                */
/* ------------------------------------------------------------------ */

typedef enum {
	A_END = 0,
	A_INT,	   /* whole JSON number within [min,max] */
	A_ENUM,	   /* JSON string, one of `choices` */
	A_SECTION, /* JSON string, one of the readable sections below */
} arg_type_t;

typedef struct {
	const char *key;
	arg_type_t type;
	bool required;
	int min, max;		    /* A_INT */
	const char *const *choices; /* A_ENUM, NULL-terminated */
} cmd_arg_t;

typedef struct {
	const char *name;     /* what the command topic accepts */
	const char *daemon;   /* NULL: resolved from the section argument */
	const char *ctrl_cmd; /* NULL: same as name */
	const cmd_arg_t *args;
	bool persists; /* the daemon records this in its config */
} cmd_def_t;

/*
 * Sections readable over MQTT, and which daemon holds each.
 *
 * This list is the policy. [rtsp], [http] and [webrtc] carry a password, and
 * [mqtt] carries the broker credential this bridge itself connects with; none
 * of them are here, so no config read can return one.
 *
 * Every daemon parses the whole file, so routing by owner is not about access
 * — it is about freshness. A live change that has not been saved yet exists
 * only in the daemon that made it.
 */
static const struct {
	const char *section;
	const char *daemon;
} section_owner[] = {
	{"sensor", "rvd"},    {"image", "rvd"}, {"stream0", "rvd"}, {"stream1", "rvd"},
	{"jpeg", "rvd"},      {"ring", "rvd"},	{"log", "rvd"},	    {"audio", "rad"},
	{"osd", "rod"},	      {"ircut", "ric"}, {"motion", "rmd"},  {"recording", "rmr"},
	{"timelapse", "rmr"}, {NULL, NULL},
};

static int section_index(const char *section)
{
	for (int i = 0; section_owner[i].section; i++) {
		if (strcmp(section, section_owner[i].section) == 0)
			return i;
	}
	return -1;
}

static const char *section_daemon(const char *section)
{
	int i = section ? section_index(section) : -1;
	return i < 0 ? NULL : section_owner[i].daemon;
}

static const char *const choices_on_off[] = {"on", "off", NULL};
static const char *const choices_daynight[] = {"auto", "day", "night", NULL};
/* Mirrors rvd's rc_map (rvd_ctrl.c). rvd silently falls back to CBR for a name
 * it does not know, so a typo would otherwise change the rate control without
 * saying so — refusing here is what makes that visible. */
static const char *const choices_rc_mode[] = {"cbr",   "vbr",	"capped_vbr", "capped_quality",
					      "fixqp", "smart", NULL};

/*
 * ric owns the range of each threshold and rejects a bad one with a message
 * naming it, so only the key is constrained here. Repeating the ranges would
 * duplicate a table that is free to change on the other side of an IPC call.
 */
static const char *const choices_threshold[] = {
	"night_luma",	 "night_gain",	   "day_gain_pct",     "night_threshold",
	"day_threshold", "hysteresis_sec", "poll_interval_ms", NULL};

/* ------------------------------------------------------------------ */
/* Argument layouts                                                    */
/*                                                                     */
/* Channel bounds only stop the field being wild. rvd rejects a channel */
/* past its own stream count, which is the number that matters and is   */
/* not knowable from here.                                              */
/* ------------------------------------------------------------------ */

static const cmd_arg_t args_none[] = {
	{.type = A_END},
};

static const cmd_arg_t args_channel_opt[] = {
	{.key = "channel", .type = A_INT, .required = false, .min = 0, .max = 3},
	{.type = A_END},
};

static const cmd_arg_t args_bitrate[] = {
	{.key = "channel", .type = A_INT, .required = true, .min = 0, .max = 3},
	{.key = "value", .type = A_INT, .required = true, .min = 32000, .max = 50000000},
	{.type = A_END},
};

static const cmd_arg_t args_gop[] = {
	{.key = "channel", .type = A_INT, .required = true, .min = 0, .max = 3},
	{.key = "value", .type = A_INT, .required = true, .min = 1, .max = 300},
	{.type = A_END},
};

static const cmd_arg_t args_fps[] = {
	{.key = "channel", .type = A_INT, .required = true, .min = 0, .max = 3},
	{.key = "value", .type = A_INT, .required = true, .min = 1, .max = 120},
	{.type = A_END},
};

/* H.264 and H.265 both use QP 0-51. */
static const cmd_arg_t args_qp_bounds[] = {
	{.key = "channel", .type = A_INT, .required = true, .min = 0, .max = 3},
	{.key = "min", .type = A_INT, .required = true, .min = 0, .max = 51},
	{.key = "max", .type = A_INT, .required = true, .min = 0, .max = 51},
	{.type = A_END},
};

static const cmd_arg_t args_rc_mode[] = {
	{.key = "channel", .type = A_INT, .required = true, .min = 0, .max = 3},
	{.key = "mode", .type = A_ENUM, .required = true, .choices = choices_rc_mode},
	{.key = "bitrate", .type = A_INT, .required = false, .min = 32000, .max = 50000000},
	{.type = A_END},
};

static const cmd_arg_t args_daynight[] = {
	{.key = "value", .type = A_ENUM, .required = true, .choices = choices_daynight},
	{.type = A_END},
};

static const cmd_arg_t args_on_off[] = {
	{.key = "value", .type = A_ENUM, .required = true, .choices = choices_on_off},
	{.type = A_END},
};

static const cmd_arg_t args_threshold[] = {
	{.key = "key", .type = A_ENUM, .required = true, .choices = choices_threshold},
	{.key = "value", .type = A_INT, .required = true, .min = 0, .max = 1000000},
	{.type = A_END},
};

/* Both SoC families take volume as 0-100 and gain as the ADC's 0-31 steps. */
static const cmd_arg_t args_volume[] = {
	{.key = "value", .type = A_INT, .required = true, .min = 0, .max = 100},
	{.type = A_END},
};

static const cmd_arg_t args_gain[] = {
	{.key = "value", .type = A_INT, .required = true, .min = 0, .max = 31},
	{.type = A_END},
};

static const cmd_arg_t args_bool[] = {
	{.key = "value", .type = A_INT, .required = true, .min = 0, .max = 1},
	{.type = A_END},
};

static const cmd_arg_t args_section[] = {
	{.key = "section", .type = A_SECTION, .required = true},
	{.type = A_END},
};

/*
 * ISP tuning. The platforms disagree about which of these do anything — the
 * HAL answers "unsupported" where a chip has no such block, which reaches the
 * sender as a plain error — but they agree on the scale, so one range serves.
 */
static const cmd_arg_t args_isp_255[] = {
	{.key = "value", .type = A_INT, .required = true, .min = 0, .max = 255},
	{.type = A_END},
};

/* Gain ceilings are 0-160 on every platform that implements them. */
static const cmd_arg_t args_isp_gain[] = {
	{.key = "value", .type = A_INT, .required = true, .min = 0, .max = 160},
	{.type = A_END},
};

static const cmd_arg_t args_isp_backlight[] = {
	{.key = "value", .type = A_INT, .required = true, .min = 0, .max = 10},
	{.type = A_END},
};

/* ------------------------------------------------------------------ */
/* The allowlist                                                       */
/*                                                                     */
/* Live tier: every command here takes effect on the running camera     */
/* without interrupting it. Settings that only apply on a restart go    */
/* through config-set and the key table below, and nothing that merely  */
/* stops a daemon appears in either.                                    */
/* ------------------------------------------------------------------ */

static const cmd_def_t commands[] = {
	/* -- Video. rvd applies each to the running encoder and records it in
	 *    its config, so each owes a save. -- */
	{"set-bitrate", "rvd", NULL, args_bitrate, true},
	{"set-gop", "rvd", NULL, args_gop, true},
	{"set-fps", "rvd", NULL, args_fps, true},
	{"set-qp-bounds", "rvd", NULL, args_qp_bounds, true},
	{"set-rc-mode", "rvd", NULL, args_rc_mode, true},
	{"request-idr", "rvd", NULL, args_channel_opt, false},

	/* -- Image. rvd applies each to the ISP and records it in [image], so
	 *    every one owes a save. All seventeen keys of the section are
	 *    here: the tier is live because tuning is done by looking at the
	 *    picture, which a restart between adjustments makes impossible. -- */
	{"set-brightness", "rvd", NULL, args_isp_255, true},
	{"set-contrast", "rvd", NULL, args_isp_255, true},
	{"set-saturation", "rvd", NULL, args_isp_255, true},
	{"set-sharpness", "rvd", NULL, args_isp_255, true},
	{"set-hue", "rvd", NULL, args_isp_255, true},
	{"set-sinter", "rvd", NULL, args_isp_255, true},
	{"set-temper", "rvd", NULL, args_isp_255, true},
	{"set-ae-comp", "rvd", NULL, args_isp_255, true},
	{"set-max-again", "rvd", NULL, args_isp_gain, true},
	{"set-max-dgain", "rvd", NULL, args_isp_gain, true},
	{"set-dpc", "rvd", NULL, args_isp_255, true},
	{"set-drc", "rvd", NULL, args_isp_255, true},
	{"set-defog-strength", "rvd", NULL, args_isp_255, true},
	{"set-highlight-depress", "rvd", NULL, args_isp_255, true},
	{"set-backlight-comp", "rvd", NULL, args_isp_backlight, true},
	{"set-hflip", "rvd", NULL, args_bool, true},
	{"set-vflip", "rvd", NULL, args_bool, true},

	/* -- Day/night. The LED banks are a manual override of automatic
	 *    behaviour rather than a setting, which is why they persist
	 *    nothing: the next auto transition takes them back. -- */
	{"ircut-mode", "ric", "mode", args_daynight, true},
	{"ircut-threshold", "ric", "set-threshold", args_threshold, true},
	{"ir850", "ric", NULL, args_on_off, false},
	{"ir940", "ric", NULL, args_on_off, false},

	/* -- Audio -- */
	{"set-volume", "rad", NULL, args_volume, true},
	{"set-gain", "rad", NULL, args_gain, true},
	{"ao-set-volume", "rad", NULL, args_volume, false},
	{"ao-set-gain", "rad", NULL, args_gain, false},
	{"set-aec", "rad", NULL, args_bool, true},
	{"set-hpf", "rad", NULL, args_bool, false},

	/* -- OSD. rod pauses rendering rather than writing [osd] enabled, so
	 *    this deliberately persists nothing: a reboot restores the
	 *    configured state, which is what rod already does. -- */
	{"osd-enable", "rod", "enable", args_none, false},
	{"osd-disable", "rod", "disable", args_none, false},

	{"config-get-section", NULL, NULL, args_section, false},

	/* `config-set` is deliberately absent: it is not a daemon request, so
	 * it has its own table below and its own planner. */

	{NULL, NULL, NULL, NULL, false},
};

static const cmd_def_t *find_command(const char *name)
{
	for (int i = 0; commands[i].name; i++) {
		if (strcmp(commands[i].name, name) == 0)
			return &commands[i];
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* The writable config keys                                            */
/*                                                                     */
/* Restart tier: these change the file rather than the running daemon,  */
/* so the owner is restarted to pick them up.                           */
/*                                                                     */
/* Almost every value here is an integer, a boolean or a closed enum,   */
/* and for those no byte of the payload reaches the file: what is       */
/* written is the table's own spelling or a number this file formatted. */
/* That is what keeps every path, format string and endpoint alias in   */
/* raptor.conf unreachable from the network without a rule naming any   */
/* of them, and it is still the default — a key is unwritable until it  */
/* is listed, and listing it as one of these three types cannot expose  */
/* free-form text.                                                      */
/*                                                                     */
/* V_CRED is the one exception, and it exists for exactly the two RTSP  */
/* credential keys: a password nobody may choose is not a password. It  */
/* is not a general string type. The grammar is RFC 3986's unreserved   */
/* set — letters, digits, '-', '_', '.', '~' — which is what an RTSP    */
/* URL, a Digest header, an INI value and a shell word all accept       */
/* unescaped, so a credential cannot become a second config directive,  */
/* a path, or a URL that parses as something else. Length is capped at  */
/* the table entry's max. Do not reach for this type for anything but a */
/* credential: for a path or a template the grammar is no protection at */
/* all, and the reason those keys are absent is that they are absent.   */
/* ------------------------------------------------------------------ */

typedef enum {
	V_INT = 0,
	V_BOOL,
	V_ENUM,
	V_CRED,
} val_type_t;

typedef struct {
	const char *section;
	const char *key;
	val_type_t type;
	int min, max;		    /* V_INT range; V_CRED length, min 0 */
	const char *const *choices; /* V_ENUM, NULL-terminated */
} cfg_key_t;

/*
 * Which daemon re-reads a section. Mirrors raptorctl's own map
 * (raptorctl_config.c), extended with the sections raptor has gained since.
 *
 * Unlike the readable list above, [rtsp] and [http] appear here: a section
 * that cannot be read in bulk can still have individual keys written, and the
 * password is simply not one of the keys the table names.
 */
static const struct {
	const char *section;
	rmq_daemon_t owner;
} write_owner[] = {
	{"sensor", RMQ_D_RVD},	  {"stream0", RMQ_D_RVD}, {"stream1", RMQ_D_RVD},
	{"image", RMQ_D_RVD},	  {"jpeg", RMQ_D_RVD},	  {"audio", RMQ_D_RAD},
	{"rtsp", RMQ_D_RSD},	  {"http", RMQ_D_RHD},	  {"osd", RMQ_D_ROD},
	{"ircut", RMQ_D_RIC},	  {"motion", RMQ_D_RMD},  {"recording", RMQ_D_RMR},
	{"timelapse", RMQ_D_RMR}, {NULL, RMQ_D_COUNT},
};

static rmq_daemon_t write_section_owner(const char *section)
{
	for (int i = 0; write_owner[i].section; i++) {
		if (strcmp(section, write_owner[i].section) == 0)
			return write_owner[i].owner;
	}
	return RMQ_D_COUNT;
}

static const char *const choices_vcodec[] = {"h264", "h265", NULL};
static const char *const choices_acodec[] = {"pcmu", "pcma", "l16", "aac", "opus", NULL};
static const char *const choices_ainput[] = {"amic", "dmic", NULL};
static const char *const choices_arate[] = {"8000", "16000", "32000", "48000", NULL};
static const char *const choices_trigger[] = {"luma", "gain", "adc", "photo", NULL};
static const char *const choices_algorithm[] = {"move", "base_move", "persondet", "yolo", NULL};
static const char *const choices_recmode[] = {"continuous", "motion", "both", NULL};

static const cfg_key_t cfg_keys[] = {
	/* -- Sensor -- */
	{"sensor", "fps", V_INT, 1, 120, NULL},
	{"sensor", "antiflicker", V_INT, 0, 2, NULL},

	/* -- Video. Resolution is the reason this tier exists at all: an
	 *    encoder is created at its size and cannot be resized. -- */
	{"stream0", "width", V_INT, 160, 4096, NULL},
	{"stream0", "height", V_INT, 120, 4096, NULL},
	{"stream0", "codec", V_ENUM, 0, 0, choices_vcodec},
	{"stream0", "profile", V_INT, 0, 2, NULL},
	{"stream0", "osd_enabled", V_BOOL, 0, 0, NULL},
	{"stream0", "jpeg", V_BOOL, 0, 0, NULL},
	{"stream1", "enabled", V_BOOL, 0, 0, NULL},
	{"stream1", "width", V_INT, 160, 4096, NULL},
	{"stream1", "height", V_INT, 120, 4096, NULL},
	{"stream1", "codec", V_ENUM, 0, 0, choices_vcodec},
	{"stream1", "profile", V_INT, 0, 2, NULL},
	{"stream1", "osd_enabled", V_BOOL, 0, 0, NULL},
	{"stream1", "jpeg", V_BOOL, 0, 0, NULL},

	/* -- Image. Only the keys a part may refuse while its ISP channel is
	 *    running: SigmaStar carries orientation and the 3DNR level in one
	 *    creation-time call, and applies them when the channel is built.
	 *    The rest of [image] is a live command and stays out of here, so
	 *    tuning by eye costs no interruption. -- */
	/*
	 * Orientation is 0/1 and not a boolean, because rvd reads it with
	 * rss_config_get_int: `true` parses as no number at all and falls back
	 * to the default, so a flip written that way is silently not applied.
	 * The type here is the one the owning daemon reads with.
	 */
	{"image", "hflip", V_INT, 0, 1, NULL},
	{"image", "vflip", V_INT, 0, 1, NULL},
	{"image", "temper", V_INT, 0, 255, NULL},

	/* -- Snapshots -- */
	{"jpeg", "enabled", V_BOOL, 0, 0, NULL},
	{"jpeg", "quality", V_INT, 1, 100, NULL},
	{"jpeg", "fps", V_INT, 1, 30, NULL},
	{"jpeg", "idle", V_BOOL, 0, 0, NULL},

	/* -- Audio -- */
	{"audio", "enabled", V_BOOL, 0, 0, NULL},
	{"audio", "input", V_ENUM, 0, 0, choices_ainput},
	{"audio", "sample_rate", V_ENUM, 0, 0, choices_arate},
	{"audio", "codec", V_ENUM, 0, 0, choices_acodec},
	{"audio", "bitrate", V_INT, 8000, 320000, NULL},

	/* -- RTSP and HTTP. Both sections hold a username and password, and
	 *    neither appears among the keys, so the credential is out of
	 *    reach in the same way an unnamed command is. -- */
	{"rtsp", "enabled", V_BOOL, 0, 0, NULL},
	{"rtsp", "port", V_INT, 1, 65535, NULL},
	{"rtsp", "max_clients", V_INT, 1, 32, NULL},
	{"rtsp", "session_timeout", V_INT, 10, 3600, NULL},
	{"rtsp", "idr_on_join", V_BOOL, 0, 0, NULL},
	/* rsd enables Digest auth only when both are set, so clearing either
	 * one turns authentication off — which is the only way to turn it off,
	 * and is why an empty value is accepted here. */
	{"rtsp", "username", V_CRED, 0, 63, NULL},
	{"rtsp", "password", V_CRED, 0, 63, NULL},
	{"http", "enabled", V_BOOL, 0, 0, NULL},
	{"http", "port", V_INT, 1, 65535, NULL},
	{"http", "max_clients", V_INT, 1, 32, NULL},

	/* -- OSD -- */
	{"osd", "enabled", V_BOOL, 0, 0, NULL},
	{"osd", "font_size", V_INT, 8, 96, NULL},
	{"osd", "font_stroke", V_INT, 0, 5, NULL},

	/* -- Day/night. The luma and gain thresholds are live commands rather
	 *    than writes; what is left here is everything ric reads only at
	 *    startup — how the board is wired, and how the two triggers that
	 *    are not luma are calibrated. -- */
	{"ircut", "enabled", V_BOOL, 0, 0, NULL},
	{"ircut", "trigger", V_ENUM, 0, 0, choices_trigger},
	{"ircut", "pulse_ms", V_INT, 1, 1000, NULL},
	/*
	 * GPIO pin assignments. These describe the board rather than any
	 * behaviour, and are normally read from /etc/thingino.json — but that
	 * file is absent on an OpenIPC base, which leaves the config the only
	 * place to put them, and MQTT the only way to reach the config.
	 *
	 * A wrong pin here drives a wrong pin: the ceiling is the SoC's GPIO
	 * count rather than anything ric knows to be safe. Bounded, not made
	 * harmless.
	 */
	{"ircut", "gpio_ircut", V_INT, -1, 127, NULL},
	{"ircut", "gpio_ircut2", V_INT, -1, 127, NULL},
	{"ircut", "gpio_irled", V_INT, -1, 127, NULL},
	{"ircut", "gpio_irled2", V_INT, -1, 127, NULL},
	/* trigger = adc: a photoresistor on an ADC channel, 12-bit. */
	{"ircut", "adc_channel", V_INT, 0, 7, NULL},
	{"ircut", "adc_night", V_INT, 0, 4095, NULL},
	{"ircut", "adc_day", V_INT, 0, 4095, NULL},
	/* trigger = photo: EV thresholds, where higher means darker. */
	{"ircut", "photo_ev_night", V_INT, 0, 10000000, NULL},
	{"ircut", "photo_ev_deep", V_INT, 0, 10000000, NULL},
	{"ircut", "photo_ev_day", V_INT, 0, 10000000, NULL},
	/* AWB baselines for the same trigger; 0 self-calibrates from the
	 * sensor, so the range starts there rather than at 1x (1024). */
	{"ircut", "photo_rgain_rec", V_INT, 0, 8192, NULL},
	{"ircut", "photo_bgain_rec", V_INT, 0, 8192, NULL},

	/* -- Motion -- */
	{"motion", "enabled", V_BOOL, 0, 0, NULL},
	{"motion", "algorithm", V_ENUM, 0, 0, choices_algorithm},
	{"motion", "sensitivity", V_INT, 0, 5, NULL},
	{"motion", "cooldown_sec", V_INT, 1, 3600, NULL},
	{"motion", "record", V_BOOL, 0, 0, NULL},
	{"motion", "record_post_sec", V_INT, 0, 600, NULL},

	/* -- Recording. storage_path is absent with the rest of the strings,
	 *    which also keeps the write path away from the filesystem. -- */
	{"recording", "enabled", V_BOOL, 0, 0, NULL},
	{"recording", "mode", V_ENUM, 0, 0, choices_recmode},
	{"recording", "stream", V_INT, 0, 1, NULL},
	{"recording", "audio", V_BOOL, 0, 0, NULL},
	{"recording", "segment_minutes", V_INT, 1, 1440, NULL},
	{"recording", "max_storage_mb", V_INT, 0, 1000000, NULL},
	{"recording", "prebuffer_sec", V_INT, 0, 5, NULL},

	/* -- Timelapse -- */
	{"timelapse", "enabled", V_BOOL, 0, 0, NULL},
	{"timelapse", "interval", V_INT, 2, 86400, NULL},
	{"timelapse", "playback_fps", V_INT, 1, 120, NULL},
	{"timelapse", "max_mb", V_INT, 0, 1000000, NULL},

	{NULL, NULL, V_INT, 0, 0, NULL},
};

static const cfg_key_t *cfg_key_find(const char *section, const char *key)
{
	for (int i = 0; cfg_keys[i].section; i++) {
		if (strcmp(cfg_keys[i].section, section) == 0 && strcmp(cfg_keys[i].key, key) == 0)
			return &cfg_keys[i];
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Validation                                                          */
/* ------------------------------------------------------------------ */

static void err_choices(char *err, size_t errsz, const char *key, const char *const *choices)
{
	/* Naming the alternatives is safe — a closed enum is the whole point
	 * of the field — and turns a rejection into something fixable. */
	int n = snprintf(err, errsz, "'%s' must be one of:", key);
	for (int i = 0; choices[i] && n > 0 && (size_t)n < errsz; i++)
		n += snprintf(err + n, errsz - (size_t)n, " %s", choices[i]);
}

/*
 * A whole JSON number within [min,max].
 *
 * The order matters and is why both tiers call this rather than each keeping
 * their own copy: the range is checked while the value is still a double,
 * because a number far outside int wraps into range once narrowed and would
 * then pass. Narrowing is the caller's, after this returns.
 */
static int check_int(const cJSON *v, const char *key, int min, int max, double *out, char *err,
		     size_t errsz)
{
	if (!cJSON_IsNumber(v)) {
		snprintf(err, errsz, "'%s' must be a number", key);
		return -1;
	}

	double d = cJSON_GetNumberValue(v);
	if (!(d >= (double)min && d <= (double)max)) {
		snprintf(err, errsz, "'%s' out of range (%d-%d)", key, min, max);
		return -1;
	}
	if (d != (double)(long long)d) {
		snprintf(err, errsz, "'%s' must be a whole number", key);
		return -1;
	}

	*out = d;
	return 0;
}

static int add_arg(cJSON *req, const cJSON *in, const cmd_arg_t *a, char *err, size_t errsz)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(in, a->key);

	if (!v || cJSON_IsNull(v)) {
		if (a->required) {
			snprintf(err, errsz, "missing required field '%s'", a->key);
			return -1;
		}
		return 0;
	}

	if (a->type == A_INT) {
		double d;
		if (check_int(v, a->key, a->min, a->max, &d, err, errsz) < 0)
			return -1;
		cJSON_AddNumberToObject(req, a->key, d);
		return 0;
	}

	if (!cJSON_IsString(v) || !v->valuestring) {
		snprintf(err, errsz, "'%s' must be a string", a->key);
		return -1;
	}

	if (a->type == A_SECTION) {
		int idx = section_index(v->valuestring);
		if (idx < 0) {
			/* Deliberately not "no such section": a section that
			 * exists but holds credentials and one that does not
			 * exist read the same from out here. */
			snprintf(err, errsz, "section is not readable");
			return -1;
		}
		cJSON_AddStringToObject(req, a->key, section_owner[idx].section);
		return 0;
	}

	for (int i = 0; a->choices[i]; i++) {
		if (strcmp(v->valuestring, a->choices[i]) == 0) {
			/* The table's copy, not the caller's. The two compare
			 * equal, so this changes nothing about the request —
			 * it just leaves no path by which input bytes reach a
			 * daemon. */
			cJSON_AddStringToObject(req, a->key, a->choices[i]);
			return 0;
		}
	}

	err_choices(err, errsz, a->key, a->choices);
	return -1;
}

/* ------------------------------------------------------------------ */
/* config-set                                                          */
/* ------------------------------------------------------------------ */

/*
 * Render one JSON value to the string the config file will hold. Everything
 * written comes from the table — the enum's own spelling, or a number this
 * function formatted — so no byte of the payload reaches the file verbatim.
 */
static int render_value(const cfg_key_t *k, const cJSON *v, char *out, size_t outsz, char *err,
			size_t errsz)
{
	if (!v || cJSON_IsNull(v)) {
		snprintf(err, errsz, "'%s' needs a value", k->key);
		return -1;
	}

	if (k->type == V_BOOL) {
		/* A JSON boolean, or the 0/1 a Home Assistant template is
		 * likelier to render. */
		bool on;
		if (cJSON_IsBool(v)) {
			on = cJSON_IsTrue(v);
		} else if (cJSON_IsNumber(v) &&
			   (cJSON_GetNumberValue(v) == 0 || cJSON_GetNumberValue(v) == 1)) {
			on = cJSON_GetNumberValue(v) != 0;
		} else {
			snprintf(err, errsz, "'%s' must be true or false", k->key);
			return -1;
		}
		snprintf(out, outsz, "%s", on ? "true" : "false");
		return 0;
	}

	if (k->type == V_INT) {
		double d;
		if (check_int(v, k->key, k->min, k->max, &d, err, errsz) < 0)
			return -1;
		snprintf(out, outsz, "%d", (int)d);
		return 0;
	}

	if (k->type == V_CRED) {
		if (!cJSON_IsString(v) || !v->valuestring) {
			snprintf(err, errsz, "'%s' must be a string", k->key);
			return -1;
		}
		const char *s = v->valuestring;
		size_t n = strlen(s);
		if (n > (size_t)k->max || n >= outsz) {
			snprintf(err, errsz, "'%s' is longer than %d characters", k->key, k->max);
			return -1;
		}
		for (size_t i = 0; i < n; i++) {
			unsigned char c = (unsigned char)s[i];
			if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
				continue;
			/* Naming the permitted set rather than the offending
			 * byte: the value is a credential and must not be
			 * quoted back over the wire, not even one character
			 * of it. */
			snprintf(err, errsz,
				 "'%s' may contain only letters, digits, '-', '_', '.' and '~'",
				 k->key);
			return -1;
		}
		memcpy(out, s, n);
		out[n] = '\0';
		return 0;
	}

	/* V_ENUM. A number is accepted as well as a string so that the numeric
	 * enums — sample rate is the one — still match when a template renders
	 * 16000 rather than "16000". */
	char given[RMQ_CFG_VAL_MAX];
	if (cJSON_IsString(v) && v->valuestring) {
		rss_strlcpy(given, v->valuestring, sizeof(given));
	} else if (cJSON_IsNumber(v) &&
		   cJSON_GetNumberValue(v) == (double)(long long)cJSON_GetNumberValue(v)) {
		snprintf(given, sizeof(given), "%lld", (long long)cJSON_GetNumberValue(v));
	} else {
		snprintf(err, errsz, "'%s' must be a string", k->key);
		return -1;
	}

	for (int i = 0; k->choices[i]; i++) {
		if (strcmp(given, k->choices[i]) == 0) {
			rss_strlcpy(out, k->choices[i], outsz);
			return 0;
		}
	}
	err_choices(err, errsz, k->key, k->choices);
	return -1;
}

static int add_write(rmq_cmd_plan_t *out, const char *section, const char *key, const cJSON *v,
		     char *err, size_t errsz)
{
	const cfg_key_t *k = cfg_key_find(section, key);
	if (!k) {
		/* As with sections: a key that exists but holds a credential or
		 * a path, and one that was never a key at all, read the same
		 * from out here. */
		snprintf(err, errsz, "'%.32s' is not a writable key of [%.24s]", key, section);
		return -1;
	}
	if (out->write_count >= RMQ_CFG_SET_MAX) {
		snprintf(err, errsz, "too many keys in one command (max %d)", RMQ_CFG_SET_MAX);
		return -1;
	}

	rmq_cfg_write_t *w = &out->writes[out->write_count];
	if (render_value(k, v, w->value, sizeof(w->value), err, errsz) < 0)
		return -1;

	/* Names copied from the table, not from the payload, so a line that
	 * reaches raptor.conf is spelled the way this build spells it. */
	rss_strlcpy(w->section, k->section, sizeof(w->section));
	rss_strlcpy(w->key, k->key, sizeof(w->key));
	out->write_count++;
	return 0;
}

/*
 * Two shapes, one path: `key`/`value` for a single edit, or a `values` map to
 * commission a whole section in one message. The map is all-or-nothing — one
 * bad key refuses the lot — because a half-applied section is a configuration
 * nobody chose.
 */
/*
 * The /etc settings, whose whole table is two keys.
 *
 * They need a string to travel, which the config tier deliberately has no type
 * for — so each carries its own grammar instead of a shared one. The timezone
 * is a closed enum, checked against the name list, so nothing free-form
 * reaches /etc/TZ at all. The NTP server is the one genuine string in the
 * bridge, and it is narrowed to what a hostname may contain: no slash, space,
 * quote or shell metacharacter survives, so it cannot become a path or a
 * second directive on the line it is written to.
 */
static int plan_system_set(const cJSON *root, rmq_cmd_plan_t *out, char *err, size_t errsz)
{
	const cJSON *jk = cJSON_GetObjectItemCaseSensitive(root, "key");
	if (!cJSON_IsString(jk) || !jk->valuestring) {
		snprintf(err, errsz, "system-set needs a 'key'");
		return -1;
	}
	char key[RMQ_CFG_KEY_MAX];
	rss_strlcpy(key, jk->valuestring, sizeof(key));

	const cJSON *jv = cJSON_GetObjectItemCaseSensitive(root, "value");
	if (!cJSON_IsString(jv) || !jv->valuestring) {
		snprintf(err, errsz, "'%s' needs a string value", key);
		return -1;
	}
	char value[RMQ_CFG_VAL_MAX];
	if (rss_strlcpy(value, jv->valuestring, sizeof(value)) >= sizeof(value)) {
		snprintf(err, errsz, "'%s' value is too long", key);
		return -1;
	}

	if (strcmp(key, "timezone") == 0) {
		if (!rmq_system_zone_posix(value)) {
			snprintf(err, errsz, "'%s' is not a timezone this build knows", value);
			return -1;
		}
	} else if (strcmp(key, "ntp_server") == 0) {
		if (!rmq_system_valid_host(value)) {
			snprintf(err, errsz, "'%s' is not a hostname or address", value);
			return -1;
		}
	} else {
		snprintf(err, errsz, "'%s' is not a settable system key", key);
		return -1;
	}

	/* Carried in the write slot the config tier already has: same shape,
	 * one entry, with the section naming where it lands rather than a
	 * raptor.conf section that does not exist. */
	rss_strlcpy(out->writes[0].section, "system", sizeof(out->writes[0].section));
	rss_strlcpy(out->writes[0].key, key, sizeof(out->writes[0].key));
	rss_strlcpy(out->writes[0].value, value, sizeof(out->writes[0].value));
	out->write_count = 1;
	return 0;
}

static int plan_config_set(const cJSON *root, rmq_cmd_plan_t *out, char *err, size_t errsz)
{
	const cJSON *sec = cJSON_GetObjectItemCaseSensitive(root, "section");
	if (!cJSON_IsString(sec) || !sec->valuestring) {
		snprintf(err, errsz, "missing 'section'");
		return -1;
	}

	out->restart_owner = write_section_owner(sec->valuestring);
	if (out->restart_owner == RMQ_D_COUNT) {
		snprintf(err, errsz, "section is not writable");
		return -1;
	}

	const cJSON *values = cJSON_GetObjectItemCaseSensitive(root, "values");
	if (values) {
		if (!cJSON_IsObject(values)) {
			snprintf(err, errsz, "'values' must be an object");
			return -1;
		}
		const cJSON *v = NULL;
		cJSON_ArrayForEach(v, values)
		{
			if (!v->string)
				continue;
			if (add_write(out, sec->valuestring, v->string, v, err, errsz) < 0)
				return -1;
		}
		if (out->write_count == 0) {
			snprintf(err, errsz, "'values' names no keys");
			return -1;
		}
		return 0;
	}

	const cJSON *key = cJSON_GetObjectItemCaseSensitive(root, "key");
	if (!cJSON_IsString(key) || !key->valuestring) {
		snprintf(err, errsz, "missing 'key' or 'values'");
		return -1;
	}
	return add_write(out, sec->valuestring, key->valuestring,
			 cJSON_GetObjectItemCaseSensitive(root, "value"), err, errsz);
}

/*
 * The planner proper, over a payload someone else parsed.
 *
 * Split from rmq_cmd_plan() so the dispatch path parses once: it needs the
 * nonce and the command name out of the same document, and re-parsing to get
 * them cost a second pass over every payload.
 */
static int plan_parsed(const cJSON *root, rmq_cmd_plan_t *out, char *err, size_t errsz)
{
	memset(out, 0, sizeof(*out));

	if (!cJSON_IsObject(root)) {
		snprintf(err, errsz, "payload is not a JSON object");
		return -1;
	}

	const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
	if (!cJSON_IsString(cmd) || !cmd->valuestring) {
		snprintf(err, errsz, "missing 'cmd'");
		return -1;
	}

	/* Handled apart from the table because it produces edits rather than a
	 * daemon request, and is checked against its own allowlist. */
	if (strcmp(cmd->valuestring, "config-set") == 0) {
		out->kind = RMQ_PLAN_CONFIG;
		return plan_config_set(root, out, err, errsz);
	}

	/* Also the bridge's own work: rvd is asked for nothing, since opening
	 * the JPEG ring is what starts its encoder. No arguments — which ring
	 * and how often are config, not something a broker client chooses. */
	if (strcmp(cmd->valuestring, "snapshot") == 0) {
		out->kind = RMQ_PLAN_SNAPSHOT;
		return 0;
	}

	/*
	 * Reboot the camera. Not the same thing as the `restart` and
	 * `shutdown` the table refuses: those are relayed to a daemon and stop
	 * it with nothing left to start it again, whereas this brings the
	 * whole system back to the configuration it already had. It changes
	 * nothing about what boots, which is what keeps it out of the class
	 * the confirm timer exists for.
	 *
	 * It is here because the timezone needs it — a setting that applies
	 * only on reboot, offered by a bridge that could not reboot, would
	 * send its user to ssh to finish the job.
	 */
	if (strcmp(cmd->valuestring, "reboot") == 0) {
		out->kind = RMQ_PLAN_REBOOT;
		return 0;
	}

	/* Settings in /etc, which raptor.conf does not hold and the config
	 * table therefore cannot describe. */
	if (strcmp(cmd->valuestring, "system-set") == 0) {
		out->kind = RMQ_PLAN_SYSTEM;
		return plan_system_set(root, out, err, errsz);
	}

	const cmd_def_t *def = find_command(cmd->valuestring);
	if (!def) {
		/* Deny by default. Naming nothing else keeps the refusal from
		 * doubling as a directory of what would have worked. */
		snprintf(err, errsz, "command not permitted");
		return -1;
	}

	cJSON *req = cJSON_CreateObject();
	if (!req) {
		snprintf(err, errsz, "out of memory");
		return -1;
	}
	cJSON_AddStringToObject(req, "cmd", def->ctrl_cmd ? def->ctrl_cmd : def->name);

	/*
	 * Only the fields the entry names are copied across. Anything else in
	 * the payload — a 'file' smuggled alongside a permitted command, a
	 * 'nonce' that belongs to the result rather than the request — is
	 * simply not carried, so it cannot reach a daemon.
	 */
	for (int i = 0; def->args[i].type != A_END; i++) {
		if (add_arg(req, root, &def->args[i], err, errsz) < 0) {
			cJSON_Delete(req);
			return -1;
		}
	}

	out->daemon = def->daemon;
	if (!out->daemon) {
		const cJSON *sec = cJSON_GetObjectItemCaseSensitive(req, "section");
		out->daemon = section_daemon(cJSON_GetStringValue(sec));
	}
	out->persists = def->persists;

	bool fit = cJSON_PrintPreallocated(req, out->request, (int)sizeof(out->request), 0);
	cJSON_Delete(req);

	if (!fit || !out->daemon) {
		snprintf(err, errsz, "request could not be built");
		return -1;
	}
	return 0;
}

int rmq_cmd_plan(const char *json, rmq_cmd_plan_t *out, char *err, size_t errsz)
{
	cJSON *root = cJSON_Parse(json);
	if (!root) {
		memset(out, 0, sizeof(*out));
		snprintf(err, errsz, "payload is not JSON");
		return -1;
	}

	int rc = plan_parsed(root, out, err, errsz);
	cJSON_Delete(root);
	return rc;
}

/* ------------------------------------------------------------------ */
/* Deferred saves                                                      */
/* ------------------------------------------------------------------ */

static uint64_t now_ms(void)
{
	return (uint64_t)(rss_timestamp_us() / 1000);
}

/*
 * Note that a daemon has dirtied its config.
 *
 * Dragging a slider in Home Assistant is a burst of commands, and saving on
 * each one would write flash tens of times for one adjustment, so the save is
 * deferred until the burst stops.
 */
static void owe_save(rmq_state_t *st, const char *daemon)
{
	rmq_daemon_t d = rmq_daemon_by_name(daemon);
	if (d == RMQ_D_COUNT)
		return;

	uint64_t now = now_ms();
	if (!st->save_due_ms)
		st->save_first_ms = now;

	st->save_owed[d] = true;
	st->save_due_ms = now + (uint64_t)st->save_debounce_ms;

	/* A slider held down keeps pushing the deadline out, so cap the total
	 * wait: a change has to reach flash even if the burst never ends. */
	uint64_t ceiling = st->save_first_ms + RMQ_SAVE_MAX_DELAY_MS;
	if (st->save_due_ms > ceiling)
		st->save_due_ms = ceiling;
}

void rmq_cmd_flush_saves(rmq_state_t *st)
{
	if (!st->save_due_ms)
		return;

	for (int i = 0; i < RMQ_D_COUNT; i++) {
		if (!st->save_owed[i])
			continue;
		st->save_owed[i] = false;

		char sock[128];
		snprintf(sock, sizeof(sock), RSS_SOCK_FMT, rmq_daemon_name((rmq_daemon_t)i));

		char resp[256] = "";
		/*
		 * Each daemon saves only the keys it dirtied — rss_config_save
		 * edits them into the file in place rather than rewriting it —
		 * so several daemons saving the same file cannot revert each
		 * other, and a daemon with nothing dirty does not write.
		 */
		if (rss_ctrl_send_command(sock, "{\"cmd\":\"config-save\"}", resp, sizeof(resp),
					  CTRL_TIMEOUT_MS) < 0)
			RSS_WARN("cmd: %s did not answer config-save, changes stay in memory",
				 rmq_daemon_name((rmq_daemon_t)i));
	}

	st->save_due_ms = 0;
	st->save_first_ms = 0;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

int rmq_cmd_subscribe(rmq_state_t *st)
{
	if (!st->commands_enabled)
		return 0;
	/* QoS 1: a command silently dropped by the broker is worse than one
	 * delivered twice, and every command here is idempotent. */
	return rmq_mqtt_subscribe(st->mqtt, st->topic_cmd, 1);
}

static void publish_result(rmq_state_t *st, const char *cmd, const char *nonce, const char *err,
			   cJSON *daemon_resp)
{
	cJSON *root = cJSON_CreateObject();
	if (!root) {
		cJSON_Delete(daemon_resp);
		return;
	}

	cJSON_AddStringToObject(root, "status", err ? "error" : "ok");
	if (cmd)
		cJSON_AddStringToObject(root, "cmd", cmd);
	if (nonce)
		cJSON_AddStringToObject(root, "nonce", nonce);
	if (err)
		cJSON_AddStringToObject(root, "error", err);
	if (daemon_resp)
		cJSON_AddItemToObject(root, "result", daemon_resp);

	char *payload = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!payload)
		return;

	/* Not retained. A result describes one moment; replaying it to
	 * whoever subscribes next would present old news as current. */
	rmq_mqtt_publish(st->mqtt, st->topic_result, payload, strlen(payload), 1, false);
	free(payload);
}

void rmq_cmd_handle(rmq_state_t *st, const char *topic, const uint8_t *payload, size_t len)
{
	(void)topic;

	if (len == 0 || len > PAYLOAD_MAX) {
		RSS_WARN("cmd: ignoring %zu byte payload", len);
		publish_result(st, NULL, NULL, "payload empty or too large", NULL);
		return;
	}

	/* Inbound payloads are not NUL-terminated. */
	char *json = malloc(len + 1);
	if (!json)
		return;
	memcpy(json, payload, len);
	json[len] = '\0';

	cJSON *root = cJSON_Parse(json);
	free(json);

	/*
	 * The nonce and the command name are taken from the document rather
	 * than from the plan, so a refused command still comes back tagged.
	 * Without that, a sender waiting on its own nonce cannot tell a
	 * rejection from a timeout.
	 */
	char nonce_buf[NONCE_MAX + 1] = "";
	char cmd_buf[64] = "";
	if (root) {
		const cJSON *n = cJSON_GetObjectItemCaseSensitive(root, "nonce");
		if (cJSON_IsString(n) && n->valuestring)
			rss_strlcpy(nonce_buf, n->valuestring, sizeof(nonce_buf));
		const cJSON *c = cJSON_GetObjectItemCaseSensitive(root, "cmd");
		if (cJSON_IsString(c) && c->valuestring)
			rss_strlcpy(cmd_buf, c->valuestring, sizeof(cmd_buf));
	}

	/* Absent rather than empty, so the result carries no key at all when
	 * the payload named neither. */
	const char *nonce = nonce_buf[0] ? nonce_buf : NULL;
	const char *cmd_name = cmd_buf[0] ? cmd_buf : NULL;

	rmq_cmd_plan_t plan;
	char err[192];
	int rc = root ? plan_parsed(root, &plan, err, sizeof(err)) : -1;
	if (!root)
		snprintf(err, sizeof(err), "payload is not JSON");
	cJSON_Delete(root);

	if (rc < 0) {
		/* Refusals are logged, not just answered: this topic is the
		 * camera's whole management surface, so a rejected command is
		 * the one thing worth being able to find afterwards. */
		RSS_WARN("cmd: refused '%s': %s", cmd_name ? cmd_name : "(none)", err);
		publish_result(st, cmd_name, nonce, err, NULL);
		return;
	}

	if (plan.kind == RMQ_PLAN_CONFIG) {
		for (int i = 0; i < plan.write_count; i++)
			rmq_restart_stage(st, &plan.writes[i], plan.restart_owner);

		const char *owner = rmq_daemon_name(plan.restart_owner);
		RSS_INFO("cmd: staged %d config edit(s) for %s", plan.write_count, owner);

		/*
		 * Answered as accepted, not as applied: the edit is staged and
		 * the restart is still ahead of it. The result says which
		 * daemon will bounce, so a caller that cares can wait for the
		 * restart entity rather than guess.
		 */
		cJSON *r = cJSON_CreateObject();
		if (r) {
			cJSON_AddStringToObject(r, "status", "staged");
			cJSON_AddNumberToObject(r, "edits", plan.write_count);
			cJSON_AddStringToObject(r, "restarts", owner);
		}
		publish_result(st, cmd_name, nonce, NULL, r);
		return;
	}

	if (plan.kind == RMQ_PLAN_REBOOT) {
		/*
		 * Answered before it happens, because afterwards there is
		 * nothing left to answer with — and the answer is the only
		 * evidence the button did anything at all. The status topic
		 * goes to offline first for the same reason: the Last Will
		 * would eventually say so, but only after the broker's
		 * keepalive times out, which leaves every entity looking live
		 * for half a minute while the camera is already down.
		 */
		cJSON *r = cJSON_CreateObject();
		if (r)
			cJSON_AddStringToObject(r, "status", "rebooting");
		publish_result(st, cmd_name, nonce, NULL, r);
		rmq_mqtt_publish(st->mqtt, st->topic_status, RMQ_STATUS_OFFLINE,
				 strlen(RMQ_STATUS_OFFLINE), 1, true);
		rmq_mqtt_loop(st->mqtt, 200);

		RSS_WARN("reboot: requested over MQTT");

		/*
		 * Through init rather than the reboot(2) syscall, so the init
		 * scripts stop the daemons in their own order and the flash is
		 * unmounted cleanly. /etc is an overlay over NOR; pulling the
		 * rug is how a config file ends up half written.
		 */
		sync();
		if (system("/sbin/reboot") != 0)
			RSS_WARN("reboot: /sbin/reboot did not run");
		return;
	}

	if (plan.kind == RMQ_PLAN_SYSTEM) {
		const char *k = plan.writes[0].key;
		const char *v = plan.writes[0].value;
		bool tz = strcmp(k, "timezone") == 0;

		int rc = tz ? rmq_system_set_timezone(v) : rmq_system_set_ntp_server(v);
		if (rc != 0) {
			publish_result(st, cmd_name, nonce, "the file could not be written", NULL);
			return;
		}

		cJSON *r = cJSON_CreateObject();
		if (r) {
			cJSON_AddStringToObject(r, "status", "ok");
			cJSON_AddStringToObject(r, k, v);
			/* Said in the answer as well as in the entity name: the
			 * timezone is exported once at boot and a daemon
			 * restart re-execs with the environment it already had,
			 * so nothing short of a reboot moves the clock a
			 * running daemon renders with. */
			if (tz)
				cJSON_AddStringToObject(r, "applies", "on reboot");
		}
		publish_result(st, cmd_name, nonce, NULL, r);
		return;
	}

	if (plan.kind == RMQ_PLAN_SNAPSHOT) {
		if (!st->snapshot_enabled) {
			publish_result(st, cmd_name, nonce,
				       "snapshots are off — set [mqtt] snapshot = true", NULL);
			return;
		}

		int rc = rmq_snapshot_capture(st);
		cJSON *r = cJSON_CreateObject();
		if (r)
			cJSON_AddStringToObject(r, "status", rc == 0 ? "ok" : "no frame");
		publish_result(st, cmd_name, nonce,
			       rc == 0 ? NULL : "no JPEG frame arrived — is [jpeg] enabled?", r);
		return;
	}

	char sock[128];
	snprintf(sock, sizeof(sock), RSS_SOCK_FMT, plan.daemon);

	char *resp = malloc(RESP_MAX);
	if (!resp)
		return;

	if (rss_ctrl_send_command(sock, plan.request, resp, RESP_MAX, CTRL_TIMEOUT_MS) < 0) {
		free(resp);
		snprintf(err, sizeof(err), "%s is not running or did not answer", plan.daemon);
		RSS_WARN("cmd: %s", err);
		publish_result(st, cmd_name, nonce, err, NULL);
		return;
	}

	RSS_INFO("cmd: %s -> %s: %s", cmd_name ? cmd_name : "(none)", plan.daemon, plan.request);

	/*
	 * Daemons answer with either a JSON object or a bare string, so both
	 * are carried through as-is. A daemon that answers status "error"
	 * accepted the command and then failed it, which is a different thing
	 * from a refusal here — but the sender wants one place to look, so it
	 * is surfaced at the top level too.
	 */
	cJSON *dresp = cJSON_Parse(resp);
	if (!dresp)
		dresp = cJSON_CreateString(resp);
	free(resp);

	const char *fail = NULL;
	if (cJSON_IsObject(dresp)) {
		const cJSON *s = cJSON_GetObjectItemCaseSensitive(dresp, "status");
		if (cJSON_IsString(s) && s->valuestring && strcmp(s->valuestring, "ok") != 0) {
			const cJSON *e = cJSON_GetObjectItemCaseSensitive(dresp, "error");
			snprintf(err, sizeof(err), "%s: %s", plan.daemon,
				 cJSON_IsString(e) && e->valuestring ? e->valuestring
								     : s->valuestring);
			fail = err;
		}
	}

	if (!fail && plan.persists)
		owe_save(st, plan.daemon);

	publish_result(st, cmd_name, nonce, fail, dresp);
}
