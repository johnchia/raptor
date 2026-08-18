/*
 * rcd_config.c -- see rcd_config.h
 */

#include "rcd_config.h"
#include "rcd.h"
#include "rcd_ipc.h"
#include "rcd_proto.h"
#include "rcd_schema.h"

#include <rss_common.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint64_t now_ms(void)
{
	return (uint64_t)(rss_timestamp_us() / 1000);
}

/* ------------------------------------------------------------------ */
/* Drift                                                               */
/* ------------------------------------------------------------------ */

void rcd_stale_add(rcd_state_t *st, const char *section, const char *key, rcd_daemon_t owner)
{
	if (owner < RCD_D_COUNT)
		st->stale_daemon[owner] = true;

	for (int i = 0; i < st->stale_count; i++) {
		if (strcmp(st->stale[i].section, section) == 0 &&
		    strcmp(st->stale[i].key, key) == 0)
			return;
	}

	/* Full. The daemon is already recorded as behind, which is the part
	 * that decides whether a restart is owed; losing the name of one more
	 * key costs a line of the report and nothing else. */
	if (st->stale_count >= RCD_STALE_MAX)
		return;

	rss_strlcpy(st->stale[st->stale_count].section, section, RCD_SECT_MAX);
	rss_strlcpy(st->stale[st->stale_count].key, key, RCD_KEY_MAX);
	st->stale_count++;
}

void rcd_stale_clear(rcd_state_t *st, rcd_daemon_t d)
{
	if (d >= RCD_D_COUNT)
		return;
	st->stale_daemon[d] = false;

	int kept = 0;
	for (int i = 0; i < st->stale_count; i++) {
		if (rcd_section_owner(st->stale[i].section) == d)
			continue;
		st->stale[kept++] = st->stale[i];
	}
	st->stale_count = kept;
}

void rcd_config_report_stale(const rcd_state_t *st, cJSON *out)
{
	cJSON *arr = cJSON_AddArrayToObject(out, "stale");
	if (!arr)
		return;

	for (int d = 0; d < RCD_D_COUNT; d++) {
		if (!st->stale_daemon[d])
			continue;

		cJSON *o = cJSON_CreateObject();
		if (!o)
			continue;
		cJSON_AddStringToObject(o, "daemon", rcd_daemon_name((rcd_daemon_t)d));
		cJSON_AddStringToObject(o, "impact",
					rcd_impact_name(rcd_daemon_impact((rcd_daemon_t)d)));

		cJSON *keys = cJSON_AddArrayToObject(o, "keys");
		for (int i = 0; keys && i < st->stale_count; i++) {
			if (rcd_section_owner(st->stale[i].section) != (rcd_daemon_t)d)
				continue;
			cJSON *k = cJSON_CreateObject();
			if (!k)
				continue;
			cJSON_AddStringToObject(k, "section", st->stale[i].section);
			cJSON_AddStringToObject(k, "key", st->stale[i].key);
			cJSON_AddItemToArray(keys, k);
		}
		cJSON_AddItemToArray(arr, o);
	}

	if (st->apply_error[0])
		cJSON_AddStringToObject(out, "apply_error", st->apply_error);
}

/* ------------------------------------------------------------------ */
/* The drift record, across an rcd restart                                        */
/*                                                                     */
/* /run is a tmpfs, which is exactly the lifetime wanted: an rcd that   */
/* is restarted must not forget that rvd is running behind, and a       */
/* camera that has rebooted has no drift left to remember, because      */
/* every daemon read the file on its way up.                            */
/* ------------------------------------------------------------------ */

void rcd_stale_load(rcd_state_t *st)
{
	int len = 0;
	char *text = rss_read_file(RCD_STALE_PATH, &len);
	if (!text)
		return;

	cJSON *root = cJSON_Parse(text);
	free(text);
	if (!root)
		return;

	const cJSON *item = NULL;
	cJSON_ArrayForEach(item, root)
	{
		const cJSON *s = cJSON_GetObjectItemCaseSensitive(item, "section");
		const cJSON *k = cJSON_GetObjectItemCaseSensitive(item, "key");
		if (!cJSON_IsString(s) || !cJSON_IsString(k))
			continue;
		/* Re-resolved through the table rather than trusted from the
		 * file: a key this build no longer has is dropped instead of
		 * pinning a restart nothing can satisfy. */
		if (!rcd_key_find(s->valuestring, k->valuestring))
			continue;
		rcd_stale_add(st, s->valuestring, k->valuestring,
			      rcd_section_owner(s->valuestring));
	}

	cJSON_Delete(root);

	if (st->stale_count)
		RSS_INFO("rcd: %d saved edit(s) still waiting for an apply", st->stale_count);
}

