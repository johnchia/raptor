/*
 * rcd_schema.c -- see rcd_schema.h
 */

#include "rcd_schema.h"

#include <rss_common.h>

#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Daemons                                                             */
/* ------------------------------------------------------------------ */

static const char *const daemon_names[RCD_D_COUNT] = {
	[RCD_D_RVD] = "rvd", [RCD_D_RSD] = "rsd", [RCD_D_RAD] = "rad",
	[RCD_D_ROD] = "rod", [RCD_D_RIC] = "ric", [RCD_D_RMR] = "rmr",
	[RCD_D_RMD] = "rmd", [RCD_D_RHD] = "rhd", [RCD_D_RWD] = "rwd",
};

const char *rcd_daemon_name(rcd_daemon_t d)
{
	return (d >= 0 && d < RCD_D_COUNT) ? daemon_names[d] : "?";
}

rcd_daemon_t rcd_daemon_by_name(const char *name)
{
	for (int i = 0; name && i < RCD_D_COUNT; i++) {
		if (strcmp(name, daemon_names[i]) == 0)
			return (rcd_daemon_t)i;
	}
	return RCD_D_COUNT;
}

const char *rcd_impact_name(rcd_impact_t i)
{
	switch (i) {
	case RCD_IMPACT_NONE:
		return "none";
	case RCD_IMPACT_SERVICE:
		return "service";
	case RCD_IMPACT_STREAM:
		return "stream";
	case RCD_IMPACT_PIPELINE:
		return "pipeline";
	}
	return "none";
}

/*
 * rvd is alone in the top class and the reason the class exists: restarting it
 * tears MI down for the whole system, so capture stops and every daemon
 * downstream reconnects. The servers below it drop whoever is connected. The
 * rest interrupt a feature that nobody is watching a socket for.
 */
rcd_impact_t rcd_daemon_impact(rcd_daemon_t d)
{
	switch (d) {
	case RCD_D_RVD:
		return RCD_IMPACT_PIPELINE;
	case RCD_D_RSD:
	case RCD_D_RHD:
	case RCD_D_RWD:
		return RCD_IMPACT_STREAM;
	default:
		return RCD_IMPACT_SERVICE;
	}
}

/* ------------------------------------------------------------------ */
/* Sections                                                            */
/*                                                                     */
/* Readable and writable are different lists on purpose. [rtsp] and     */
/* [http] hold a password, so neither can be read in bulk -- but their  */
/* individual keys can still be written, because a credential that      */
/* cannot be set is a credential stuck at whatever the image shipped.   */
/* Settable, never reported back.                                       */
/* ------------------------------------------------------------------ */

static const struct {
	const char *section;
	const char *daemon;
} readable[] = {
	{"sensor", "rvd"},    {"image", "rvd"}, {"stream0", "rvd"}, {"stream1", "rvd"},
	{"jpeg", "rvd"},      {"ring", "rvd"},	{"log", "rvd"},	    {"audio", "rad"},
	{"osd", "rod"},	      {"ircut", "ric"}, {"motion", "rmd"},  {"recording", "rmr"},
	{"timelapse", "rmr"}, {NULL, NULL},
};

static const struct {
	const char *section;
	rcd_daemon_t owner;
} writable[] = {
	{"sensor", RCD_D_RVD},	  {"stream0", RCD_D_RVD}, {"stream1", RCD_D_RVD},
	{"image", RCD_D_RVD},	  {"jpeg", RCD_D_RVD},	  {"audio", RCD_D_RAD},
	{"rtsp", RCD_D_RSD},	  {"http", RCD_D_RHD},	  {"osd", RCD_D_ROD},
	{"ircut", RCD_D_RIC},	  {"motion", RCD_D_RMD},  {"recording", RCD_D_RMR},
	{"timelapse", RCD_D_RMR}, {NULL, RCD_D_COUNT},
};

const char *rcd_section_reader(const char *section)
{
	for (int i = 0; section && readable[i].section; i++) {
		if (strcmp(section, readable[i].section) == 0)
			return readable[i].daemon;
	}
	return NULL;
}

rcd_daemon_t rcd_section_owner(const char *section)
{
	for (int i = 0; section && writable[i].section; i++) {
		if (strcmp(section, writable[i].section) == 0)
			return writable[i].owner;
	}
	return RCD_D_COUNT;
}

/* ------------------------------------------------------------------ */
/* Enumerations                                                        */
/* ------------------------------------------------------------------ */

