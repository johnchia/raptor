/*
 * rcd_guard.c -- see rcd_guard.h
 */

#include "rcd_guard.h"
#include "rcd.h"
#include "rcd_proto.h"
#include "rcd_schema.h"
#include "rcd_system.h"

#include <rss_common.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PATH_RECORD RCD_SYSCONF_DIR "/" RCD_GUARD_RECORD_NAME

static uint64_t now_ms(void)
{
	return (uint64_t)(rss_timestamp_us() / 1000);
}

/* ------------------------------------------------------------------ */
/* The record and the deadline                                         */
/* ------------------------------------------------------------------ */

static void record_write(const rcd_state_t *st)
{
	cJSON *arr = cJSON_CreateArray();
	if (!arr)
		return;

	for (int i = 0; i < st->guard_count; i++) {
		cJSON *o = cJSON_CreateObject();
		if (!o)
			continue;
		cJSON_AddStringToObject(o, "section", st->guard[i].section);
		cJSON_AddStringToObject(o, "key", st->guard[i].key);
		/* A key the store had no value for is recorded without one:
		 * there is nothing to put back, and writing an empty string
		 * would put back a value nobody ever chose. */
		if (st->guard[i].had)
			cJSON_AddStringToObject(o, "value", st->guard[i].prev);
		cJSON_AddItemToArray(arr, o);
	}

	char *text = cJSON_PrintUnformatted(arr);
	cJSON_Delete(arr);
	if (!text)
		return;

	/* Written whole and renamed: a snapshot torn in half by a power cut
	 * is worse than none, because rcd would enact it on the way up. */
	char tmp[160];
	snprintf(tmp, sizeof(tmp), "%s.tmp", PATH_RECORD);
	FILE *f = fopen(tmp, "w");
	if (f) {
		/* Nobody but rcd has any business reading this, let alone
		 * rewriting it: it is what the camera will be put back to. */
		if (fchmod(fileno(f), 0600) != 0)
			RSS_WARN("guard: cannot set the mode on %s", tmp);
		bool ok = fputs(text, f) >= 0;
		if (fclose(f) != 0)
			ok = false;
		if (!ok || rename(tmp, PATH_RECORD) != 0) {
			RSS_WARN("guard: cannot record the previous settings in %s", PATH_RECORD);
			unlink(tmp);
		}
	} else {
		RSS_WARN("guard: cannot open %s", tmp);
	}
	free(text);
}

static void record_clear(void)
{
	unlink(PATH_RECORD);
	unlink(RCD_GUARD_ARMED_PATH);
}

static void armed_write(const rcd_state_t *st)
{
	FILE *f = fopen(RCD_GUARD_ARMED_PATH, "w");
	if (!f) {
		RSS_WARN("guard: cannot write %s; a restart of rcd will revert",
			 RCD_GUARD_ARMED_PATH);
		return;
	}
	fprintf(f, "%llu %d\n", (unsigned long long)st->guard_deadline_ms, st->guard_window_sec);
	fclose(f);
}

/* ------------------------------------------------------------------ */
/* Arming                                                              */
/* ------------------------------------------------------------------ */

static void snapshot(rcd_state_t *st)
{
	st->guard_count = 0;

	for (int i = 0; rcd_key_at(i); i++) {
		const rcd_key_t *k = rcd_key_at(i);
		if (k->guard_sec <= 0 || !k->provider)
			continue;
		if (st->guard_count >= RCD_GUARD_MAX) {
			RSS_WARN("guard: more guarded keys than the snapshot holds");
			break;
		}

		rcd_guard_snap_t *s = &st->guard[st->guard_count++];
		memset(s, 0, sizeof(*s));
		rss_strlcpy(s->section, k->section, sizeof(s->section));
		rss_strlcpy(s->key, k->key, sizeof(s->key));
		s->had = k->provider->get(s->prev, sizeof(s->prev)) == 0;
		if (!s->had)
			s->prev[0] = '\0';
	}
}