void rcd_stale_save(const rcd_state_t *st)
{
	if (st->stale_count == 0) {
		unlink(RCD_STALE_PATH);
		return;
	}

	cJSON *arr = cJSON_CreateArray();
	if (!arr)
		return;

	for (int i = 0; i < st->stale_count; i++) {
		cJSON *o = cJSON_CreateObject();
		if (!o)
			continue;
		cJSON_AddStringToObject(o, "section", st->stale[i].section);
		cJSON_AddStringToObject(o, "key", st->stale[i].key);
		cJSON_AddItemToArray(arr, o);
	}

	char *text = cJSON_PrintUnformatted(arr);
	cJSON_Delete(arr);
	if (!text)
		return;

	FILE *f = fopen(RCD_STALE_PATH, "w");
	if (f) {
		fputs(text, f);
		fclose(f);
	}
	free(text);
}

/* ------------------------------------------------------------------ */
/* Deferred saves                                                      */
/* ------------------------------------------------------------------ */

static void owe_save(rcd_state_t *st, rcd_daemon_t d)
{
	if (d >= RCD_D_COUNT)
		return;

	uint64_t now = now_ms();
	if (!st->save_due_ms)
		st->save_first_ms = now;

	st->save_owed[d] = true;
	st->save_due_ms = now + RCD_SAVE_DEBOUNCE_MS;

	/* A slider held down keeps pushing the deadline out, so cap the total
	 * wait: a change has to reach flash even if the burst never ends. */
	uint64_t ceiling = st->save_first_ms + RCD_SAVE_MAX_MS;
	if (st->save_due_ms > ceiling)
		st->save_due_ms = ceiling;
}

bool rcd_save_due(const rcd_state_t *st, uint64_t now)
{
	return st->save_due_ms && now >= st->save_due_ms;
}

void rcd_save_flush(rcd_state_t *st)
{
	if (!st->save_due_ms)
		return;

	for (int i = 0; i < RCD_D_COUNT; i++) {
		if (!st->save_owed[i])
			continue;
		st->save_owed[i] = false;

		/*
		 * Each daemon saves only the keys it dirtied -- rss_config_save
		 * edits them into the file in place rather than rewriting it --
		 * so several daemons saving the same file cannot revert each
		 * other, and a daemon with nothing dirty does not write.
		 */
		if (!rcd_ask_ok(rcd_daemon_name((rcd_daemon_t)i), "{\"cmd\":\"config-save\"}"))
			RSS_WARN("save: %s did not answer, its changes stay in memory",
				 rcd_daemon_name((rcd_daemon_t)i));
	}

	st->save_due_ms = 0;
	st->save_first_ms = 0;
}

/* ------------------------------------------------------------------ */
/* Validation                                                          */
/* ------------------------------------------------------------------ */

static void err_choices(char *err, size_t errsz, const char *key, const char *const *choices)
{
	/* Naming the alternatives is safe -- a closed enum is the whole point
	 * of the field -- and turns a rejection into something fixable. */
	int n = snprintf(err, errsz, "'%s' must be one of:", key);
	for (int i = 0; choices[i] && n > 0 && (size_t)n < errsz; i++)
		n += snprintf(err + n, errsz - (size_t)n, " %s", choices[i]);
}

/*
 * A labelled integer given by name. Refused with the same code and the same
 * list a V_ENUM would produce, so a client that does not distinguish the two
 * still gets an error it can act on.
 */
static const char *label_to_num(const rcd_key_t *k, const char *given, double *out, char *err,
				size_t errsz)
{
	for (int i = 0; k->choices[i]; i++) {
		if (strcmp(given, k->choices[i]) == 0) {
			*out = (double)(k->min + i);
			return NULL;
		}
	}
	err_choices(err, errsz, k->key, k->choices);
	return RCD_E_CHOICE;
}

/*
 * A whole JSON number within [min,max].
 *
 * The order matters: the range is checked while the value is still a double,
 * because a number far outside int wraps into range once narrowed and would
 * then pass. Narrowing is the caller's, after this returns.
 */