static const char *const choices_on_off[] = {"on", "off", NULL};
static const char *const choices_daynight[] = {"auto", "day", "night", NULL};
static const char *const choices_vcodec[] = {"h264", "h265", NULL};
static const char *const choices_acodec[] = {"pcmu", "pcma", "l16", "aac", "opus", NULL};
static const char *const choices_ainput[] = {"amic", "dmic", NULL};
static const char *const choices_arate[] = {"8000", "16000", "32000", "48000", NULL};
static const char *const choices_trigger[] = {"luma", "gain", "adc", "photo", NULL};
static const char *const choices_algorithm[] = {"move", "base_move", "persondet", "yolo", NULL};
static const char *const choices_recmode[] = {"continuous", "motion", "both", NULL};

/* Mirrors rvd's rc_map (rvd_ctrl.c). rvd silently falls back to CBR for a name
 * it does not know, so a typo would otherwise change the rate control without
 * saying so -- refusing here is what makes that visible. */
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
/* The writable keys                                                   */
/*                                                                     */
/* A key naming a live command is applied to the running daemon and     */
/* costs nothing. A key without one is written to the file and read on  */
/* the owner's next start, which is what `apply` is for. Which of the   */
/* two a key is belongs here rather than in any caller.                 */
/* ------------------------------------------------------------------ */

#define LIVE(cmd)     cmd, "value", -1
#define LIVE_CH(c, n) c, "value", n
#define SAVED	      NULL, NULL, -1

