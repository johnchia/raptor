/*
 * rcd_schema.c -- see rcd_schema.h
 */

#include "rcd_schema.h"

#include "rcd_guard.h"
#include "rcd_network.h"
#include "rcd_system.h"
#include "rcd_wifi.h"

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
	case RCD_IMPACT_NETWORK:
		return "network";
	case RCD_IMPACT_REBOOT:
		return "reboot";
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
/* Readable and writable are different lists on purpose: a section may  */
/* have no daemon able to answer for it, and then its values come from  */
/* the file alone.                                                      */
/*                                                                     */
/* [rtsp] and [http] hold a password and are read anyway. What protects */
/* it is not the section being absent from this list -- it is that a    */
/* value is emitted key by key from the table and a V_CRED returns      */
/* before any value is looked at, so the password is never on the wire  */
/* whatever the daemon put in its reply. Keeping the sections out of    */
/* here bought nothing and cost the truth: with every key commented out */
/* of raptor.conf, which is the shipped state, a client asking whether  */
/* RTSP is on was told "unset" while the server was serving.            */
/* ------------------------------------------------------------------ */

static const struct {
	const char *section;
	const char *daemon;
} readable[] = {
	{"sensor", "rvd"},
	{"image", "rvd"},
	{"stream0", "rvd"},
	{"stream1", "rvd"},
	{"jpeg", "rvd"},
	{"ring", "rvd"},
	{"log", "rvd"},
	{"audio", "rad"},
	{"osd", "rod"},
	{"ircut", "ric"},
	{"motion", "rmd"},
	{"recording", "rmr"},
	{"timelapse", "rmr"},
	{"rtsp", "rsd"},
	{"http", "rhd"},
	/* One per place on the picture; see the [osd.*] keys below. */
	{"osd.top_left", "rod"},
	{"osd.top_center", "rod"},
	{"osd.top_right", "rod"},
	{"osd.bottom_left", "rod"},
	{"osd.bottom_center", "rod"},
	{"osd.bottom_right", "rod"},
	{NULL, NULL},
};