static const char *check_int(const cJSON *v, const char *key, int min, int max, double *out,
			     char *err, size_t errsz)
{
	if (!cJSON_IsNumber(v)) {
		snprintf(err, errsz, "'%s' must be a number", key);
		return RCD_E_TYPE;
	}

	double d = cJSON_GetNumberValue(v);
	if (!(d >= (double)min && d <= (double)max)) {
		snprintf(err, errsz, "'%s' out of range (%d-%d)", key, min, max);
		return RCD_E_RANGE;
	}
	if (d != (double)(long long)d) {
		snprintf(err, errsz, "'%s' must be a whole number", key);
		return RCD_E_TYPE;
	}

	*out = d;
	return NULL;
}

/* One validated edit: what the file will hold, and the same value in the form
 * a live command wants. */
typedef struct {
	const rcd_key_t *k;
	char rendered[RCD_VAL_MAX];
	double num;
	bool is_num;
} edit_t;

/*
 * Render one JSON value into the string the config file holds.
 *
 * Everything written comes from the table -- the enum's own spelling, or a
 * number this function formatted -- so no byte of the payload reaches the file
 * verbatim. The credential is the single exception and carries its own
 * grammar; see rcd_schema.h for why that is safe and why it does not
 * generalise.
 */
static const char *render(const rcd_key_t *k, const cJSON *v, rcd_edit_t *e, char *err,
			  size_t errsz)
{
	e->k = k;
	e->is_num = false;
	e->num = 0;

	if (!v || cJSON_IsNull(v)) {
		snprintf(err, errsz, "'%s' needs a value", k->key);
		return RCD_E_TYPE;
	}

	if (k->type == V_BOOL) {
		/* A JSON boolean, or the 0/1 a templating client is likelier
		 * to render. */
		bool on;
		if (cJSON_IsBool(v)) {
			on = cJSON_IsTrue(v);
		} else if (cJSON_IsNumber(v) &&
			   (cJSON_GetNumberValue(v) == 0 || cJSON_GetNumberValue(v) == 1)) {
			on = cJSON_GetNumberValue(v) != 0;
		} else {
			snprintf(err, errsz, "'%s' must be true or false", k->key);
			return RCD_E_TYPE;
		}
		snprintf(e->rendered, sizeof(e->rendered), "%s", on ? "true" : "false");
		e->num = on ? 1 : 0;
		e->is_num = true;
		return NULL;
	}

	if (k->type == V_INT) {
		double d;
		const char *code;
		/* A labelled integer accepts the label as well as the number:
		 * the number is what gets written either way. */
		if (k->choices && cJSON_IsString(v) && v->valuestring)
			code = label_to_num(k, v->valuestring, &d, err, errsz);
		else
			code = check_int(v, k->key, k->min, k->max, &d, err, errsz);
		if (code)
			return code;
		snprintf(e->rendered, sizeof(e->rendered), "%d", (int)d);
		e->num = d;
		e->is_num = true;
		return NULL;
	}

	if (k->type == V_CRED) {
		if (!cJSON_IsString(v) || !v->valuestring) {
			snprintf(err, errsz, "'%s' must be a string", k->key);
			return RCD_E_TYPE;
		}
		const char *s = v->valuestring;
		size_t n = strlen(s);
		if (n > (size_t)k->max || n >= sizeof(e->rendered)) {
			snprintf(err, errsz, "'%s' is longer than %d characters", k->key, k->max);
			return RCD_E_RANGE;
		}
		for (size_t i = 0; i < n; i++) {
			unsigned char c = (unsigned char)s[i];
			if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
				continue;
			/* Naming the permitted set rather than the offending
			 * byte: the value is a credential and must not be
			 * quoted back over the wire, not even one character. */
			snprintf(err, errsz,
				 "'%s' may contain only letters, digits, '-', '_', '.' and '~'",
				 k->key);
			return RCD_E_CHOICE;
		}
		memcpy(e->rendered, s, n);
		e->rendered[n] = '\0';
		return NULL;
	}

	/* V_ENUM. A number is accepted as well as a string so that the numeric
	 * enums -- sample rate is the one -- still match when a client renders
	 * 16000 rather than "16000". */
	char given[RCD_VAL_MAX];
	if (cJSON_IsString(v) && v->valuestring) {
		rss_strlcpy(given, v->valuestring, sizeof(given));
	} else if (cJSON_IsNumber(v) &&
		   cJSON_GetNumberValue(v) == (double)(long long)cJSON_GetNumberValue(v)) {
		snprintf(given, sizeof(given), "%lld", (long long)cJSON_GetNumberValue(v));
	} else {
		snprintf(err, errsz, "'%s' must be a string", k->key);
		return RCD_E_TYPE;
	}

	for (int i = 0; k->choices[i]; i++) {
		if (strcmp(given, k->choices[i]) == 0) {
			rss_strlcpy(e->rendered, k->choices[i], sizeof(e->rendered));
			return NULL;
		}
	}
	err_choices(err, errsz, k->key, k->choices);
	return RCD_E_CHOICE;
}