static const rcd_key_t keys[] = {
	/* -- Sensor -- */
	{"sensor", "fps", V_INT, 1, 120, NULL, SAVED},
	{"sensor", "antiflicker", V_INT, 0, 2, NULL, SAVED},

	/*
	 * -- Video. Resolution is the reason the restart tier exists at all:
	 *    an encoder is created at its size and cannot be resized. Rate and
	 *    GOP are the opposite -- the encoder takes them while it runs, so
	 *    they carry the channel their command needs.
	 */
	{"stream0", "width", V_INT, 160, 4096, NULL, SAVED},
	{"stream0", "height", V_INT, 120, 4096, NULL, SAVED},
	{"stream0", "codec", V_ENUM, 0, 0, choices_vcodec, SAVED},
	{"stream0", "profile", V_INT, 0, 2, NULL, SAVED},
	{"stream0", "bitrate", V_INT, 32000, 50000000, NULL, LIVE_CH("set-bitrate", 0)},
	{"stream0", "gop", V_INT, 1, 300, NULL, LIVE_CH("set-gop", 0)},
	{"stream0", "fps", V_INT, 1, 120, NULL, LIVE_CH("set-fps", 0)},
	{"stream0", "osd_enabled", V_BOOL, 0, 0, NULL, SAVED},
	{"stream0", "jpeg", V_BOOL, 0, 0, NULL, SAVED},
	{"stream1", "enabled", V_BOOL, 0, 0, NULL, SAVED},
	{"stream1", "width", V_INT, 160, 4096, NULL, SAVED},
	{"stream1", "height", V_INT, 120, 4096, NULL, SAVED},
	{"stream1", "codec", V_ENUM, 0, 0, choices_vcodec, SAVED},
	{"stream1", "profile", V_INT, 0, 2, NULL, SAVED},
	{"stream1", "bitrate", V_INT, 32000, 50000000, NULL, LIVE_CH("set-bitrate", 1)},
	{"stream1", "gop", V_INT, 1, 300, NULL, LIVE_CH("set-gop", 1)},
	{"stream1", "fps", V_INT, 1, 120, NULL, LIVE_CH("set-fps", 1)},
	{"stream1", "osd_enabled", V_BOOL, 0, 0, NULL, SAVED},
	{"stream1", "jpeg", V_BOOL, 0, 0, NULL, SAVED},

	/*
	 * -- Image. Every key of the section is live, because tuning is done
	 *    by looking at the picture and a restart between adjustments makes
	 *    that impossible.
	 *
	 *    Three of them are live only on some hardware: SigmaStar carries
	 *    orientation and the 3DNR level in a creation-time call, so rvd
	 *    refuses them while its channel is running. They are declared live
	 *    all the same -- rcd tries the command, and a refusal falls back
	 *    to the file with the outcome reported per edit, which is right on
	 *    both families without either one knowing about the other.
	 *
	 *    Orientation is 0/1 rather than a boolean because rvd reads it
	 *    with rss_config_get_int: `true` parses as no number at all and
	 *    falls back to the default, so a flip written that way is silently
	 *    not applied. The type here is the one the owning daemon reads with.
	 */
	{"image", "brightness", V_INT, 0, 255, NULL, LIVE("set-brightness")},
	{"image", "contrast", V_INT, 0, 255, NULL, LIVE("set-contrast")},
	{"image", "saturation", V_INT, 0, 255, NULL, LIVE("set-saturation")},
	{"image", "sharpness", V_INT, 0, 255, NULL, LIVE("set-sharpness")},
	{"image", "hue", V_INT, 0, 255, NULL, LIVE("set-hue")},
	{"image", "sinter", V_INT, 0, 255, NULL, LIVE("set-sinter")},
	{"image", "temper", V_INT, 0, 255, NULL, LIVE("set-temper")},
	{"image", "ae_comp", V_INT, 0, 255, NULL, LIVE("set-ae-comp")},
	{"image", "max_again", V_INT, 0, 160, NULL, LIVE("set-max-again")},
	{"image", "max_dgain", V_INT, 0, 160, NULL, LIVE("set-max-dgain")},
	{"image", "dpc_strength", V_INT, 0, 255, NULL, LIVE("set-dpc")},
	{"image", "drc_strength", V_INT, 0, 255, NULL, LIVE("set-drc")},
	{"image", "defog_strength", V_INT, 0, 255, NULL, LIVE("set-defog-strength")},
	{"image", "highlight_depress", V_INT, 0, 255, NULL, LIVE("set-highlight-depress")},
	{"image", "backlight_comp", V_INT, 0, 10, NULL, LIVE("set-backlight-comp")},
	{"image", "hflip", V_INT, 0, 1, NULL, LIVE("set-hflip")},
	{"image", "vflip", V_INT, 0, 1, NULL, LIVE("set-vflip")},

	/* -- Snapshots -- */
	{"jpeg", "enabled", V_BOOL, 0, 0, NULL, SAVED},
	{"jpeg", "quality", V_INT, 1, 100, NULL, SAVED},
	{"jpeg", "fps", V_INT, 1, 30, NULL, SAVED},
	{"jpeg", "idle", V_BOOL, 0, 0, NULL, SAVED},

	/* -- Audio. The levels are live; what the stream is made of is not. -- */
	{"audio", "enabled", V_BOOL, 0, 0, NULL, SAVED},
	{"audio", "input", V_ENUM, 0, 0, choices_ainput, SAVED},
	{"audio", "sample_rate", V_ENUM, 0, 0, choices_arate, SAVED},
	{"audio", "codec", V_ENUM, 0, 0, choices_acodec, SAVED},
	{"audio", "bitrate", V_INT, 8000, 320000, NULL, SAVED},
	/* Both SoC families take volume as 0-100 and gain as the ADC's steps. */
	{"audio", "volume", V_INT, 0, 100, NULL, LIVE("set-volume")},
	{"audio", "gain", V_INT, 0, 31, NULL, LIVE("set-gain")},
	{"audio", "ao_volume", V_INT, 0, 100, NULL, LIVE("ao-set-volume")},
	{"audio", "ao_gain", V_INT, 0, 31, NULL, LIVE("ao-set-gain")},
	{"audio", "aec_enabled", V_BOOL, 0, 0, NULL, LIVE("set-aec")},
	{"audio", "hpf_enabled", V_BOOL, 0, 0, NULL, LIVE("set-hpf")},

	/* -- RTSP and HTTP. Both sections hold a username and password, and
	 *    `credentials-set` writes the pair in one command so the camera
	 *    has one account rather than two that drift. -- */
	{"rtsp", "enabled", V_BOOL, 0, 0, NULL, SAVED},
	{"rtsp", "port", V_INT, 1, 65535, NULL, SAVED},
	{"rtsp", "max_clients", V_INT, 1, 32, NULL, SAVED},
	{"rtsp", "session_timeout", V_INT, 10, 3600, NULL, SAVED},
	{"rtsp", "idr_on_join", V_BOOL, 0, 0, NULL, SAVED},
	/* rsd enables Digest auth, and rhd Basic auth, only when both the
	 * username and the password are set -- so clearing either one turns
	 * authentication off, which is the only way to turn it off and is why
	 * an empty value is accepted here. */
	{"rtsp", "username", V_CRED, 0, 63, NULL, SAVED},
	{"rtsp", "password", V_CRED, 0, 63, NULL, SAVED},
	{"http", "enabled", V_BOOL, 0, 0, NULL, SAVED},
	{"http", "port", V_INT, 1, 65535, NULL, SAVED},
	{"http", "max_clients", V_INT, 1, 32, NULL, SAVED},
	{"http", "username", V_CRED, 0, 63, NULL, SAVED},
	{"http", "password", V_CRED, 0, 63, NULL, SAVED},

	/* -- OSD -- */
	{"osd", "enabled", V_BOOL, 0, 0, NULL, SAVED},
	{"osd", "font_size", V_INT, 8, 96, NULL, SAVED},
	{"osd", "font_stroke", V_INT, 0, 5, NULL, SAVED},

	/* -- Day/night. The luma and gain thresholds are an action rather than
	 *    keys; what is here is everything ric reads only at startup -- how
	 *    the board is wired, and how the two triggers that are not luma
	 *    are calibrated. -- */
	{"ircut", "enabled", V_BOOL, 0, 0, NULL, SAVED},
	{"ircut", "trigger", V_ENUM, 0, 0, choices_trigger, SAVED},
	{"ircut", "pulse_ms", V_INT, 1, 1000, NULL, SAVED},
	/*
	 * GPIO pin assignments. These describe the board rather than any
	 * behaviour, and are normally read from /etc/thingino.json -- but that
	 * file is absent on an OpenIPC base, which leaves the config the only
	 * place to put them.
	 *
	 * A wrong pin here drives a wrong pin: the ceiling is the SoC's GPIO
	 * count rather than anything ric knows to be safe. Bounded, not made
	 * harmless.
	 */
	{"ircut", "gpio_ircut", V_INT, -1, 127, NULL, SAVED},
	{"ircut", "gpio_ircut2", V_INT, -1, 127, NULL, SAVED},
	{"ircut", "gpio_irled", V_INT, -1, 127, NULL, SAVED},
	{"ircut", "gpio_irled2", V_INT, -1, 127, NULL, SAVED},
	/* trigger = adc: a photoresistor on an ADC channel, 12-bit. */
	{"ircut", "adc_channel", V_INT, 0, 7, NULL, SAVED},
	{"ircut", "adc_night", V_INT, 0, 4095, NULL, SAVED},
	{"ircut", "adc_day", V_INT, 0, 4095, NULL, SAVED},
	/* trigger = photo: EV thresholds, where higher means darker. */
	{"ircut", "photo_ev_night", V_INT, 0, 10000000, NULL, SAVED},
	{"ircut", "photo_ev_deep", V_INT, 0, 10000000, NULL, SAVED},
	{"ircut", "photo_ev_day", V_INT, 0, 10000000, NULL, SAVED},
	/* AWB baselines for the same trigger; 0 self-calibrates from the
	 * sensor, so the range starts there rather than at 1x (1024). */
	{"ircut", "photo_rgain_rec", V_INT, 0, 8192, NULL, SAVED},
	{"ircut", "photo_bgain_rec", V_INT, 0, 8192, NULL, SAVED},

	/* -- Motion -- */
	{"motion", "enabled", V_BOOL, 0, 0, NULL, SAVED},
	{"motion", "algorithm", V_ENUM, 0, 0, choices_algorithm, SAVED},
	{"motion", "sensitivity", V_INT, 0, 5, NULL, SAVED},
	{"motion", "cooldown_sec", V_INT, 1, 3600, NULL, SAVED},
	{"motion", "record", V_BOOL, 0, 0, NULL, SAVED},
	{"motion", "record_post_sec", V_INT, 0, 600, NULL, SAVED},

	/* -- Recording. storage_path is absent with the rest of the strings,
	 *    which also keeps the write path away from the filesystem. -- */
	{"recording", "enabled", V_BOOL, 0, 0, NULL, SAVED},
	{"recording", "mode", V_ENUM, 0, 0, choices_recmode, SAVED},
	{"recording", "stream", V_INT, 0, 1, NULL, SAVED},
	{"recording", "audio", V_BOOL, 0, 0, NULL, SAVED},
	{"recording", "segment_minutes", V_INT, 1, 1440, NULL, SAVED},
	{"recording", "max_storage_mb", V_INT, 0, 1000000, NULL, SAVED},
	{"recording", "prebuffer_sec", V_INT, 0, 5, NULL, SAVED},

	/* -- Timelapse -- */
	{"timelapse", "enabled", V_BOOL, 0, 0, NULL, SAVED},
	{"timelapse", "interval", V_INT, 2, 86400, NULL, SAVED},
	{"timelapse", "playback_fps", V_INT, 1, 120, NULL, SAVED},
	{"timelapse", "max_mb", V_INT, 0, 1000000, NULL, SAVED},

	{NULL, NULL, V_INT, 0, 0, NULL, SAVED},
};