void rcd_guard_arm(rcd_state_t *st, int window_sec)
{
	if (window_sec <= 0)
		return;

	if (st->guard_deadline_ms) {
		/*
		 * Already armed. The snapshot stays as it is -- it holds the
		 * last state somebody proved they could reach, and the edit
		 * that arrived now was made from inside the same window, over
		 * a connection that has not been confirmed either.
		 */
		st->guard_deadline_ms = now_ms() + (uint64_t)window_sec * 1000;
		st->guard_window_sec = window_sec;
		armed_write(st);
		RSS_INFO("guard: window extended to %d s", window_sec);
		return;
	}

	snapshot(st);
	st->guard_window_sec = window_sec;
	st->guard_deadline_ms = now_ms() + (uint64_t)window_sec * 1000;

	/* The record before the deadline: rcd finding a deadline with no
	 * snapshot beside it has armed a guard it cannot honour. */
	record_write(st);
	armed_write(st);

	RSS_INFO("guard: armed for %d s over %d key(s)", window_sec, st->guard_count);
}

int rcd_guard_remaining(const rcd_state_t *st)
{
	if (!st->guard_deadline_ms)
		return 0;

	uint64_t now = now_ms();
	if (now >= st->guard_deadline_ms)
		return 0;
	/* Rounded up, so a client counting down never shows 0 while the
	 * guard is still armed and waiting for it. */
	return (int)((st->guard_deadline_ms - now + 999) / 1000);
}

void rcd_guard_report(const rcd_state_t *st, cJSON *out)
{
	if (!st->guard_deadline_ms || !out)
		return;

	cJSON *g = cJSON_AddObjectToObject(out, "guard");
	if (!g)
		return;
	cJSON_AddBoolToObject(g, "armed", true);
	cJSON_AddNumberToObject(g, "revert_in_sec", rcd_guard_remaining(st));
	cJSON_AddNumberToObject(g, "window_sec", st->guard_window_sec);

	/* What goes back, so a client can say which settings it is being
	 * asked about rather than "some of them". */
	cJSON *arr = cJSON_AddArrayToObject(g, "keys");
	for (int i = 0; arr && i < st->guard_count; i++) {
		cJSON *o = cJSON_CreateObject();
		if (!o)
			continue;
		cJSON_AddStringToObject(o, "section", st->guard[i].section);
		cJSON_AddStringToObject(o, "key", st->guard[i].key);
		cJSON_AddItemToArray(arr, o);
	}
}

/* ------------------------------------------------------------------ */
/* Disarming, both ways                                                */
/* ------------------------------------------------------------------ */

void rcd_guard_confirm(rcd_state_t *st)
{
	if (!st->guard_deadline_ms)
		return;
	st->guard_deadline_ms = 0;
	st->guard_window_sec = 0;
	st->guard_count = 0;
	record_clear();
	RSS_INFO("guard: confirmed");
}

void rcd_guard_revert(rcd_state_t *st, const char *why)
{
	if (!st->guard_deadline_ms && st->guard_count == 0)
		return;

	RSS_WARN("guard: reverting %d key(s) -- %s", st->guard_count, why ? why : "");

	/*
	 * Table order, which is the order they were taken in. Providers that
	 * share a file are adjacent in the table for that reason: a stanza is
	 * rewritten in the order its keys are declared, going forwards and
	 * coming back.
	 */
	for (int i = 0; i < st->guard_count; i++) {
		const rcd_guard_snap_t *s = &st->guard[i];
		const rcd_key_t *k = rcd_key_find(s->section, s->key);
		if (!k || !k->provider)
			continue;

		if (!s->had) {
			/* Nothing was there to go back to. A provider stores a
			 * value or it does not; there is no verb for unsetting
			 * one, and inventing a default here would be a third
			 * state nobody asked for. */
			RSS_WARN("guard: [%s] %s had no previous value; it keeps the new one",
				 s->section, s->key);
			continue;
		}

		if (k->provider->set(s->prev) != 0)
			RSS_ERROR("guard: [%s] %s could not be put back to %s", s->section, s->key,
				  s->prev);
		else
			RSS_INFO("guard: [%s] %s back to %s", s->section, s->key, s->prev);
	}

	st->guard_deadline_ms = 0;
	st->guard_window_sec = 0;
	st->guard_count = 0;
	record_clear();
}

void rcd_guard_tick(rcd_state_t *st, uint64_t now)
{
	if (!st->guard_deadline_ms || now < st->guard_deadline_ms)
		return;
	rcd_guard_revert(st, "nobody confirmed within the window");
}

/* ------------------------------------------------------------------ */
/* Startup                                                             */
/* ------------------------------------------------------------------ */