/* ------------------------------------------------------------------ */
/* get                                                                 */
/* ------------------------------------------------------------------ */

/*
 * The value a key currently has, typed as the schema says it is.
 *
 * Asked of the owning daemon first and read from the file only when it does
 * not answer: a live key changed a moment ago is in the daemon and not yet in
 * the file, and the daemon's answer is the one that describes the camera.
 */
static cJSON *typed_value(const rcd_key_t *k, const char *raw)
{
	if (!raw)
		return NULL;

	if (k->type == V_BOOL)
		return cJSON_CreateBool(strcmp(raw, "true") == 0 || strcmp(raw, "1") == 0 ||
					strcmp(raw, "yes") == 0 || strcmp(raw, "on") == 0);
	if (k->type == V_INT)
		return cJSON_CreateNumber(strtol(raw, NULL, 10));
	return cJSON_CreateString(raw);
}

static void emit_value(cJSON *arr, const rcd_key_t *k, rss_config_t *file, const cJSON *from_daemon)
{
	/* A credential is settable and never readable. Reporting the key with
	 * no value is the honest rendering: the client draws the input and
	 * knows not to expect it to fill in. */
	if (k->type == V_CRED) {
		cJSON *o = cJSON_CreateObject();
		if (!o)
			return;
		cJSON_AddStringToObject(o, "section", k->section);
		cJSON_AddStringToObject(o, "key", k->key);
		cJSON_AddBoolToObject(o, "readable", false);
		cJSON_AddItemToArray(arr, o);
		return;
	}

	const char *raw = NULL;
	const char *source = "daemon";

	if (from_daemon) {
		const cJSON *v = cJSON_GetObjectItemCaseSensitive(from_daemon, k->key);
		if (cJSON_IsString(v))
			raw = v->valuestring;
	}
	if (!raw && file) {
		raw = rss_config_get_str(file, k->section, k->key, NULL);
		source = "file";
	}
	cJSON *o = cJSON_CreateObject();
	if (!o)
		return;
	cJSON_AddStringToObject(o, "section", k->section);
	cJSON_AddStringToObject(o, "key", k->key);

	/*
	 * A key nobody has ever set is reported as unset rather than omitted.
	 * Silence would be indistinguishable from a key this build does not
	 * have, and the two call for different things from a client: one draws
	 * an empty field, the other draws nothing at all.
	 *
	 * There is no default to offer in its place. Each daemon supplies its
	 * own when it reads the file, and rcd does not link them.
	 */
	if (!raw) {
		cJSON_AddBoolToObject(o, "set", false);
		cJSON_AddItemToArray(arr, o);
		return;
	}

	cJSON *val = typed_value(k, raw);
	if (!val) {
		cJSON_Delete(o);
		return;
	}
	cJSON_AddItemToObject(o, "value", val);
	cJSON_AddStringToObject(o, "source", source);
	cJSON_AddItemToArray(arr, o);
}

/* One round trip per section rather than per key: a page asks for a whole
 * section at a time, and `config-get` costs a connection each. */
static cJSON *section_from_daemon(const char *section)
{
	const char *daemon = rcd_section_reader(section);
	if (!daemon)
		return NULL;

	char req[128];
	snprintf(req, sizeof(req), "{\"cmd\":\"config-get-section\",\"section\":\"%s\"}", section);

	cJSON *resp = rcd_ask_json(daemon, req);
	if (!resp)
		return NULL;

	cJSON *keys = cJSON_DetachItemFromObjectCaseSensitive(resp, "keys");
	cJSON_Delete(resp);
	return keys;
}