const rcd_key_t *rcd_key_find(const char *section, const char *key)
{
	if (!section || !key)
		return NULL;
	for (int i = 0; keys[i].section; i++) {
		if (strcmp(keys[i].section, section) == 0 && strcmp(keys[i].key, key) == 0)
			return &keys[i];
	}
	return NULL;
}

const rcd_key_t *rcd_key_at(int i)
{
	if (i < 0 || i >= (int)(sizeof(keys) / sizeof(keys[0])) - 1)
		return NULL;
	return &keys[i];
}

/* ------------------------------------------------------------------ */
/* Actions                                                             */
/*                                                                     */
/* Verbs that are not a single config value: a momentary command, or    */
/* one taking more arguments than a value. Every one takes effect       */
/* without interrupting the camera -- `apply` and `restart` are         */
/* protocol commands rather than entries here precisely because they    */
/* do interrupt, and nothing that merely stops a daemon is reachable    */
/* from either.                                                        */
/* ------------------------------------------------------------------ */

static const rcd_arg_t args_none[] = {
	{.type = A_END},
};

static const rcd_arg_t args_channel_opt[] = {
	{.key = "channel", .type = A_INT, .required = false, .min = 0, .max = 3},
	{.type = A_END},
};

/* H.264 and H.265 both use QP 0-51. */
static const rcd_arg_t args_qp_bounds[] = {
	{.key = "channel", .type = A_INT, .required = true, .min = 0, .max = 3},
	{.key = "min", .type = A_INT, .required = true, .min = 0, .max = 51},
	{.key = "max", .type = A_INT, .required = true, .min = 0, .max = 51},
	{.type = A_END},
};

