/*
 * rcd_config.c -- see rcd_config.h
 */

#include "rcd_config.h"
#include "rcd.h"
#include "rcd_guard.h"
#include "rcd_ipc.h"
#include "rcd_proto.h"
#include "rcd_schema.h"
#include "rcd_wifi.h"

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

void rcd_stale_forget(rcd_state_t *st, const char *section, const char *key)
{
	int kept = 0;
	for (int i = 0; i < st->stale_count; i++) {
		if (strcmp(st->stale[i].section, section) == 0 &&
		    strcmp(st->stale[i].key, key) == 0)
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

	/*
	 * And the camera's own settings, which no daemon owns. Reported under
	 * the same name the schema gives them so that a client grouping drift
	 * by owner finds them where it found the keys -- a page that lists
	 * what an apply will do must not leave out the half that changes the
	 * address it is talking to.
	 */
	const rcd_key_t *owed[RCD_STALE_MAX];
	int n = rcd_enact_owed(st, owed, RCD_STALE_MAX);
	if (n > 0) {
		cJSON *o = cJSON_CreateObject();
		if (o) {
			rcd_impact_t worst = RCD_IMPACT_NONE;
			for (int i = 0; i < n; i++) {
				rcd_impact_t imp = rcd_key_impact(owed[i]);
				if (imp > worst)
					worst = imp;
			}
			cJSON_AddStringToObject(o, "daemon", "system");
			cJSON_AddStringToObject(o, "impact", rcd_impact_name(worst));

			cJSON *keys = cJSON_AddArrayToObject(o, "keys");
			for (int i = 0; keys && i < n; i++) {
				cJSON *k = cJSON_CreateObject();
				if (!k)
					continue;
				cJSON_AddStringToObject(k, "section", owed[i]->section);
				cJSON_AddStringToObject(k, "key", owed[i]->key);
				cJSON_AddItemToArray(keys, k);
			}
			cJSON_AddItemToArray(arr, o);
		}
	}

	if (st->apply_error[0])
		cJSON_AddStringToObject(out, "apply_error", st->apply_error);
}

int rcd_enact_owed(const rcd_state_t *st, const rcd_key_t **out, int max)
{
	int n = 0;
	for (int i = 0; i < st->stale_count && n < max; i++) {
		const rcd_key_t *k = rcd_key_find(st->stale[i].section, st->stale[i].key);
		if (k && k->provider && k->provider->enact)
			out[n++] = k;
	}
	return n;
}

void rcd_enact_done(rcd_state_t *st, const rcd_provider_t *const *done, int n)
{
	int kept = 0;
	for (int i = 0; i < st->stale_count; i++) {
		const rcd_key_t *k = rcd_key_find(st->stale[i].section, st->stale[i].key);

		/* By enact, not by provider: the five keys of one interface
		 * are separate providers sharing one call, and that call
		 * having succeeded settles all of them. */
		bool enacted = false;
		for (int j = 0; k && k->provider && k->provider->enact && j < n; j++)
			enacted = enacted || done[j]->enact == k->provider->enact;

		if (enacted)
			continue;
		st->stale[kept++] = st->stale[i];
	}
	st->stale_count = kept;
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

	int i = 0;
	for (; choices[i] && n > 0 && (size_t)n < errsz; i++)
		n += snprintf(err + n, errsz - (size_t)n, " %s", choices[i]);

	/*
	 * The timezone list is three hundred entries and none of them fits
	 * past the first dozen. Saying so beats a sentence that stops in the
	 * middle of Africa and reads like the list ends there -- a caller can
	 * fetch the whole thing from `schema`, which is where it belongs.
	 */
	static const char tail[] = " ... (see schema)";
	if (choices[i] && errsz > sizeof(tail))
		memcpy(err + errsz - sizeof(tail), tail, sizeof(tail));
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
	e->reset = false;
	e->rendered[0] = '\0';

	/*
	 * An explicit null is a reset: put this key back to its default. A
	 * missing value is still a request that forgot one -- the two are
	 * distinguishable here and mean opposite things, so they are not
	 * folded together.
	 */
	if (cJSON_IsNull(v)) {
		if (!rcd_key_resettable(k)) {
			snprintf(err, errsz, "'%s' has no default to go back to", k->key);
			return RCD_E_RANGE;
		}
		e->reset = true;
		return NULL;
	}

	if (!v) {
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
		/*
		 * An ISP knob also takes the word, and keeps it as the word:
		 * "auto" asks for the tuning file's own curve, which is not a
		 * point on the knob's scale and cannot be written as one. The
		 * live command carries the string and rvd reads it there; the
		 * file gets the same five characters, so somebody reading it
		 * later sees what was meant. See rcd_key_t::auto_ok.
		 */
		if (k->auto_ok && cJSON_IsString(v) && v->valuestring &&
		    strcmp(v->valuestring, "auto") == 0) {
			rss_strlcpy(e->rendered, "auto", sizeof(e->rendered));
			return NULL;
		}
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

	if (k->type == V_TEXT) {
		if (!cJSON_IsString(v) || !v->valuestring) {
			snprintf(err, errsz, "'%s' must be a string", k->key);
			return RCD_E_TYPE;
		}
		const char *s = v->valuestring;
		size_t n = strlen(s);
		if (n < (size_t)k->min || n > (size_t)k->max || n >= sizeof(e->rendered)) {
			snprintf(err, errsz, "'%s' must be %d to %d characters", k->key, k->min,
				 k->max);
			return RCD_E_RANGE;
		}
		for (size_t i = 0; i < n; i++) {
			unsigned char c = (unsigned char)s[i];
			/*
			 * Printable ASCII only. A control byte here would be a
			 * newline in a line-oriented file, and the two
			 * exclusions below would each end the quoted string
			 * this value is rendered inside.
			 */
			if (c < 0x20 || c > 0x7e || c == '"' || c == '\\') {
				snprintf(err, errsz,
					 "'%s' may contain only printable characters, and not "
					 "'\"' or '\\'",
					 k->key);
				return RCD_E_CHOICE;
			}
		}
		memcpy(e->rendered, s, n);
		e->rendered[n] = '\0';
		return NULL;
	}

	if (k->type == V_SECRET) {
		if (!cJSON_IsString(v) || !v->valuestring) {
			snprintf(err, errsz, "'%s' must be a string", k->key);
			return RCD_E_TYPE;
		}
		const char *s = v->valuestring;
		size_t n = strlen(s);

		/*
		 * An open network has no passphrase, which is a configuration
		 * and not an omission -- the same reason the gateway and the
		 * name server accept it. The table decides whether this key
		 * is one of those.
		 */
		if (!n) {
			if (k->min != 0) {
				snprintf(err, errsz, "'%s' needs a passphrase", k->key);
				return RCD_E_RANGE;
			}
			e->rendered[0] = '\0';
			return NULL;
		}

		/*
		 * Exactly 64 hex digits is a pre-derived PSK rather than a
		 * passphrase, and it is checked first because it is outside
		 * WPA's 8-to-63 length rule rather than an instance of it.
		 * This is the form a client sends when it has hashed the
		 * passphrase itself, so the plaintext never crosses the
		 * network that carried it.
		 */
		if (n == 64) {
			size_t hex = 0;
			while (hex < n && isxdigit((unsigned char)s[hex]))
				hex++;
			if (hex == n) {
				memcpy(e->rendered, s, n);
				e->rendered[n] = '\0';
				return NULL;
			}
		}

		if (n < 8 || n > 63) {
			/* Not quoting the value back, for the V_CRED reason:
			 * it is a secret and must not reach a log or a reply. */
			snprintf(err, errsz, "'%s' must be 8 to 63 characters, or 64 hex digits",
				 k->key);
			return RCD_E_RANGE;
		}
		for (size_t i = 0; i < n; i++) {
			unsigned char c = (unsigned char)s[i];
			if (c < 0x20 || c > 0x7e || c == '"' || c == '\\') {
				snprintf(err, errsz,
					 "'%s' may contain only printable characters, and not "
					 "'\"' or '\\'",
					 k->key);
				return RCD_E_CHOICE;
			}
		}
		memcpy(e->rendered, s, n);
		e->rendered[n] = '\0';
		return NULL;
	}

	if (k->type == V_HOST) {
		if (!cJSON_IsString(v) || !v->valuestring) {
			snprintf(err, errsz, "'%s' must be a string", k->key);
			return RCD_E_TYPE;
		}
		const char *s = v->valuestring;
		size_t n = strlen(s);
		if (n < 1 || n > (size_t)k->max || n >= sizeof(e->rendered)) {
			snprintf(err, errsz, "'%s' must be 1 to %d characters", k->key, k->max);
			return RCD_E_RANGE;
		}
		for (size_t i = 0; i < n; i++) {
			unsigned char c = (unsigned char)s[i];
			if (isalnum(c) || c == '.' || c == '-')
				continue;
			snprintf(err, errsz, "'%s' must be a hostname or an IPv4 address", k->key);
			return RCD_E_CHOICE;
		}
		/* Leading or trailing punctuation is not a hostname anyone
		 * meant, and a leading '-' would be an option to whatever
		 * eventually reads the file. */
		if (s[0] == '.' || s[0] == '-' || s[n - 1] == '.' || s[n - 1] == '-') {
			snprintf(err, errsz, "'%s' may not start or end with '.' or '-'", k->key);
			return RCD_E_CHOICE;
		}
		memcpy(e->rendered, s, n);
		e->rendered[n] = '\0';
		return NULL;
	}

	if (k->type == V_IPV4) {
		if (!cJSON_IsString(v) || !v->valuestring) {
			snprintf(err, errsz, "'%s' must be a string", k->key);
			return RCD_E_TYPE;
		}
		const char *sv = v->valuestring;

		/*
		 * Empty is a value where the table allows it. A camera with no
		 * gateway is on a flat network and a camera with no name
		 * server is one that resolves nothing -- both are
		 * configurations somebody may mean, and neither can be
		 * expressed by leaving the key out of a form that always
		 * submits every field.
		 */
		if (!sv[0]) {
			if (k->min != 0) {
				snprintf(err, errsz, "'%s' needs an address", k->key);
				return RCD_E_RANGE;
			}
			e->rendered[0] = '\0';
			return NULL;
		}

		int octet = 0, digits = 0, value = 0;
		bool bad = false;
		for (const char *c = sv;; c++) {
			if (*c >= '0' && *c <= '9') {
				/* A leading zero is octal to most things that
				 * will read this file back, and to nobody who
				 * types it. Refused rather than reinterpreted. */
				if (digits == 1 && value == 0)
					bad = true;
				value = value * 10 + (*c - '0');
				if (++digits > 3 || value > 255)
					bad = true;
				continue;
			}
			if (*c == '.' || *c == '\0') {
				if (digits == 0)
					bad = true;
				octet++;
				digits = 0;
				value = 0;
				if (*c == '\0')
					break;
				continue;
			}
			bad = true;
			break;
		}
		if (bad || octet != 4) {
			snprintf(err, errsz, "'%s' must be four numbers 0-255 separated by dots",
				 k->key);
			return RCD_E_CHOICE;
		}

		rss_strlcpy(e->rendered, sv, sizeof(e->rendered));
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
/* What this SoC can actually do                                       */
/* ------------------------------------------------------------------ */

/*
 * A camera advertises the same table whatever silicon it is running on, and
 * more than half of [image] is absent on some of it: the i6c has no hue, no
 * spatial denoise, no DPC, DRC, highlight or backlight control and no gain
 * ceilings. Offering those and reporting "not supported on this SoC" after the
 * fact is the worst of both -- the operator has already decided what to
 * change, and on the old path the value was written to the file and the
 * pipeline marked stale, inviting a restart to enact something the silicon
 * cannot do.
 *
 * rvd answers this from the vtable it dispatches through, so there is no
 * second list to drift: a NULL entry is exactly what makes RSS_HAL_CALL
 * return NOTSUP. It reports that a setter exists, not that a live call will
 * be honoured -- orientation is published on SigmaStar and refused while the
 * channel is up -- which is the distinction that keeps hflip visible.
 */
static void probe_isp(rcd_state_t *st)
{
	if (st->isp_settable[0])
		return;

	cJSON *resp = rcd_ask_json("rvd", "{\"cmd\":\"get-isp\"}");
	if (!resp)
		return; /* rvd is down; ask again next time rather than hide */

	const cJSON *v = cJSON_GetObjectItemCaseSensitive(resp, "settable");
	if (cJSON_IsString(v) && v->valuestring && v->valuestring[0])
		rss_strlcpy(st->isp_settable, v->valuestring, sizeof(st->isp_settable));
	cJSON_Delete(resp);
}

bool rcd_key_available(rcd_state_t *st, const rcd_key_t *k)
{
	if (!k)
		return true;

	/* A camera with no radio has no wifi settings, as opposed to having
	 * them unset -- which is what drops the section from a console rather
	 * than offering a form nothing can act on. */
	if (strcmp(k->section, "wifi") == 0)
		return rcd_wifi_present();

	/* Only [image] is answered for beyond that, and only once rvd has said
	 * something. Everything else is available until proven otherwise,
	 * which is the safe direction: hiding a working control is worse than
	 * showing one that turns out to refuse. */
	if (strcmp(k->section, "image") != 0)
		return true;

	probe_isp(st);
	if (!st->isp_settable[0])
		return true;

	char needle[RCD_KEY_MAX + 2];
	snprintf(needle, sizeof(needle), ",%s,", k->key);
	return strstr(st->isp_settable, needle) != NULL;
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
	if (k->type == V_INT) {
		/*
		 * Except for the one integer that has a word among its values.
		 * Read as a number, "auto" is 0 -- a value on the scale, in
		 * range, and wrong: it would show a knob following the tuning
		 * as one pinned at its floor, and a client that wrote it back
		 * would pin it there for real.
		 */
		if (k->auto_ok && strcmp(raw, "auto") == 0)
			return cJSON_CreateString(raw);
		return cJSON_CreateNumber(strtol(raw, NULL, 10));
	}
	return cJSON_CreateString(raw);
}

static void emit_value(cJSON *arr, const rcd_key_t *k, rss_config_t *file, const cJSON *from_daemon)
{
	/* A credential is settable and never readable. Reporting the key with
	 * no value is the honest rendering: the client draws the input and
	 * knows not to expect it to fill in. */
	if (k->type == V_CRED || k->type == V_SECRET) {
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

	/*
	 * A provider-backed key is neither in the file nor in a daemon: its
	 * store is the answer, and the only one. Reported as unset when the
	 * store says nothing, which is what an unconfigured camera looks like.
	 */
	char provided[RCD_VAL_MAX];
	if (k->provider) {
		if (k->provider->get(provided, sizeof(provided)) == 0) {
			raw = provided;
			source = "system";
		}
		from_daemon = NULL;
		file = NULL;
	}

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

	/*
	 * Whether anybody chose this, as opposed to it being what the daemon
	 * runs on when nobody has. The value above cannot answer that: a
	 * daemon reports its resolved default in the same shape as a
	 * configured one, and it is right to -- that is the value in force.
	 * Only the file knows which of the two it is, and a client offering
	 * to put the key back has to ask.
	 */
	bool configured =
		k->provider ? true : file && rss_config_get_str(file, k->section, k->key, NULL);
	if (!configured)
		cJSON_AddBoolToObject(o, "configured", false);

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

/*
 * What each section's daemon said, for the length of one request.
 *
 * `section` is the table's own string rather than a copy: the table outlives
 * every request, and the entries being compared came out of it.
 *
 * A section whose daemon said nothing is cached as nothing, deliberately. The
 * expensive case is a daemon that is not answering, and re-asking it once per
 * key is exactly what this exists to stop.
 */
typedef struct {
	const char *section;
	cJSON *live; /* owned */
} live_cache_t;

static cJSON *live_for(live_cache_t *cache, int *count, const char *section)
{
	for (int i = 0; i < *count; i++) {
		if (strcmp(cache[i].section, section) == 0)
			return cache[i].live;
	}

	/*
	 * Unreachable: the table has fewer sections than the cache has room
	 * for, and a test asserts it. Answering without the daemon's values
	 * rather than asking uncached, because falling back to the thing this
	 * function exists to prevent is not a fallback.
	 */
	if (*count >= RCD_LIVE_MAX)
		return NULL;

	cache[*count].section = section;
	cache[*count].live = section_from_daemon(section);
	return cache[(*count)++].live;
}

static void live_cache_free(live_cache_t *cache, int count)
{
	for (int i = 0; i < count; i++)
		cJSON_Delete(cache[i].live);
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

	/*
	 * Named keys, which is the other half of the same trip.
	 *
	 * The comment above section_from_daemon says one round trip per
	 * section rather than per key, and until this cache it was true only
	 * of the branch above: naming forty keys of one section asked rvd for
	 * that section forty times, and nothing noticed because a healthy
	 * daemon answers in well under a millisecond.
	 *
	 * A daemon that has stopped answering is where it stops being
	 * invisible, and the cap below is the other half of that -- see
	 * RCD_GETS_MAX.
	 */
	live_cache_t cache[RCD_LIVE_MAX];
	int cached = 0;
	int asked = 0;

	const cJSON *ke = NULL;
	cJSON_ArrayForEach(ke, keys)
	{
		if (++asked > RCD_GETS_MAX) {
			live_cache_free(cache, cached);
			cJSON_Delete(resp);
			rss_config_free(file);
			return rcd_err(RCD_E_TOOMANY, "too many keys in one request");
		}

		const cJSON *s = cJSON_GetObjectItemCaseSensitive(ke, "section");
		const cJSON *n = cJSON_GetObjectItemCaseSensitive(ke, "key");
		if (!cJSON_IsString(s) || !cJSON_IsString(n))
			continue;

		const rcd_key_t *k = rcd_key_find(s->valuestring, n->valuestring);
		if (!k)
			continue;

		emit_value(out, k, file, live_for(cache, &cached, k->section));
	}

	live_cache_free(cache, cached);
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
static int write_file(rcd_state_t *st, rcd_edit_t *edits, const bool *to_file, bool *changed, int n)
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

		/*
		 * A reset takes the line out. One asked for on a key that has
		 * no line changes nothing, and saying so here is what keeps it
		 * from being recorded as drift: a section put back to its
		 * defaults is mostly keys that were already at them, and
		 * charging a restart for those would make the button useless.
		 */
		if (edits[i].reset) {
			changed[i] = rss_config_unset(cfg, edits[i].k->section, edits[i].k->key);
			if (changed[i])
				RSS_INFO("reset: [%s] %s back to its default", edits[i].k->section,
					 edits[i].k->key);
			continue;
		}

		rss_config_set_str(cfg, edits[i].k->section, edits[i].k->key, edits[i].rendered);
		changed[i] = true;

		/* The log records what changed, not what it changed to -- for
		 * a password those are different things, and this file is
		 * readable by anyone who can read the flash. */
		bool secret =
			edits[i].k->type == V_SECRET ||
			(edits[i].k->type == V_CRED && strcmp(edits[i].k->key, "password") == 0);
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
	 * Refused whole, before anything is written. A batch is all-or-nothing
	 * everywhere else in this command and an edit the silicon cannot carry
	 * out is no different -- and writing it anyway would mark the pipeline
	 * stale, which asks the operator to restart the camera to enact
	 * something that cannot exist.
	 */
	for (int i = 0; i < n; i++) {
		if (rcd_key_available(st, edits[i].k))
			continue;
		cJSON *e = rcd_err(RCD_E_UNSUPPORTED, "this camera has no such control");
		rcd_err_where(e, edits[i].k->section, edits[i].k->key);
		return e;
	}

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
	bool changed[RCD_EDITS_MAX] = {false};
	bool staged[RCD_EDITS_MAX] = {false};
	const char *note[RCD_EDITS_MAX] = {NULL};
	char notebuf[RCD_EDITS_MAX][192];

	for (int i = 0; i < n; i++) {
		const rcd_key_t *k = edits[i].k;

		/*
		 * A provider is the store, so there is no file to fall back to
		 * and nothing to stage: `stage` exists so that a form's live
		 * fields land together with its saved ones, and these have no
		 * second half to wait for.
		 *
		 * A failure here is reported as the whole request failing, the
		 * same as a config file that could not be written. Both can
		 * leave an earlier edit in the batch applied; both mean the
		 * storage is broken, which is not a state a partial success
		 * report helps anyone with.
		 */
		if (k->provider) {
			/*
			 * What is about to be written over, remembered before
			 * it is. Nothing starts counting here: this costs
			 * nothing and can be undone by writing it again, which
			 * is precisely why it is not the moment to guard.
			 */
			if (k->guard_sec > 0)
				rcd_guard_hold(st);

			char before[RCD_VAL_MAX] = "";
			bool had = k->provider->get(before, sizeof(before)) == 0;

			if (k->provider->set(edits[i].reset ? "" : edits[i].rendered) != 0) {
				cJSON_Delete(resp);
				cJSON *e = rcd_err(RCD_E_IO, "the setting could not be stored");
				rcd_err_where(e, k->section, k->key);
				return e;
			}
			/*
			 * A store already holding what was asked of it owes
			 * nothing, and a reset asks that of most of a section.
			 * Read back rather than compared to the request: only
			 * the store knows what emptying it came to.
			 */
			char after[RCD_VAL_MAX] = "";
			bool has = k->provider->get(after, sizeof(after)) == 0;
			changed[i] = had != has || (had && strcmp(before, after) != 0);
			if (!changed[i]) {
				note[i] = edits[i].reset ? "already at its default"
							 : "already set to that";
				continue;
			}

			/*
			 * A provider that can enact has not enacted: the store
			 * holds the value and the running system does not, so
			 * it is drift like any other and `apply` settles it.
			 */
			if (k->provider->enact) {
				rcd_stale_add(st, k->section, k->key, RCD_D_COUNT);
				staged[i] = true;
				note[i] =
					k->guard_sec > 0
						? "stored -- apply puts it in force, and you will "
						  "have to confirm it"
						: "stored -- apply puts it in force";
			} else if (rcd_key_impact(k) == RCD_IMPACT_REBOOT) {
				note[i] = "stored -- takes effect on reboot";
			}
			continue;
		}

		/*
		 * A reset has no value to hand a live command, and rcd has no
		 * default to put in its place -- the daemon's own is the point
		 * of the exercise, and it reaches it by reading the file at
		 * its next start. So every reset is restart-tier, whatever the
		 * key's tier is when it carries a value.
		 */
		if (!k->live_cmd || stage_only || edits[i].reset) {
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

	if (write_file(st, edits, to_file, changed, n) != 0) {
		cJSON_Delete(resp);
		return rcd_err(RCD_E_IO, "the config file could not be written");
	}

	for (int i = 0; i < n; i++)
		if (edits[i].reset && to_file[i] && !changed[i])
			note[i] = "already at its default";

	int saved = 0;
	for (int i = 0; i < n; i++) {
		if (staged[i]) {
			saved++; /* already recorded above, with no daemon */
			continue;
		}
		if (!to_file[i] || !changed[i])
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
		 * credential is echoed as nothing at all, and a reset has
		 * nothing to echo -- what the key now reads as is whatever the
		 * daemon that owns it brings, and rcd is not the one to say. */
		if (edits[i].reset) {
			cJSON_AddBoolToObject(o, "reset", true);
		} else if (edits[i].k->type != V_CRED && edits[i].k->type != V_SECRET) {
			cJSON *v = typed_value(edits[i].k, edits[i].rendered);
			if (v)
				cJSON_AddItemToObject(o, "value", v);
		}
		/*
		 * "saved" means rcd still owes this to a daemon restart, which
		 * is what `apply` is for. Anything else is in force already:
		 * a live command that reached its daemon, a provider whose
		 * store is the value, or an edit that found the setting the
		 * way it was asking for it.
		 */
		bool owed = staged[i] || (to_file[i] && changed[i]);
		cJSON_AddStringToObject(o, "applied", owed ? "saved" : "live");
		if (note[i] && note[i][0])
			cJSON_AddStringToObject(o, "note", note[i]);
		cJSON_AddItemToArray(results, o);
	}

	/* What is now owed. Reported on every set, so a client never has to
	 * ask a second question to know whether it just cost an outage. */
	rcd_config_report_stale(st, resp);

	/* And the clock, if one is already running -- a set made inside an
	 * open window does not start a second one. */
	rcd_guard_report(st, resp);
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

	/* An action rcd performs itself has nothing to route to, and the
	 * request built above is never sent -- it is built anyway so that one
	 * path validates every action's arguments. */
	if (a->local) {
		*owner = NULL;
		return NULL;
	}

	*owner = a->daemon;
	if (!*owner) {
		const cJSON *sec = cJSON_GetObjectItemCaseSensitive(root, "section");
		*owner = cJSON_IsString(sec) ? rcd_section_reader(sec->valuestring) : NULL;
		if (!*owner)
			return rcd_err(RCD_E_MALFORMED, "the request could not be routed");
	}
	return NULL;
}

/*
 * An action rcd carries out itself.
 *
 * Two refusals before the handler, and neither belongs to the handler. A
 * local action reaches a store directly, with no daemon in between to hold an
 * opinion about whether the hardware exists or whether now is the moment --
 * so the questions a daemon would have answered are asked here.
 */
static cJSON *action_local(rcd_state_t *st, const rcd_action_t *a)
{
	if (a->avail && !a->avail())
		return rcd_err(RCD_E_UNSUPPORTED, "this camera has no such hardware");

	/*
	 * Not while the guard is holding a snapshot -- for an action that
	 * costs something.
	 *
	 * The stores rcd writes directly are exactly the stores the guard
	 * snapshots, so an unconfirmed change and a local action are two
	 * writers of the same variables with a timer between them: the revert
	 * would land afterwards and put back the very thing this was asked to
	 * clear, and the camera would come up on a network nobody chose. It
	 * is refused rather than silently winning or silently losing, and
	 * both ways out -- confirm, cancel -- are one call.
	 *
	 * An action whose impact is none writes nothing, so there is nothing
	 * for a revert to race and refusing it would only take a scan away
	 * from the page that is waiting on the guard.
	 */
	if (a->impact != RCD_IMPACT_NONE && rcd_guard_held(st))
		return rcd_err(RCD_E_BUSY,
			       "a change is still waiting to be confirmed; confirm or cancel it "
			       "first");

	cJSON *resp = rcd_ok();
	if (!resp)
		return NULL;

	char err[192] = "";
	if (a->local(resp, err, sizeof(err)) != 0) {
		cJSON_Delete(resp);
		return rcd_err(RCD_E_IO, err[0] ? err : "it could not be done");
	}

	/* A read-only action is a poll: the portal asks for a scan every few
	 * seconds, and a line each would bury everything else in the log. */
	if (a->impact != RCD_IMPACT_NONE)
		RSS_INFO("action: %s -> rcd", a->name);
	else
		RSS_DEBUG("action: %s -> rcd", a->name);

	cJSON_AddStringToObject(resp, "action", a->name);
	cJSON_AddStringToObject(resp, "owner", "rcd");
	cJSON_AddStringToObject(resp, "impact", rcd_impact_name(a->impact));
	if (a->note)
		cJSON_AddStringToObject(resp, "note", a->note);
	return resp;
}

cJSON *rcd_cmd_action(rcd_state_t *st, const cJSON *root)
{
	char wire[RCD_REQ_MAX];
	const char *daemon = NULL;

	cJSON *refusal = rcd_action_validate(root, wire, sizeof(wire), &daemon);
	if (refusal)
		return refusal;

	const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "action");
	const rcd_action_t *a = rcd_action_find(cJSON_GetStringValue(name));

	if (a && a->local)
		return action_local(st, a);

	char derr[192];
	if (!rcd_ask_ok_err(daemon, wire, derr, sizeof(derr)))
		return rcd_err(RCD_E_DAEMON, derr);

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