cJSON *rcd_cmd_get(rcd_state_t *st, const cJSON *root)
{
	const cJSON *sec = cJSON_GetObjectItemCaseSensitive(root, "section");
	const cJSON *keys = cJSON_GetObjectItemCaseSensitive(root, "keys");

	if (!cJSON_IsString(sec) && !cJSON_IsArray(keys))
		return rcd_err(RCD_E_MALFORMED, "get needs a 'section' or a 'keys' array");

	cJSON *resp = rcd_ok();
	if (!resp)
		return NULL;
	cJSON *out = cJSON_AddArrayToObject(resp, "values");
	if (!out)
		return resp;

	rss_config_t *file = rss_config_load(st->config_path);

	if (cJSON_IsString(sec) && sec->valuestring) {
		/* Walk the table rather than the daemon's reply: what a client
		 * may act on is what the table names, and a raw section dump
		 * would otherwise carry keys nothing here can validate. */
		cJSON *live = section_from_daemon(sec->valuestring);
		int found = 0;
		for (int i = 0;; i++) {
			const rcd_key_t *k = rcd_key_at(i);
			if (!k)
				break;
			if (strcmp(k->section, sec->valuestring) != 0)
				continue;
			emit_value(out, k, file, live);
			found++;
		}
		cJSON_Delete(live);

		if (!found) {
			cJSON_Delete(resp);
			rss_config_free(file);
			cJSON *e = rcd_err(RCD_E_UNKNOWN, "no such section");
			rcd_err_where(e, sec->valuestring, NULL);
			return e;
		}
	}

	const cJSON *ke = NULL;
	cJSON_ArrayForEach(ke, keys)
	{
		const cJSON *s = cJSON_GetObjectItemCaseSensitive(ke, "section");
		const cJSON *n = cJSON_GetObjectItemCaseSensitive(ke, "key");
		if (!cJSON_IsString(s) || !cJSON_IsString(n))
			continue;

		const rcd_key_t *k = rcd_key_find(s->valuestring, n->valuestring);
		if (!k)
			continue;

		cJSON *live = section_from_daemon(k->section);
		emit_value(out, k, file, live);
		cJSON_Delete(live);
	}

	rss_config_free(file);
	return resp;
}

/* ------------------------------------------------------------------ */
/* set                                                                 */
/* ------------------------------------------------------------------ */

/* Build the live request for one edit, entirely from the table. */
static bool live_request(const rcd_edit_t *e, char *out, size_t outsz)
{
	cJSON *req = cJSON_CreateObject();
	if (!req)
		return false;

	cJSON_AddStringToObject(req, "cmd", e->k->live_cmd);
	if (e->k->live_chn >= 0)
		cJSON_AddNumberToObject(req, "channel", e->k->live_chn);
	if (e->is_num)
		cJSON_AddNumberToObject(req, e->k->live_arg, e->num);
	else
		cJSON_AddStringToObject(req, e->k->live_arg, e->rendered);

	bool fit = cJSON_PrintPreallocated(req, out, (int)outsz, 0);
	cJSON_Delete(req);
	return fit;
}

/* Write every file-bound edit in one pass.
 *
 * A fresh load rather than a cached copy: rss_config_save writes only the keys
 * dirtied on the config handed to it, so starting from a clean read means
 * exactly these edits reach the file -- not anything a daemon has changed in
 * its own memory and not yet saved. */
static int write_file(rcd_state_t *st, rcd_edit_t *edits, const bool *to_file, int n)
{
	int wanted = 0;
	for (int i = 0; i < n; i++)
		wanted += to_file[i] ? 1 : 0;

	/* Every edit went to a running daemon, which will save its own. Loading
	 * the file to write nothing to it would still cost the read. */
	if (wanted == 0)
		return 0;

	rss_config_t *cfg = rss_config_load(st->config_path);
	if (!cfg)
		return -1;

	for (int i = 0; i < n; i++) {
		if (!to_file[i])
			continue;
		rss_config_set_str(cfg, edits[i].k->section, edits[i].k->key, edits[i].rendered);

		/* The log records what changed, not what it changed to -- for
		 * a password those are different things, and this file is
		 * readable by anyone who can read the flash. */
		bool secret =
			edits[i].k->type == V_CRED && strcmp(edits[i].k->key, "password") == 0;
		RSS_INFO("set: [%s] %s = %s", edits[i].k->section, edits[i].k->key,
			 secret ? "(set)" : edits[i].rendered);
	}

	int ret = rss_config_save(cfg, st->config_path);
	rss_config_free(cfg);
	return ret;
}