static const rcd_arg_t args_rc_mode[] = {
	{.key = "channel", .type = A_INT, .required = true, .min = 0, .max = 3},
	{.key = "mode", .type = A_ENUM, .required = true, .choices = choices_rc_mode},
	{.key = "bitrate", .type = A_INT, .required = false, .min = 32000, .max = 50000000},
	{.type = A_END},
};

static const rcd_arg_t args_daynight[] = {
	{.key = "value", .type = A_ENUM, .required = true, .choices = choices_daynight},
	{.type = A_END},
};

static const rcd_arg_t args_on_off[] = {
	{.key = "value", .type = A_ENUM, .required = true, .choices = choices_on_off},
	{.type = A_END},
};

static const rcd_arg_t args_threshold[] = {
	{.key = "key", .type = A_ENUM, .required = true, .choices = choices_threshold},
	{.key = "value", .type = A_INT, .required = true, .min = 0, .max = 1000000},
	{.type = A_END},
};

static const rcd_action_t actions[] = {
	{"request-idr", "rvd", NULL, args_channel_opt, false},
	{"set-qp-bounds", "rvd", NULL, args_qp_bounds, true},
	{"set-rc-mode", "rvd", NULL, args_rc_mode, true},

	/* -- Day/night. The LED banks are a manual override of automatic
	 *    behaviour rather than a setting, which is why they persist
	 *    nothing: the next auto transition takes them back. -- */
	{"ircut-mode", "ric", "mode", args_daynight, true},
	{"ircut-threshold", "ric", "set-threshold", args_threshold, true},
	{"ir850", "ric", NULL, args_on_off, false},
	{"ir940", "ric", NULL, args_on_off, false},

	/* -- OSD. rod pauses rendering rather than writing [osd] enabled, so
	 *    this deliberately persists nothing: a reboot restores the
	 *    configured state, which is what rod already does. -- */
	{"osd-enable", "rod", "enable", args_none, false},
	{"osd-disable", "rod", "disable", args_none, false},

	{NULL, NULL, NULL, NULL, false},
};