static const struct {
	const char *section;
	rcd_daemon_t owner;
} writable[] = {
	{"sensor", RCD_D_RVD},
	{"stream0", RCD_D_RVD},
	{"stream1", RCD_D_RVD},
	{"image", RCD_D_RVD},
	{"jpeg", RCD_D_RVD},
	{"audio", RCD_D_RAD},
	{"rtsp", RCD_D_RSD},
	{"http", RCD_D_RHD},
	{"osd", RCD_D_ROD},
	{"ircut", RCD_D_RIC},
	{"motion", RCD_D_RMD},
	{"recording", RCD_D_RMR},
	{"timelapse", RCD_D_RMR},
	{"device", RCD_D_COUNT},
	{"network", RCD_D_COUNT},
	{"wifi", RCD_D_COUNT},
	{"osd.top_left", RCD_D_ROD},
	{"osd.top_center", RCD_D_ROD},
	{"osd.top_right", RCD_D_ROD},
	{"osd.bottom_left", RCD_D_ROD},
	{"osd.bottom_center", RCD_D_ROD},
	{"osd.bottom_right", RCD_D_ROD},
	{NULL, RCD_D_COUNT},
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
static const char *const choices_align[] = {"left", "center", "right", NULL};
static const char *const choices_algorithm[] = {"move", "base_move", "persondet", "yolo", NULL};
static const char *const choices_recmode[] = {"continuous", "motion", "both", NULL};

/*
 * Integer keys whose values are names rather than magnitudes.
 *
 * The array labels the value `min + i` and is display only: what reaches the
 * file is still the number, because rvd and rmr read these with
 * rss_config_get_int, whose strtol answers a spelled-out value by returning
 * the default. Writing "baseline" would therefore select High, silently and
 * only on the next start. A name is accepted on input and mapped here, so a
 * caller may send either the number or the name and the file is the same
 * either way.
 */
static const char *const labels_antiflicker[] = {"off", "50hz", "60hz", NULL};
static const char *const labels_profile[] = {"baseline", "main", "high", NULL};
static const char *const labels_recstream[] = {"main", "sub", NULL};

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

/*
 * The tail of an entry: how it is applied, where it is kept, what it costs.
 * Written as macros because that tail is the same for almost every key, and
 * because -Werror=missing-field-initializers means the whole of it has to be
 * spelled out either way.
 */
#define LIVE(cmd)     cmd, "value", -1, NULL, NULL, RCD_IMPACT_NONE, 0, false, NULL
#define LIVE_CH(c, n) c, "value", n, NULL, NULL, RCD_IMPACT_NONE, 0, false, NULL

/* A live command whose value is not called `value`. rvd's set-rc-mode names
 * its argument `mode` because the command carries a bitrate as well, and a
 * key is applied by the command the daemon already has rather than by one
 * added to spell the field the way this table would prefer. */
#define LIVE_CH_ARG(c, a, n) c, a, n, NULL, NULL, RCD_IMPACT_NONE, 0, false, NULL

/* A live command that answers for a family of settings and is told which one
 * by name. See rcd_key_t::live_sel. */
#define LIVE_SEL(cmd, sel) cmd, "value", -1, sel, NULL, RCD_IMPACT_NONE, 0, false, NULL

#define SAVED		   NULL, NULL, -1, NULL, NULL, RCD_IMPACT_NONE, 0, false, NULL
#define PROVIDED(p, imp)   NULL, NULL, -1, NULL, &(p), (imp), 0, false, NULL

/* An ISP knob: live like the rest, and it takes the word "auto" as well as a
 * number. See rcd_key_t::auto_ok -- the word says "follow the tuning's own
 * curve", which no number on the scale can say.
 *
 * And it is put back by asking rvd rather than by restarting it: the ISP keeps
 * its state in the driver, so a restart re-reads a file that no longer names
 * the key and writes nothing over the value left behind. See
 * rcd_key_t::live_reset. */
#define LIVE_ISP(cmd) cmd, "value", -1, NULL, NULL, RCD_IMPACT_NONE, 0, true, "reset-isp"

/* A provider whose value can cost the client its way back in: written like
 * any other, and put back if nobody confirms within `sec`. */
#define GUARDED(p, imp, sec) NULL, NULL, -1, NULL, &(p), (imp), (sec), false, NULL

static const rcd_key_t keys[] = {
	/* -- Sensor -- */
	{"sensor", "fps", V_INT, 1, 120, NULL, SAVED},
	{"sensor", "antiflicker", V_INT, 0, 2, labels_antiflicker, SAVED},

	/*
	 * -- Video. Resolution is the reason the restart tier exists at all:
	 *    an encoder is created at its size and cannot be resized. Rate,
	 *    rate control and GOP are the opposite -- the encoder takes them
	 *    while it runs, so they carry the channel their command needs.
	 */
	{"stream0", "width", V_INT, 160, 4096, NULL, SAVED},
	{"stream0", "height", V_INT, 120, 4096, NULL, SAVED},
	{"stream0", "codec", V_ENUM, 0, 0, choices_vcodec, SAVED},
	{"stream0", "profile", V_INT, 0, 2, labels_profile, SAVED},
	{"stream0", "rc_mode", V_ENUM, 0, 0, choices_rc_mode,
	 LIVE_CH_ARG("set-rc-mode", "mode", 0)},
	{"stream0", "bitrate", V_INT, 32000, 50000000, NULL, LIVE_CH("set-bitrate", 0)},
	{"stream0", "gop", V_INT, 1, 300, NULL, LIVE_CH("set-gop", 0)},
	{"stream0", "fps", V_INT, 1, 120, NULL, LIVE_CH("set-fps", 0)},
	{"stream0", "osd_enabled", V_BOOL, 0, 0, NULL, SAVED},
	{"stream0", "jpeg", V_BOOL, 0, 0, NULL, SAVED},
	{"stream1", "enabled", V_BOOL, 0, 0, NULL, SAVED},
	{"stream1", "width", V_INT, 160, 4096, NULL, SAVED},
	{"stream1", "height", V_INT, 120, 4096, NULL, SAVED},
	{"stream1", "codec", V_ENUM, 0, 0, choices_vcodec, SAVED},
	{"stream1", "profile", V_INT, 0, 2, labels_profile, SAVED},
	{"stream1", "rc_mode", V_ENUM, 0, 0, choices_rc_mode,
	 LIVE_CH_ARG("set-rc-mode", "mode", 1)},
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
	/*
	 * The ISP knobs' bounds here are the widest any platform accepts, not
	 * the bound in force on this camera. They cannot be the latter: this
	 * table is a compile-time constant and the real range belongs to the
	 * silicon and the loaded tuning -- brightness is a level in 0..100 on
	 * Infinity6C and a byte on Ingenic, and 3DNR is eight positions rather
	 * than 256. rvd publishes the true range per knob in get-isp's `caps`,
	 * enforces it in the HAL, and rcd forwards it to subscribers.
	 *
	 * So what these do is reject a value no platform could take. A value
	 * inside them but outside this camera's range is refused by rvd when
	 * the key is applied, with an error naming the knob.
	 */
	{"image", "brightness", V_INT, 0, 255, NULL, LIVE_ISP("set-brightness")},
	{"image", "contrast", V_INT, 0, 255, NULL, LIVE_ISP("set-contrast")},
	{"image", "saturation", V_INT, 0, 255, NULL, LIVE_ISP("set-saturation")},
	{"image", "sharpness", V_INT, 0, 255, NULL, LIVE_ISP("set-sharpness")},
	{"image", "hue", V_INT, 0, 255, NULL, LIVE_ISP("set-hue")},
	{"image", "sinter", V_INT, 0, 255, NULL, LIVE_ISP("set-sinter")},
	{"image", "temper", V_INT, 0, 255, NULL, LIVE_ISP("set-temper")},
	/*
	 * Signed, and the one knob here that is: exposure compensation biases
	 * the AE target either way, and SigmaStar states it in EV steps around
	 * zero. The old 0-255 made the whole darker half unreachable through
	 * rcd -- rvd took -3 from the CLI and rcd refused it as out of range.
	 * The magnitude is Ingenic's, which takes a bare int with no bound the
	 * vendor documents, so this rejects the absurd and nothing else.
	 */
	{"image", "ae_comp", V_INT, -255, 255, NULL, LIVE_ISP("set-ae-comp")},
	/* The gain ceilings are policy this daemon owns rather than knobs a
	 * tuning expresses, so there is no curve to hand back and no auto. */
	{"image", "max_again", V_INT, 0, 160, NULL, LIVE("set-max-again")},
	{"image", "max_dgain", V_INT, 0, 160, NULL, LIVE("set-max-dgain")},
	{"image", "dpc_strength", V_INT, 0, 255, NULL, LIVE_ISP("set-dpc")},
	{"image", "drc_strength", V_INT, 0, 255, NULL, LIVE_ISP("set-drc")},
	{"image", "defog_strength", V_INT, 0, 255, NULL, LIVE_ISP("set-defog-strength")},
	{"image", "highlight_depress", V_INT, 0, 255, NULL, LIVE_ISP("set-highlight-depress")},
	{"image", "backlight_comp", V_INT, 0, 10, NULL, LIVE_ISP("set-backlight-comp")},
	/* Orientation is a channel property, not an image one: there is no
	 * "either way" for the sensor to be reading out. */
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

	/*
	 * -- The six places on the picture. --
	 *
	 * rod's overlay is a list of elements, each named by whoever wrote the
	 * config and placed by a `position` line -- which is the right model
	 * for what it can draw and the wrong one for a table of keys fixed at
	 * compile time, because nothing here can name a section a person has
	 * not written yet. So rod also reads a section named for a place as
	 * being in that place, and these are those six names. An element named
	 * anything else is untouched by this and simply not reachable from
	 * here; it is not overwritten, hidden, or moved.
	 *
	 * `position` is deliberately absent. It is the section name, and a key
	 * that could disagree with it would let a slot be dragged out of the
	 * slot it is.
	 *
	 * A template is the one value in this table that a person composes
	 * rather than chooses, so it is the widest grammar here -- V_TEXT, the
	 * type the wifi SSID uses. It is not a path, a format or a command:
	 * rod expands its own %var% names into a bitmap and passes it to
	 * nothing. The 71 is what one value in this protocol holds, and rod's
	 * own limit is longer -- a template past it is still settable by hand
	 * and through rod's `set-element`.
	 *
	 * Spelled out six times rather than expanded from a macro, because a
	 * macro here formats badly enough to be worth avoiding and the six are
	 * meant to be greppable by section name. What keeps them identical is
	 * a test that walks them, which a macro could not have caught anyway:
	 * the drift that matters is one slot gaining a key the others lack.
	 *
	 * What a corner is, and not how it is drawn: the type is the overlay's
	 * and is set once in [osd]. rod reads a per-element `font_size` and
	 * `max_chars` and honours both, and they stay hand-edited -- six copies
	 * of a size that is meant to match is a way to end up with six sizes
	 * that do not.
	 */
	{"osd.top_left", "template", V_TEXT, 0, 71, NULL, SAVED},
	{"osd.top_left", "visible", V_BOOL, 0, 0, NULL, SAVED},
	{"osd.top_left", "align", V_ENUM, 0, 0, choices_align, SAVED},
	{"osd.top_center", "template", V_TEXT, 0, 71, NULL, SAVED},
	{"osd.top_center", "visible", V_BOOL, 0, 0, NULL, SAVED},
	{"osd.top_center", "align", V_ENUM, 0, 0, choices_align, SAVED},
	{"osd.top_right", "template", V_TEXT, 0, 71, NULL, SAVED},
	{"osd.top_right", "visible", V_BOOL, 0, 0, NULL, SAVED},
	{"osd.top_right", "align", V_ENUM, 0, 0, choices_align, SAVED},
	{"osd.bottom_left", "template", V_TEXT, 0, 71, NULL, SAVED},
	{"osd.bottom_left", "visible", V_BOOL, 0, 0, NULL, SAVED},
	{"osd.bottom_left", "align", V_ENUM, 0, 0, choices_align, SAVED},
	{"osd.bottom_center", "template", V_TEXT, 0, 71, NULL, SAVED},
	{"osd.bottom_center", "visible", V_BOOL, 0, 0, NULL, SAVED},
	{"osd.bottom_center", "align", V_ENUM, 0, 0, choices_align, SAVED},
	{"osd.bottom_right", "template", V_TEXT, 0, 71, NULL, SAVED},
	{"osd.bottom_right", "visible", V_BOOL, 0, 0, NULL, SAVED},
	{"osd.bottom_right", "align", V_ENUM, 0, 0, choices_align, SAVED},

	/* -- Day/night: how the board is wired, and what nightfall is. -- */
	{"ircut", "enabled", V_BOOL, 0, 0, NULL, SAVED},
	{"ircut", "trigger", V_ENUM, 0, 0, choices_trigger, SAVED},
	{"ircut", "pulse_ms", V_INT, 1, 1000, NULL, SAVED},
	/*
	 * What the default trigger calls night, and what it calls morning.
	 *
	 * Live, because ric takes each one through `set-threshold` and records
	 * it in its own config -- the same route `ircut-threshold` takes, and
	 * the reason these carry a selector rather than a command of their
	 * own. Tuning them is a feedback loop: the value is judged by whether
	 * the camera switches at the right moment, which a restart between
	 * attempts makes slow to see.
	 *
	 * The ranges are ric's own, from its set-threshold handler. All three
	 * belong to this trigger and no other: the day-verify latch that
	 * re-reads `night_luma` is armed only on a luma day transition, and
	 * the other triggers reach their own verdict from their own readings.
	 *
	 * `night_luma` is a level in 0-255 the platform's AE reports, and what
	 * counts as dark differs by how the AE derives it: Ingenic Gen3 reads
	 * a linear histogram and lands far below Gen1/2's for the same scene.
	 * Calibrate against the camera in hand, never against another model's
	 * number.
	 */
	{"ircut", "night_luma", V_INT, 0, 255, NULL, LIVE_SEL("set-threshold", "night_luma")},
	{"ircut", "night_gain", V_INT, 0, 1000000, NULL, LIVE_SEL("set-threshold", "night_gain")},
	{"ircut", "day_gain_pct", V_INT, 1, 100, NULL, LIVE_SEL("set-threshold", "day_gain_pct")},
	/*
	 * The same question for the legacy gain trigger, which asks it in
	 * absolute gain both ways instead of a ratio one way. Absolute means
	 * sensor-specific: these have to be read off the camera in hand, and
	 * they have to be read again if the night sensor rate changes, because
	 * a lower rate raises the exposure ceiling and the AE answers with
	 * less gain for the same scene.
	 */
	{"ircut", "night_threshold", V_INT, 0, 1000000, NULL,
	 LIVE_SEL("set-threshold", "night_threshold")},
	{"ircut", "day_threshold", V_INT, 0, 1000000, NULL,
	 LIVE_SEL("set-threshold", "day_threshold")},
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
	/*
	 * ...and which way each one is wired. A pin number says which line to
	 * drive and nothing about the sense of it, so a pin is only half an
	 * answer: boards exist for either polarity, and ric has honoured these
	 * flags all along -- they were simply unreachable from here, which left
	 * an inverted board with no route but hand-editing raptor.conf.
	 *
	 * false is the plain form: the IR-cut pin is driven high for day, and
	 * an LED bank lights on 1.
	 *
	 * There is no gpio_ircut2_active_low, and that is not an omission. A
	 * two-pin filter is a motor across an H-bridge, where the polarity IS
	 * the order of the two pins; ric consults the flag below only on the
	 * single-pin path. Setting it on an H-bridge board changes nothing --
	 * swap the pins instead.
	 *
	 * /etc/thingino.json carries the same information, as {"pin": N,
	 * "active_low": true}, and is absent on an OpenIPC base for the same
	 * reason the pins above are here.
	 */
	{"ircut", "gpio_ircut_active_low", V_BOOL, 0, 0, NULL, SAVED},
	{"ircut", "gpio_irled_active_low", V_BOOL, 0, 0, NULL, SAVED},
	{"ircut", "gpio_irled2_active_low", V_BOOL, 0, 0, NULL, SAVED},
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
	/*
	 * How often the light is looked at, and how long a verdict has to hold
	 * before it is acted on. Neither belongs to a trigger -- every one of
	 * them is sampled on this interval and debounced by this count.
	 *
	 * Live for the same reason the thresholds are: a camera that flips at
	 * dusk is diagnosed by widening the hold and watching, which wants the
	 * next poll rather than the next start.
	 */
	{"ircut", "hysteresis_sec", V_INT, 1, 300, NULL,
	 LIVE_SEL("set-threshold", "hysteresis_sec")},
	{"ircut", "poll_interval_ms", V_INT, 50, 10000, NULL,
	 LIVE_SEL("set-threshold", "poll_interval_ms")},

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
	{"recording", "stream", V_INT, 0, 1, labels_recstream, SAVED},
	{"recording", "audio", V_BOOL, 0, 0, NULL, SAVED},
	{"recording", "segment_minutes", V_INT, 1, 1440, NULL, SAVED},
	{"recording", "max_storage_mb", V_INT, 0, 1000000, NULL, SAVED},
	{"recording", "prebuffer_sec", V_INT, 0, 5, NULL, SAVED},

	/* -- Timelapse -- */
	{"timelapse", "enabled", V_BOOL, 0, 0, NULL, SAVED},
	{"timelapse", "interval", V_INT, 2, 86400, NULL, SAVED},
	{"timelapse", "playback_fps", V_INT, 1, 120, NULL, SAVED},
	{"timelapse", "max_mb", V_INT, 0, 1000000, NULL, SAVED},

	/*
	 * -- The camera itself. No daemon owns these and no line of
	 *    raptor.conf holds them: each names a provider that reads and
	 *    writes the file in /etc that does. See rcd_system.h.
	 *
	 *    The timezone is the one key in the table that costs a reboot. TZ
	 *    is exported once by rcS and a raptor daemon restarts by re-execing
	 *    itself, keeping the environment it already had -- so nothing short
	 *    of a reboot moves the clock a running daemon renders with, and
	 *    saying anything else here would be a promise the camera cannot
	 *    keep. --
	 */
	{"device", "timezone", V_ENUM, 0, 0, rcd_zone_names,
	 PROVIDED(rcd_provider_timezone, RCD_IMPACT_REBOOT)},
	{"device", "ntp_server", V_HOST, 0, 63, NULL,
	 PROVIDED(rcd_provider_ntp_server, RCD_IMPACT_NONE)},

	/*
	 * The name, and the first key that can cost a client its way back.
	 * It is in force immediately -- the kernel is renamed, /etc/hosts
	 * follows, and mdnsd re-announces -- so a console that reached this
	 * camera at <name>.local is looking at the wrong name the moment the
	 * reply arrives, and gets it back by not confirming.
	 *
	 * That is also why this key is guarded first: the failure is real
	 * enough to exercise the machinery and mild enough that a bug in it
	 * costs a name rather than an address. Nothing that can actually
	 * strand a camera is guarded until this one has been.
	 */
	{"device", "hostname", V_HOST, 1, 63, NULL,
	 GUARDED(rcd_provider_hostname, RCD_IMPACT_SERVICE, RCD_GUARD_NAME_SEC)},

	/*
	 * -- The camera's address. One interface, the camera's -- see
	 *    rcd_network.h -- and five directives of one file, so they are
	 *    declared together and enacted together: `apply` brings the
	 *    interface down and back up once, whichever of them changed.
	 *
	 *    Every one of them is guarded, because every one of them can be
	 *    wrong in a way that ends the conversation. This is the section
	 *    the guard was built for; the hostname above was the rehearsal. --
	 */
	{"network", "dhcp", V_BOOL, 0, 0, NULL,
	 GUARDED(rcd_provider_net_dhcp, RCD_IMPACT_NETWORK, RCD_GUARD_NET_SEC)},
	{"network", "address", V_IPV4, 1, 0, NULL,
	 GUARDED(rcd_provider_net_address, RCD_IMPACT_NETWORK, RCD_GUARD_NET_SEC)},
	{"network", "netmask", V_IPV4, 1, 0, NULL,
	 GUARDED(rcd_provider_net_netmask, RCD_IMPACT_NETWORK, RCD_GUARD_NET_SEC)},
	/* A camera on a flat network has no gateway and no name server, so
	 * both of these accept the empty string and the two above do not. */
	{"network", "gateway", V_IPV4, 0, 0, NULL,
	 GUARDED(rcd_provider_net_gateway, RCD_IMPACT_NETWORK, RCD_GUARD_NET_SEC)},
	{"network", "dns", V_IPV4, 0, 0, NULL,
	 GUARDED(rcd_provider_net_dns, RCD_IMPACT_NETWORK, RCD_GUARD_NET_SEC)},

	/*
	 * -- The network this camera joins, on the cameras that join one.
	 *    Two variables of the boot environment -- see rcd_wifi.h -- and
	 *    one enact behind both, because they are read together by the
	 *    generator that builds the supplicant's configuration.
	 *
	 *    Guarded like the wired address and for a stronger reason: on a
	 *    camera whose only interface is the radio, a wrong passphrase is
	 *    not a degraded camera but an unreachable one, and the window is
	 *    the only way back that does not involve a serial console.
	 *
	 *    The SSID caps at 32 because 802.11 does, not because the store
	 *    does. The passphrase carries no length here at all: WPA's own
	 *    rule is the grammar's, and what this entry says is only that an
	 *    open network -- no passphrase -- is one of the answers. --
	 */
	{"wifi", "ssid", V_TEXT, 1, 32, NULL,
	 GUARDED(rcd_provider_wifi_ssid, RCD_IMPACT_NETWORK, RCD_GUARD_WIFI_SEC)},
	{"wifi", "psk", V_SECRET, 0, 63, NULL,
	 GUARDED(rcd_provider_wifi_psk, RCD_IMPACT_NETWORK, RCD_GUARD_WIFI_SEC)},

	{NULL, NULL, V_INT, 0, 0, NULL, SAVED},
};

/*
 * The tier answers one question: does this key still owe something to `apply`?
 *
 * A live command does not -- the daemon has the value. Nor does a provider
 * with no way to enact: its store is read as it stands, so once it is written
 * rcd has nothing further to do. What the *running* system does with it is a
 * separate question, and the impact is where that is answered -- the timezone
 * is stored the moment it is set and read once, at boot, so it is live and
 * costs a reboot.
 *
 * A provider that *can* enact is the restart tier by the same test: the store
 * holds the value and the system does not, and closing that gap is what
 * `apply` is for. It is also the only honest answer for a key that costs the
 * operator their connection -- a setting that dangerous must be something
 * they press a button for, not something that happens as they leave the
 * field.
 */
bool rcd_key_live(const rcd_key_t *k)
{
	if (!k)
		return false;
	if (k->live_cmd)
		return true;
	/* A provider that can enact has separated storing the value from
	 * putting it in force, and the second half is owed to `apply` --
	 * which is what the restart tier means. One that cannot enact is a
	 * store the system reads as it stands, so `set` is the whole of it. */
	return k->provider && !k->provider->enact;
}

rcd_impact_t rcd_key_impact(const rcd_key_t *k)
{
	if (!k)
		return RCD_IMPACT_NONE;
	/* A declared impact is the answer whether or not the key is live: it
	 * is what taking effect costs, not what applying it costs. */
	if (k->impact != RCD_IMPACT_NONE)
		return k->impact;
	if (rcd_key_live(k))
		return RCD_IMPACT_NONE;
	return rcd_daemon_impact(rcd_section_owner(k->section));
}

bool rcd_key_resettable(const rcd_key_t *k)
{
	if (!k)
		return false;
	/* A key in raptor.conf is reset by taking its line out, which is
	 * always available. A provider answers for its own store. */
	return k->provider ? k->provider->resettable : true;
}

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
	{.name = "request-idr", .daemon = "rvd", .args = args_channel_opt},
	{.name = "set-qp-bounds", .daemon = "rvd", .args = args_qp_bounds, .persists = true},
	{.name = "set-rc-mode", .daemon = "rvd", .args = args_rc_mode, .persists = true},

	/* -- Day/night. The LED banks are a manual override of automatic
	 *    behaviour rather than a setting, which is why they persist
	 *    nothing: the next auto transition takes them back. -- */
	{.name = "ircut-mode",
	 .daemon = "ric",
	 .ctrl_cmd = "mode",
	 .args = args_daynight,
	 .persists = true},
	{.name = "ircut-threshold",
	 .daemon = "ric",
	 .ctrl_cmd = "set-threshold",
	 .args = args_threshold,
	 .persists = true},
	{.name = "ir850", .daemon = "ric", .args = args_on_off},
	{.name = "ir940", .daemon = "ric", .args = args_on_off},

	/* -- OSD. rod pauses rendering rather than writing [osd] enabled, so
	 *    this deliberately persists nothing: a reboot restores the
	 *    configured state, which is what rod already does. -- */
	{.name = "osd-enable", .daemon = "rod", .ctrl_cmd = "enable", .args = args_none},
	{.name = "osd-disable", .daemon = "rod", .ctrl_cmd = "disable", .args = args_none},

	/* -- Provisioning. The only action rcd performs itself, because the
	 *    boot environment is not a daemon's to be asked for. Reboot-tier
	 *    rather than live: the credentials are gone from the store the
	 *    moment this returns, and which mode the camera comes up in is a
	 *    decision the boot path makes. See rcd_wifi.h. -- */
	/* -- Setup. A scan is a read, so it costs nothing and is not refused
	 *    while the guard is holding a snapshot: the page that most wants
	 *    to rescan is the one waiting for a credential to be confirmed. -- */
	{.name = "wifi-scan", .args = args_none, .local = rcd_wifi_scan, .avail = rcd_wifi_present},

	{.name = "provision-reset",
	 .args = args_none,
	 .local = rcd_wifi_provision_reset,
	 .avail = rcd_wifi_present,
	 .impact = RCD_IMPACT_REBOOT,
	 .note = "the network is forgotten; the camera comes back in setup mode at its "
		 "next boot"},

	/* -- Restarting. rcd's own, like provisioning, and for the same reason:
	 *    no daemon owns it. Last in the table because it is the one entry
	 *    here that ends the conversation. -- */
	{.name = "reboot",
	 .args = args_none,
	 .local = rcd_system_reboot,
	 .impact = RCD_IMPACT_REBOOT,
	 .note = "the camera restarts, and every connection to it is dropped until it is "
		 "back"},

	{.name = NULL},
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
	case V_HOST:
		return "host";
	case V_IPV4:
		return "ipv4";
	case V_TEXT:
		return "text";
	case V_SECRET:
		return "secret";
	}
	return "int";
}