/*
 * Collect the edits a request carries.
 *
 * Two shapes, one path: an `edits` array, or the single section/key/value form
 * a one-field client finds easier to build. The array is all-or-nothing --
 * nothing is applied unless every edit validates -- because a half-applied
 * form is a configuration nobody chose.
 */
cJSON *rcd_set_validate(const cJSON *root, rcd_edit_t *edits, int *count)
{
	const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "edits");
	const cJSON *one_sec = cJSON_GetObjectItemCaseSensitive(root, "section");

	*count = 0;

	if (!cJSON_IsArray(arr)) {
		if (!cJSON_IsString(one_sec))
			return rcd_err(RCD_E_MALFORMED, "set needs an 'edits' array");

		const cJSON *kn = cJSON_GetObjectItemCaseSensitive(root, "key");
		if (!cJSON_IsString(kn))
			return rcd_err(RCD_E_MALFORMED, "set needs a 'key'");

		const rcd_key_t *k = rcd_key_find(one_sec->valuestring, kn->valuestring);
		if (!k) {
			cJSON *e = rcd_err(RCD_E_UNKNOWN, "not a writable key");
			rcd_err_where(e, one_sec->valuestring, kn->valuestring);
			return e;
		}

		char err[192];
		const char *code = render(k, cJSON_GetObjectItemCaseSensitive(root, "value"),
					  &edits[0], err, sizeof(err));
		if (code) {
			cJSON *e = rcd_err(code, err);
			rcd_err_where(e, k->section, k->key);
			return e;
		}
		*count = 1;
		return NULL;
	}

	const cJSON *item = NULL;
	cJSON_ArrayForEach(item, arr)
	{
		if (*count >= RCD_EDITS_MAX)
			return rcd_err(RCD_E_TOOMANY, "too many edits in one request");

		const cJSON *s = cJSON_GetObjectItemCaseSensitive(item, "section");
		const cJSON *n = cJSON_GetObjectItemCaseSensitive(item, "key");
		if (!cJSON_IsString(s) || !cJSON_IsString(n))
			return rcd_err(RCD_E_MALFORMED, "each edit needs a 'section' and a 'key'");

		const rcd_key_t *k = rcd_key_find(s->valuestring, n->valuestring);
		if (!k) {
			/* A key that exists but holds a path, and one that was
			 * never a key at all, read the same from out here. */
			cJSON *e = rcd_err(RCD_E_UNKNOWN, "not a writable key");
			rcd_err_where(e, s->valuestring, n->valuestring);
			return e;
		}

		char err[192];
		const char *code = render(k, cJSON_GetObjectItemCaseSensitive(item, "value"),
					  &edits[*count], err, sizeof(err));
		if (code) {
			cJSON *e = rcd_err(code, err);
			rcd_err_where(e, k->section, k->key);
			return e;
		}
		(*count)++;
	}

	if (*count == 0)
		return rcd_err(RCD_E_MALFORMED, "'edits' names nothing");
	return NULL;
}