/* The deadline left in /run, or 0 when there is none to read. */
static uint64_t armed_read(int *window_sec)
{
	int len = 0;
	char *text = rss_read_file(RCD_GUARD_ARMED_PATH, &len);
	if (!text)
		return 0;

	unsigned long long deadline = 0;
	int win = 0;
	int got = sscanf(text, "%llu %d", &deadline, &win);
	free(text);
	if (got < 1)
		return 0;
	if (window_sec)
		*window_sec = win;
	return (uint64_t)deadline;
}

void rcd_guard_load(rcd_state_t *st)
{
	int len = 0;
	char *text = rss_read_file(PATH_RECORD, &len);
	if (!text)
		return;

	cJSON *root = cJSON_Parse(text);
	free(text);
	if (!root) {
		RSS_WARN("guard: %s is not readable; discarding it", PATH_RECORD);
		record_clear();
		return;
	}

	st->guard_count = 0;
	const cJSON *item = NULL;
	cJSON_ArrayForEach(item, root)
	{
		const cJSON *sec = cJSON_GetObjectItemCaseSensitive(item, "section");
		const cJSON *key = cJSON_GetObjectItemCaseSensitive(item, "key");
		if (!cJSON_IsString(sec) || !cJSON_IsString(key))
			continue;
		/* Resolved through the table rather than trusted from the
		 * file: a key this build no longer has is dropped instead of
		 * being written back through a provider that is gone. */
		if (!rcd_key_find(sec->valuestring, key->valuestring))
			continue;
		if (st->guard_count >= RCD_GUARD_MAX)
			break;

		rcd_guard_snap_t *s = &st->guard[st->guard_count++];
		memset(s, 0, sizeof(*s));
		rss_strlcpy(s->section, sec->valuestring, sizeof(s->section));
		rss_strlcpy(s->key, key->valuestring, sizeof(s->key));
		const cJSON *v = cJSON_GetObjectItemCaseSensitive(item, "value");
		if (cJSON_IsString(v) && v->valuestring) {
			rss_strlcpy(s->prev, v->valuestring, sizeof(s->prev));
			s->had = true;
		}
	}
	cJSON_Delete(root);

	int win = 0;
	uint64_t deadline = armed_read(&win);

	/*
	 * No deadline beside the snapshot. /run is a tmpfs, so it is gone
	 * because the camera rebooted -- and a camera that rebooted inside the
	 * window did not confirm, whatever the reason. Put it back.
	 */
	if (!deadline) {
		rcd_guard_revert(st, "the camera rebooted before anyone confirmed");
		return;
	}

	st->guard_window_sec = win > 0 ? win : RCD_GUARD_NAME_SEC;
	st->guard_deadline_ms = deadline;

	/* rcd itself restarted, and the clock ran on while it was down. */
	if (now_ms() >= deadline) {
		rcd_guard_revert(st, "the window ran out while rcd was not running");
		return;
	}

	RSS_INFO("guard: still armed, %d s left over %d key(s)", rcd_guard_remaining(st),
		 st->guard_count);
}

/* ------------------------------------------------------------------ */
/* The two verbs                                                       */
/* ------------------------------------------------------------------ */

/*
 * Both answer "ok" with nothing armed rather than refusing.
 *
 * The expected caller is a client that has just re-reached the camera and does
 * not know whether it made it inside the window. "There is no guard" is the
 * answer to that question, and an error would send it looking for a fault.
 */
cJSON *rcd_cmd_confirm(rcd_state_t *st, const cJSON *root)
{
	(void)root;

	bool was = st->guard_deadline_ms != 0;
	rcd_guard_confirm(st);

	cJSON *r = rcd_ok();
	if (!r)
		return NULL;
	cJSON_AddBoolToObject(r, "confirmed", was);
	cJSON_AddBoolToObject(r, "armed", false);
	return r;
}

cJSON *rcd_cmd_cancel(rcd_state_t *st, const cJSON *root)
{
	(void)root;

	bool was = st->guard_deadline_ms != 0;
	rcd_guard_revert(st, "cancelled");

	cJSON *r = rcd_ok();
	if (!r)
		return NULL;
	cJSON_AddBoolToObject(r, "reverted", was);
	cJSON_AddBoolToObject(r, "armed", false);
	return r;
}
