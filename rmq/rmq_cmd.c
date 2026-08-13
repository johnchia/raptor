/*
 * rmq_cmd.c -- Command policy and dispatch
 */

#include "rmq_cmd.h"
#include "rmq.h"

#include <rss_common.h>
#include <rss_ipc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ------------------------------------------------------------------ */
/* The allowlist                                                       */
/*                                                                     */
/* Live tier only: every command here takes effect on the running       */
/* camera. Anything needing a daemon restart waits for the restart      */
/* protocol, and nothing that stops a daemon appears at all.            */
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
		if (!cJSON_IsNumber(v)) {
			snprintf(err, errsz, "'%s' must be a number", a->key);
			return -1;
		}
		double d = cJSON_GetNumberValue(v);
		/* Range-checked as a double before narrowing. A value far
		 * outside int would otherwise wrap into range and pass. */
		if (!(d >= (double)a->min && d <= (double)a->max)) {
			snprintf(err, errsz, "'%s' out of range (%d-%d)", a->key, a->min, a->max);
			return -1;
		}
		if (d != (double)(long long)d) {
			snprintf(err, errsz, "'%s' must be a whole number", a->key);
			return -1;
		}
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

int rmq_cmd_plan(const char *json, rmq_cmd_plan_t *out, char *err, size_t errsz)
{
	memset(out, 0, sizeof(*out));

	cJSON *root = cJSON_Parse(json);
	if (!root) {
		snprintf(err, errsz, "payload is not JSON");
		return -1;
	}
	if (!cJSON_IsObject(root)) {
		snprintf(err, errsz, "payload is not a JSON object");
		cJSON_Delete(root);
		return -1;
	}

	const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
	if (!cJSON_IsString(cmd) || !cmd->valuestring) {
		snprintf(err, errsz, "missing 'cmd'");
		cJSON_Delete(root);
		return -1;
	}

	const cmd_def_t *def = find_command(cmd->valuestring);
	if (!def) {
		/* Deny by default. Naming nothing else keeps the refusal from
		 * doubling as a directory of what would have worked. */
		snprintf(err, errsz, "command not permitted");
		cJSON_Delete(root);
		return -1;
	}

	cJSON *req = cJSON_CreateObject();
	if (!req) {
		snprintf(err, errsz, "out of memory");
		cJSON_Delete(root);
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
			cJSON_Delete(root);
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
	cJSON_Delete(root);

	if (!fit || !out->daemon) {
		snprintf(err, errsz, "request could not be built");
		return -1;
	}
	return 0;
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

	/*
	 * The nonce is read from the raw payload rather than the plan, so a
	 * refused command still comes back tagged. Without that, a sender
	 * waiting on its own nonce cannot tell a rejection from a timeout.
	 */
	char nonce[NONCE_MAX + 1] = "";
	char cmd_name[64] = "";
	cJSON *root = cJSON_Parse(json);
	if (root) {
		const cJSON *n = cJSON_GetObjectItemCaseSensitive(root, "nonce");
		if (cJSON_IsString(n) && n->valuestring)
			rss_strlcpy(nonce, n->valuestring, sizeof(nonce));
		const cJSON *c = cJSON_GetObjectItemCaseSensitive(root, "cmd");
		if (cJSON_IsString(c) && c->valuestring)
			rss_strlcpy(cmd_name, c->valuestring, sizeof(cmd_name));
		cJSON_Delete(root);
	}

	rmq_cmd_plan_t plan;
	char err[192];
	if (rmq_cmd_plan(json, &plan, err, sizeof(err)) < 0) {
		/* Refusals are logged, not just answered: this topic is the
		 * camera's whole management surface, so a rejected command is
		 * the one thing worth being able to find afterwards. */
		RSS_WARN("cmd: refused '%s': %s", cmd_name[0] ? cmd_name : "(none)", err);
		publish_result(st, cmd_name[0] ? cmd_name : NULL, nonce[0] ? nonce : NULL, err,
			       NULL);
		free(json);
		return;
	}
	free(json);

	char sock[128];
	snprintf(sock, sizeof(sock), RSS_SOCK_FMT, plan.daemon);

	char *resp = malloc(RESP_MAX);
	if (!resp)
		return;

	int rc = rss_ctrl_send_command(sock, plan.request, resp, RESP_MAX, CTRL_TIMEOUT_MS);
	if (rc < 0) {
		free(resp);
		snprintf(err, sizeof(err), "%s is not running or did not answer", plan.daemon);
		RSS_WARN("cmd: %s", err);
		publish_result(st, cmd_name, nonce[0] ? nonce : NULL, err, NULL);
		return;
	}

	RSS_INFO("cmd: %s -> %s: %s", cmd_name, plan.daemon, plan.request);

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

	publish_result(st, cmd_name, nonce[0] ? nonce : NULL, fail, dresp);
}