cJSON *rcd_cmd_set(rcd_state_t *st, const cJSON *root)
{
	rcd_edit_t edits[RCD_EDITS_MAX];
	int n = 0;

	cJSON *refusal = rcd_set_validate(root, edits, &n);
	if (refusal)
		return refusal;

	/*
	 * Whether a live key should also be applied now. It is by default: a
	 * slider that needs a second round trip to show anything is not a
	 * slider. A client filling a whole form at once can ask to stage
	 * instead, so its fields land together rather than one at a time.
	 */
	const cJSON *stage = cJSON_GetObjectItemCaseSensitive(root, "stage");
	bool stage_only = cJSON_IsTrue(stage);

	cJSON *resp = rcd_ok();
	if (!resp)
		return NULL;
	cJSON *results = cJSON_AddArrayToObject(resp, "results");

	bool to_file[RCD_EDITS_MAX] = {false};
	const char *note[RCD_EDITS_MAX] = {NULL};
	char notebuf[RCD_EDITS_MAX][192];

	for (int i = 0; i < n; i++) {
		const rcd_key_t *k = edits[i].k;

		if (!k->live_cmd || stage_only) {
			to_file[i] = true;
			continue;
		}

		char req[RCD_REQ_MAX];
		if (!live_request(&edits[i], req, sizeof(req))) {
			to_file[i] = true;
			note[i] = "the live command could not be built";
			continue;
		}

		const char *daemon = rcd_daemon_name(rcd_section_owner(k->section));
		if (rcd_ask_ok_err(daemon, req, notebuf[i], sizeof(notebuf[i]))) {
			/* The daemon holds the new value and records it in its
			 * own config, so it -- not rcd -- writes the file. */
			owe_save(st, rcd_section_owner(k->section));
			continue;
		}

		/*
		 * The live command was refused. That is the ordinary case for
		 * three [image] keys on SigmaStar, where orientation and the
		 * 3DNR level are fixed when the ISP channel is created, and it
		 * is also what a stopped daemon looks like. Either way the
		 * value belongs in the file, and the reply says so rather than
		 * reporting a success the camera did not have.
		 */
		to_file[i] = true;
		note[i] = notebuf[i];
	}

	if (write_file(st, edits, to_file, n) != 0) {
		cJSON_Delete(resp);
		return rcd_err(RCD_E_IO, "the config file could not be written");
	}

	int saved = 0;
	for (int i = 0; i < n; i++) {
		if (!to_file[i])
			continue;
		rcd_stale_add(st, edits[i].k->section, edits[i].k->key,
			      rcd_section_owner(edits[i].k->section));
		saved++;
	}
	if (saved)
		rcd_stale_save(st);

	for (int i = 0; i < n && results; i++) {
		cJSON *o = cJSON_CreateObject();
		if (!o)
			continue;
		cJSON_AddStringToObject(o, "section", edits[i].k->section);
		cJSON_AddStringToObject(o, "key", edits[i].k->key);

		/* Echoed in the type the schema declares, not as the string the
		 * file happens to hold: a client that sent a number and reads
		 * back "140" has to know which of its fields to un-quote. A
		 * credential is echoed as nothing at all. */
		if (edits[i].k->type != V_CRED) {
			cJSON *v = typed_value(edits[i].k, edits[i].rendered);
			if (v)
				cJSON_AddItemToObject(o, "value", v);
		}
		cJSON_AddStringToObject(o, "applied", to_file[i] ? "saved" : "live");
		if (note[i] && note[i][0])
			cJSON_AddStringToObject(o, "note", note[i]);
		cJSON_AddItemToArray(results, o);
	}

	/* What is now owed. Reported on every set, so a client never has to
	 * ask a second question to know whether it just cost an outage. */
	rcd_config_report_stale(st, resp);
	return resp;
}

cJSON *rcd_cmd_credentials(rcd_state_t *st, const cJSON *root)
{
	static const char *const fields[] = {"username", "password", NULL};
	static const char *const sections[] = {"rtsp", "http", NULL};

	cJSON *edits = cJSON_CreateArray();
	if (!edits)
		return NULL;

	for (int f = 0; fields[f]; f++) {
		const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, fields[f]);
		if (!v || cJSON_IsNull(v))
			continue;
		for (int s = 0; sections[s]; s++) {
			cJSON *e = cJSON_CreateObject();
			if (!e)
				continue;
			cJSON_AddStringToObject(e, "section", sections[s]);
			cJSON_AddStringToObject(e, "key", fields[f]);
			cJSON_AddItemToObject(e, "value", cJSON_Duplicate(v, true));
			cJSON_AddItemToArray(edits, e);
		}
	}

	if (cJSON_GetArraySize(edits) == 0) {
		cJSON_Delete(edits);
		return rcd_err(RCD_E_MALFORMED, "credentials needs a 'username' or a 'password'");
	}

	/*
	 * Both daemons authenticate only when both fields are set, so clearing
	 * one turns authentication off on both endpoints together -- which is
	 * the point of setting them together, and why an empty value is
	 * accepted by the table.
	 */
	cJSON *req = cJSON_CreateObject();
	if (!req) {
		cJSON_Delete(edits);
		return NULL;
	}
	cJSON_AddItemToObject(req, "edits", edits);

	cJSON *resp = rcd_cmd_set(st, req);
	cJSON_Delete(req);
	return resp;
}

/* ------------------------------------------------------------------ */
/* action                                                              */
/* ------------------------------------------------------------------ */