const rcd_action_t *rcd_action_find(const char *name)
{
	for (int i = 0; name && actions[i].name; i++) {
		if (strcmp(actions[i].name, name) == 0)
			return &actions[i];
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Serialization                                                       */
/* ------------------------------------------------------------------ */

static const char *type_name(rcd_val_type_t t)
{
	switch (t) {
	case V_INT:
		return "int";
	case V_BOOL:
		return "bool";
	case V_ENUM:
		return "enum";
	case V_CRED:
		return "credential";
	}
	return "int";
}

static void emit_choices(cJSON *o, const char *const *choices)
{
	cJSON *a = cJSON_AddArrayToObject(o, "choices");
	for (int i = 0; a && choices[i]; i++)
		cJSON_AddItemToArray(a, cJSON_CreateString(choices[i]));
}

static void emit_key(cJSON *arr, const rcd_key_t *k)
{
	cJSON *o = cJSON_CreateObject();
	if (!o)
		return;

	cJSON_AddStringToObject(o, "section", k->section);
	cJSON_AddStringToObject(o, "key", k->key);
	cJSON_AddStringToObject(o, "type", type_name(k->type));

	if (k->type == V_INT) {
		cJSON_AddNumberToObject(o, "min", k->min);
		cJSON_AddNumberToObject(o, "max", k->max);
	} else if (k->type == V_CRED) {
		cJSON_AddNumberToObject(o, "max_length", k->max);
	} else if (k->type == V_ENUM) {
		emit_choices(o, k->choices);
	}

	rcd_daemon_t owner = rcd_section_owner(k->section);
	cJSON_AddStringToObject(o, "owner", rcd_daemon_name(owner));

	/*
	 * The whole point of serving this table: a client knows before it
	 * submits whether a field costs nothing or costs an outage, and can
	 * say so next to the field rather than after the fact.
	 */
	bool live = k->live_cmd != NULL;
	cJSON_AddStringToObject(o, "tier", live ? "live" : "restart");
	cJSON_AddStringToObject(o, "impact",
				rcd_impact_name(live ? RCD_IMPACT_NONE : rcd_daemon_impact(owner)));

	/* A credential is settable and never readable, and a client that does
	 * not know that draws an input which always looks empty and calls it a
	 * bug. Said plainly instead. */
	if (k->type == V_CRED || !rcd_section_reader(k->section))
		cJSON_AddBoolToObject(o, "readable", false);

	cJSON_AddItemToArray(arr, o);
}

static void emit_action(cJSON *arr, const rcd_action_t *a)
{
	cJSON *o = cJSON_CreateObject();
	if (!o)
		return;

	cJSON_AddStringToObject(o, "name", a->name);
	if (a->daemon) {
		cJSON_AddStringToObject(o, "owner", a->daemon);
		cJSON_AddStringToObject(o, "impact", rcd_impact_name(RCD_IMPACT_NONE));
	}

	cJSON *args = cJSON_AddArrayToObject(o, "args");
	for (int i = 0; args && a->args[i].type != A_END; i++) {
		cJSON *ao = cJSON_CreateObject();
		if (!ao)
			continue;
		cJSON_AddStringToObject(ao, "key", a->args[i].key);
		switch (a->args[i].type) {
		case A_INT:
			cJSON_AddStringToObject(ao, "type", "int");
			cJSON_AddNumberToObject(ao, "min", a->args[i].min);
			cJSON_AddNumberToObject(ao, "max", a->args[i].max);
			break;
		case A_ENUM:
			cJSON_AddStringToObject(ao, "type", "enum");
			emit_choices(ao, a->args[i].choices);
			break;
		case A_SECTION:
			cJSON_AddStringToObject(ao, "type", "section");
			break;
		case A_END:
			break;
		}
		cJSON_AddBoolToObject(ao, "required", a->args[i].required);
		cJSON_AddItemToArray(args, ao);
	}

	cJSON_AddItemToArray(arr, o);
}

void rcd_schema_emit(cJSON *out, const char *section_filter)
{
	cJSON *karr = cJSON_AddArrayToObject(out, "keys");
	for (int i = 0; karr && keys[i].section; i++) {
		if (section_filter && strcmp(section_filter, keys[i].section) != 0)
			continue;
		emit_key(karr, &keys[i]);
	}

	/* Actions belong to no section, so a filtered schema leaves them out
	 * rather than repeating all of them under every section asked for. */
	if (section_filter)
		return;

	cJSON *aarr = cJSON_AddArrayToObject(out, "actions");
	for (int i = 0; aarr && actions[i].name; i++)
		emit_action(aarr, &actions[i]);
}