static void emit_choices(cJSON *o, const char *const *choices)
{
	cJSON *a = cJSON_AddArrayToObject(o, "choices");
	for (int i = 0; a && choices[i]; i++)
		cJSON_AddItemToArray(a, cJSON_CreateString(choices[i]));
}

static void emit_labels(cJSON *o, const char *const *labels)
{
	cJSON *a = cJSON_AddArrayToObject(o, "labels");
	for (int i = 0; a && labels[i]; i++)
		cJSON_AddItemToArray(a, cJSON_CreateString(labels[i]));
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
		/* Named values, when the number means nothing on its own.
		 * Additive: a client that ignores this still renders the key
		 * correctly, as a number within the range above. */
		if (k->choices)
			emit_labels(o, k->choices);
		/*
		 * And that the word "auto" is a value here too, which a client
		 * has no other way to learn: it is not a number in the range,
		 * and a form that only offers the range can never say "leave
		 * this to the tuning". Said only where it is true, so a client
		 * that has never heard of it draws what it always drew.
		 *
		 * This is the grammar. Whether this camera's knob has an auto
		 * mode at all is the silicon's answer and arrives with the
		 * rest of its range, in `state`'s per-knob caps.
		 */
		if (k->auto_ok)
			cJSON_AddBoolToObject(o, "auto", true);
	} else if (k->type == V_CRED || k->type == V_HOST || k->type == V_TEXT) {
		cJSON_AddNumberToObject(o, "max_length", k->max);
	} else if (k->type == V_SECRET) {
		/* Its length rule is WPA's -- 8 to 63, or 64 hex digits -- and
		 * belongs to the grammar rather than to this entry, so the
		 * only thing left to say is whether an open network is one of
		 * the answers. */
		cJSON_AddBoolToObject(o, "optional", k->min == 0);
	} else if (k->type == V_IPV4) {
		/* Whether the empty string is one of its values. A form that
		 * always submits every field needs to know which of them it
		 * may submit empty. */
		cJSON_AddBoolToObject(o, "optional", k->min == 0);
	} else if (k->type == V_ENUM) {
		emit_choices(o, k->choices);
	}

	/*
	 * Who re-reads this key. "system" for a provider-backed one: no daemon
	 * owns it, and a client that groups by owner needs a name it can group
	 * under rather than the "?" an absent daemon renders as -- one that
	 * cannot collide with a daemon it might otherwise try to restart.
	 *
	 * This is an owner, not a section, and the distinction is why the
	 * section holding the provider-backed keys is called [device]. The two
	 * shared the spelling until they were told apart: raptor.conf has a real
	 * [system] that rvd reads video_backend and video_device out of, so the
	 * virtual section shadowed it and `get section=system` answered with
	 * timezone and hostname while silently omitting what an operator meant.
	 * The owner keeps the name because nothing else claims it -- [network]'s
	 * keys report this same owner, and console.html reads it to decide that
	 * a change "changes the camera itself" and cannot be undone by
	 * restarting a daemon.
	 */
	cJSON_AddStringToObject(o, "owner",
				k->provider ? "system"
					    : rcd_daemon_name(rcd_section_owner(k->section)));

	/*
	 * The whole point of serving this table: a client knows before it
	 * submits whether a field costs nothing or costs an outage, and can
	 * say so next to the field rather than after the fact.
	 */
	cJSON_AddStringToObject(o, "tier", rcd_key_live(k) ? "live" : "restart");
	cJSON_AddStringToObject(o, "impact", rcd_impact_name(rcd_key_impact(k)));

	/* A credential is settable and never readable, and a client that does
	 * not know that draws an input which always looks empty and calls it a
	 * bug. Said plainly instead. A provider-backed key has no daemon to ask
	 * and is read anyway -- from the store it is written to. */
	if (k->type == V_CRED || k->type == V_SECRET ||
	    (!k->provider && !rcd_section_reader(k->section)))
		cJSON_AddBoolToObject(o, "readable", false);

	/* Present only where it applies, so a client that has never heard of
	 * the guard renders every other key exactly as it did before. */
	if (k->guard_sec > 0)
		cJSON_AddNumberToObject(o, "guard_sec", k->guard_sec);

	/* Said only when the answer is no, for the same reason: almost every
	 * key can be put back, and a client that has never heard of reset
	 * draws what it always drew. */
	if (!rcd_key_resettable(k))
		cJSON_AddBoolToObject(o, "resettable", false);

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
	} else if (a->local) {
		cJSON_AddStringToObject(o, "owner", "rcd");
		cJSON_AddStringToObject(o, "impact", rcd_impact_name(a->impact));

		/* Marked the way an unusable key is, and for the same reason:
		 * a client hides it rather than offering a button whose only
		 * outcome is the refusal. */
		if (a->avail && !a->avail())
			cJSON_AddBoolToObject(o, "available", false);
	}
	if (a->note)
		cJSON_AddStringToObject(o, "note", a->note);

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