static const char *add_arg(cJSON *req, const cJSON *in, const rcd_arg_t *a, char *err, size_t errsz)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(in, a->key);

	if (!v || cJSON_IsNull(v)) {
		if (a->required) {
			snprintf(err, errsz, "missing required field '%s'", a->key);
			return RCD_E_MALFORMED;
		}
		return NULL;
	}

	if (a->type == A_INT) {
		double d;
		const char *code = check_int(v, a->key, a->min, a->max, &d, err, errsz);
		if (code)
			return code;
		cJSON_AddNumberToObject(req, a->key, d);
		return NULL;
	}

	if (!cJSON_IsString(v) || !v->valuestring) {
		snprintf(err, errsz, "'%s' must be a string", a->key);
		return RCD_E_TYPE;
	}

	if (a->type == A_SECTION) {
		const char *daemon = rcd_section_reader(v->valuestring);
		if (!daemon) {
			snprintf(err, errsz, "section is not readable");
			return RCD_E_UNKNOWN;
		}
		cJSON_AddStringToObject(req, a->key, v->valuestring);
		return NULL;
	}

	for (int i = 0; a->choices[i]; i++) {
		if (strcmp(v->valuestring, a->choices[i]) == 0) {
			/* The table's copy, not the caller's. The two compare
			 * equal, so this changes nothing about the request --
			 * it just leaves no path by which input bytes reach a
			 * daemon. */
			cJSON_AddStringToObject(req, a->key, a->choices[i]);
			return NULL;
		}
	}

	err_choices(err, errsz, a->key, a->choices);
	return RCD_E_CHOICE;
}

cJSON *rcd_action_validate(const cJSON *root, char *wire, size_t wiresz, const char **owner)
{
	const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "action");
	if (!cJSON_IsString(name) || !name->valuestring)
		return rcd_err(RCD_E_MALFORMED, "action needs an 'action'");

	const rcd_action_t *a = rcd_action_find(name->valuestring);
	if (!a) {
		/* Deny by default. Naming nothing else keeps the refusal from
		 * doubling as a directory of what would have worked. */
		return rcd_err(RCD_E_UNKNOWN, "no such action");
	}

	cJSON *req = cJSON_CreateObject();
	if (!req)
		return rcd_err(RCD_E_MALFORMED, "out of memory");
	cJSON_AddStringToObject(req, "cmd", a->ctrl_cmd ? a->ctrl_cmd : a->name);

	/*
	 * Only the fields the entry names are copied across. Anything else in
	 * the payload -- a 'file' smuggled alongside a permitted action -- is
	 * simply not carried, so it cannot reach a daemon.
	 */
	char err[192];
	for (int i = 0; a->args[i].type != A_END; i++) {
		const char *code = add_arg(req, root, &a->args[i], err, sizeof(err));
		if (code) {
			cJSON_Delete(req);
			return rcd_err(code, err);
		}
	}

	bool fit = cJSON_PrintPreallocated(req, wire, (int)wiresz, 0);
	cJSON_Delete(req);
	if (!fit)
		return rcd_err(RCD_E_MALFORMED, "the request could not be built");

	*owner = a->daemon;
	if (!*owner) {
		const cJSON *sec = cJSON_GetObjectItemCaseSensitive(root, "section");
		*owner = cJSON_IsString(sec) ? rcd_section_reader(sec->valuestring) : NULL;
		if (!*owner)
			return rcd_err(RCD_E_MALFORMED, "the request could not be routed");
	}
	return NULL;
}

cJSON *rcd_cmd_action(rcd_state_t *st, const cJSON *root)
{
	char wire[RCD_REQ_MAX];
	const char *daemon = NULL;

	cJSON *refusal = rcd_action_validate(root, wire, sizeof(wire), &daemon);
	if (refusal)
		return refusal;

	char derr[192];
	if (!rcd_ask_ok_err(daemon, wire, derr, sizeof(derr)))
		return rcd_err(RCD_E_DAEMON, derr);

	const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "action");
	const rcd_action_t *a = rcd_action_find(cJSON_GetStringValue(name));
	if (a && a->persists)
		owe_save(st, rcd_daemon_by_name(daemon));

	RSS_INFO("action: %s -> %s", a ? a->name : "?", daemon);

	cJSON *resp = rcd_ok();
	if (resp) {
		cJSON_AddStringToObject(resp, "action", a ? a->name : "");
		cJSON_AddStringToObject(resp, "owner", daemon);
	}
	return resp;
}
