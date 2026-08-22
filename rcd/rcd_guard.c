/*
 * rcd_guard.c -- see rcd_guard.h
 */

#include "rcd_guard.h"
#include "rcd.h"
#include "rcd_config.h"
#include "rcd_proto.h"
#include "rcd_schema.h"
#include "rcd_system.h"
#include "rcd_wifi.h"

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
	cJSON *root = cJSON_CreateObject();
	if (!root)
		return;

	/*
	 * Armed or merely held. A held snapshot is one whose values were
	 * written to their stores and never enacted, so finding it after a
	 * restart means there is nothing in force to undo -- while an armed
	 * one found after a reboot is exactly the case the guard exists for.
	 */
	cJSON_AddBoolToObject(root, "armed", st->guard_deadline_ms != 0);
	cJSON_AddNumberToObject(root, "window_sec", st->guard_window_sec);

	cJSON *arr = cJSON_AddArrayToObject(root, "keys");
	for (int i = 0; arr && i < st->guard_count; i++) {
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

	char *text = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
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

/*
 * What the store holds, which is not always what `get` answers. A provider
 * that falls back elsewhere when its own store is silent publishes `stored`
 * so the guard can tell the two apart -- rcd_provider_t says why that matters
 * here and nowhere else.
 *
 * Both the snapshot and the comparison below go through this, because a
 * snapshot taken one way and compared the other would find every such key
 * changed and write the fallback back on the strength of it.
 */
static int guard_get(const rcd_provider_t *p, char *out, size_t outsz)
{
	return p->stored ? p->stored(out, outsz) : p->get(out, outsz);
}

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
		s->had = guard_get(k->provider, s->prev, sizeof(s->prev)) == 0;
		if (!s->had)
			s->prev[0] = '\0';
	}
}

void rcd_guard_hold(rcd_state_t *st)
{
	/*
	 * Already holding one. It stays: it is the last state somebody proved
	 * they could reach, and an edit made after it -- whether or not the
	 * one before it was ever applied -- is part of the same experiment.
	 */
	if (st->guard_count > 0)
		return;

	snapshot(st);
	record_write(st);
	RSS_INFO("guard: holding %d key(s) against an apply", st->guard_count);
}

/*
 * What may be said out loud about a value being put back.
 *
 * The revert is the one place a guarded value is written to the log, and two
 * of the keys it covers are secrets: the wifi passphrase, and a credential if
 * one is ever guarded. `set` and `get` have refused to report those since they
 * existed, and this was the hole left behind -- a wifi revert printed the
 * whole PSK to syslog, where it outlives the change that caused it.
 *
 * The line is kept rather than dropped: which key went back is what an
 * operator is reading the log for, and only the value has to go.
 */
static const char *redacted(const rcd_key_t *k, const char *value)
{
	if (k->type == V_SECRET || k->type == V_CRED)
		return "its previous value";
	return value;
}

/*
 * Did any wifi key move? The snapshot holds what each guarded store held
 * before `set` wrote it, so a key whose store now reads differently is one
 * that changed -- including a key that had no value and now has one, which is
 * the fresh-camera case the portal is.
 */
static bool wifi_moved(const rcd_state_t *st)
{
	for (int i = 0; i < st->guard_count; i++) {
		const rcd_guard_snap_t *s = &st->guard[i];

		if (strcmp(s->section, "wifi") != 0)
			continue;

		const rcd_key_t *k = rcd_key_find(s->section, s->key);
		if (!k || !k->provider)
			continue;

		char now[RCD_VAL_MAX] = "";
		bool has = guard_get(k->provider, now, sizeof(now)) == 0;

		if (has != s->had || (has && strcmp(now, s->prev) != 0))
			return true;
	}
	return false;
}

void rcd_guard_arm(rcd_state_t *st, int window_sec)
{
	if (window_sec <= 0)
		return;

	/* Nothing was held -- an apply of guarded keys that no set preceded,
	 * which is a client enacting somebody else's staged edit. Snapshot
	 * now: it is the state that is about to stop being true. */
	if (st->guard_count == 0)
		snapshot(st);

	bool extend = st->guard_deadline_ms != 0;
	st->guard_retries = 0; /* a fresh experiment, not a stuck revert */
	st->guard_window_sec = window_sec;
	st->guard_deadline_ms = now_ms() + (uint64_t)window_sec * 1000;

	/*
	 * Whether a wifi credential is among what changed, which decides
	 * whether this window can end by itself. Answered by comparing the
	 * snapshot against the store rather than by being told: `set` has
	 * already written the store, so the two differ exactly where
	 * something moved, and nothing has to be threaded through apply.
	 *
	 * Sticky across an extension: a second guarded set inside the window
	 * keeps the first snapshot, so it must not take away the reason the
	 * first one could confirm itself.
	 */
	if (!extend)
		st->guard_wifi_moved = false;
	st->guard_wifi_moved |= wifi_moved(st);
	st->guard_wifi_ask_ms = now_ms() + (uint64_t)RCD_GUARD_WIFI_DWELL_SEC * 1000;

	/* The record before the deadline: rcd finding a deadline with no
	 * snapshot beside it has armed a guard it cannot honour. */
	record_write(st);
	armed_write(st);

	RSS_INFO("guard: %s for %d s over %d key(s)", extend ? "window extended" : "armed",
		 window_sec, st->guard_count);
}

bool rcd_guard_held(const rcd_state_t *st)
{
	return st && st->guard_count > 0;
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
	st->guard_retries = 0;
	st->guard_wifi_moved = false;
	st->guard_wifi_ask_ms = 0;
	record_clear();
	RSS_INFO("guard: confirmed");
}

/* Collected rather than called: five keys of one stanza share one enact, and
 * bringing the interface up five times would turn one revert into five
 * outages. Nothing is collected for a snapshot that was never armed -- there
 * is nothing in force to undo, and an enact would be an outage the undo
 * invented. */
static void add_enact(const rcd_provider_t **list, int *count, const rcd_provider_t *p,
		      bool was_armed)
{
	if (!was_armed || *count >= RCD_GUARD_MAX)
		return;
	for (int i = 0; i < *count; i++) {
		if (list[i]->enact == p->enact)
			return;
	}
	list[(*count)++] = p;
}

/*
 * A revert that did not fully land is retried, not forgotten.
 *
 * The reasons one fails are mostly the ones that pass: an overlay that has not
 * finished coming back, a filesystem that is momentarily full, an interface
 * that will not come up on the first attempt. Dropping the snapshot on the
 * first of those would leave the camera on settings nobody confirmed with no
 * record of what it had before -- the precise moment the guard exists for, and
 * the one case where giving up is unrecoverable.
 */
#define RCD_GUARD_RETRY_MS 15000
#define RCD_GUARD_RETRIES  4

static void guard_retry(rcd_state_t *st, int failed)
{
	/* Whatever did go back is settled and no longer owed to an apply, so
	 * the drift record is written even though the guard is not done. */
	rcd_stale_save(st);

	if (st->guard_retries < RCD_GUARD_RETRIES) {
		st->guard_retries++;
		st->guard_deadline_ms = now_ms() + RCD_GUARD_RETRY_MS;

		/* The marker only. The snapshot on flash is unchanged and this
		 * is a camera whose /etc is already refusing writes; rewriting
		 * it every fifteen seconds would spend the flash on saying the
		 * same thing. */
		armed_write(st);
		RSS_ERROR("guard: %d key(s) did not go back; trying again in %d s (attempt %d "
			  "of %d)",
			  failed, RCD_GUARD_RETRY_MS / 1000, st->guard_retries, RCD_GUARD_RETRIES);
		return;
	}

	/*
	 * Out of attempts. The record stays and the marker goes, which is the
	 * pair rcd_guard_load reads as "armed, and the camera has rebooted
	 * since" -- so power-cycling still puts the old settings back, exactly
	 * as rcd_guard.h promises. What stops here is the retrying, not the
	 * guard.
	 */
	RSS_ERROR("guard: %d key(s) still will not go back after %d attempts; the record is kept, "
		  "so power-cycling the camera restores them",
		  failed, RCD_GUARD_RETRIES);
	st->guard_deadline_ms = 0;
	st->guard_window_sec = 0;
	st->guard_count = 0;
	st->guard_retries = 0;
	unlink(RCD_GUARD_ARMED_PATH);
}

void rcd_guard_revert(rcd_state_t *st, const char *why)
{
	if (!st->guard_deadline_ms && st->guard_count == 0)
		return;

	/* Whether anything is actually in force. A snapshot that was held and
	 * never applied has stores to put back and nothing to re-enact -- and
	 * bouncing an interface to undo a change that never reached it would
	 * be an outage invented by the undo. */
	bool was_armed = st->guard_deadline_ms != 0;

	RSS_INFO("guard: putting back what changed of %d key(s) -- %s", st->guard_count,
		 why ? why : "");

	const rcd_provider_t *enacted[RCD_GUARD_MAX];
	int enact_count = 0;

	/* Anything that would not go back. Counted rather than noted, because
	 * what happens next is the same whichever key it was. */
	int failed = 0;

	/*
	 * Table order, which is the order they were taken in. Providers that
	 * share a file are adjacent in the table for that reason: a stanza is
	 * rewritten in the order its keys are declared, going forwards and
	 * coming back.
	 */
	for (int i = 0; i < st->guard_count; i++) {
		/* Not const: a revert that writes a store marks it as owing the
		 * enact that puts it back into force. */
		rcd_guard_snap_t *s = &st->guard[i];
		const rcd_key_t *k = rcd_key_find(s->section, s->key);
		if (!k || !k->provider)
			continue;

		/*
		 * Only what actually moved. The snapshot covers every guarded
		 * key so that a batch is undone as a batch, but writing back
		 * a value that never changed leaves a fingerprint on a store
		 * nobody touched -- and for a key whose `get` falls back to
		 * somewhere else when its own store is silent, it would
		 * materialise that fallback as a setting. It also decides
		 * whether anything needs re-enacting at all: a revert of keys
		 * that all still hold their old values is not an outage.
		 */
		char cur[RCD_VAL_MAX] = "";
		bool has_cur = guard_get(k->provider, cur, sizeof(cur)) == 0;

		if ((!s->had && !has_cur) || (s->had && has_cur && strcmp(cur, s->prev) == 0)) {
			/* Already what it was. Nothing to write, nothing to
			 * enact -- and nothing owed to an apply either, which
			 * is the part that has to be said out loud: a key
			 * staged and then set back by hand is settled, and
			 * leaving it in the drift list would offer to enact a
			 * change that is no longer one. */

			/*
			 * Unless an earlier pass is what made it match and
			 * could not then put it into force. That key is not
			 * settled, it is halfway back -- and telling it apart
			 * from one that never moved is the whole reason the
			 * snapshot carries `owed_enact`, because re-enacting
			 * the ones that never moved bounces every guarded
			 * interface on the camera.
			 */
			if (s->owed_enact && k->provider->enact)
				add_enact(enacted, &enact_count, k->provider, was_armed);
			rcd_stale_forget(st, s->section, s->key);
			continue;
		}

		/*
		 * Nothing was there before, and something is now. The store
		 * has to go back to having no value rather than to an empty
		 * one -- a camera whose address was never configured is the
		 * ordinary case, not an edge, since that is every camera that
		 * has only ever used DHCP.
		 */
		if (!s->had) {
			if (k->provider->set("") != 0) {
				RSS_WARN("guard: [%s] %s cannot be emptied; it keeps the new "
					 "value",
					 s->section, s->key);
				failed++;
				continue;
			}
			RSS_INFO("guard: [%s] %s back to unset", s->section, s->key);
			if (k->provider->enact) {
				s->owed_enact = true;
				add_enact(enacted, &enact_count, k->provider, was_armed);
			}
			rcd_stale_forget(st, s->section, s->key);
			continue;
		}

		if (k->provider->set(s->prev) != 0) {
			RSS_ERROR("guard: [%s] %s could not be put back to %s", s->section, s->key,
				  redacted(k, s->prev));
			failed++;
			continue;
		}
		RSS_INFO("guard: [%s] %s back to %s", s->section, s->key, redacted(k, s->prev));

		/*
		 * And it is no longer owed to an apply. The store holds what
		 * the system holds again -- whether because the enact below
		 * is about to put it there, or because nothing ever moved --
		 * so leaving it in the drift list would show the operator a
		 * pending change that is not one.
		 */
		rcd_stale_forget(st, s->section, s->key);

		if (k->provider->enact) {
			s->owed_enact = true;
			add_enact(enacted, &enact_count, k->provider, was_armed);
		}
	}

	/*
	 * Last, and after every store is back. The enact is what the client
	 * lost its connection to, so it is also what gives it back, and it
	 * has to see the whole restored stanza rather than half of one.
	 */
	for (int i = 0; i < enact_count; i++) {
		if (enacted[i]->enact() != 0) {
			RSS_ERROR("guard: the old settings are back in the file but not in force");
			failed++;
			continue;
		}

		/* In force, so nothing is halfway back any more -- for every
		 * key of the store, since one call is what they share. */
		for (int j = 0; j < st->guard_count; j++) {
			const rcd_key_t *k = rcd_key_find(st->guard[j].section, st->guard[j].key);
			if (k && k->provider && k->provider->enact == enacted[i]->enact)
				st->guard[j].owed_enact = false;
		}
	}

	/* Stores back but not in force is still a camera on settings nobody
	 * confirmed, so it is a failed revert and not a noisy success. */
	if (failed) {
		guard_retry(st, failed);
		return;
	}

	st->guard_deadline_ms = 0;
	st->guard_window_sec = 0;
	st->guard_count = 0;
	st->guard_retries = 0;
	record_clear();
	rcd_stale_save(st);
}

void rcd_guard_tick(rcd_state_t *st, uint64_t now)
{
	if (!st->guard_deadline_ms)
		return;

	/*
	 * A wifi change that has been shown to work confirms itself. See
	 * RCD_GUARD_WIFI_DWELL_SEC for why the camera is allowed to answer
	 * this one, and why not immediately.
	 *
	 * Ahead of the deadline check so that a window which has just run out
	 * on a radio that is plainly associated ends as a confirmation rather
	 * than as a revert of something that worked.
	 */
	if (st->guard_wifi_moved && st->guard_wifi_ask_ms && now >= st->guard_wifi_ask_ms) {
		st->guard_wifi_ask_ms = now + RCD_GUARD_WIFI_POLL_MS;
		if (rcd_wifi_settled()) {
			RSS_INFO("guard: the radio is associated and addressed on the "
				 "configured network; confirming");
			rcd_guard_confirm(st);
			return;
		}
	}

	if (now < st->guard_deadline_ms)
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

	bool was_armed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "armed"));
	const cJSON *w = cJSON_GetObjectItemCaseSensitive(root, "window_sec");
	int win_sec = cJSON_IsNumber(w) ? (int)cJSON_GetNumberValue(w) : 0;

	st->guard_count = 0;
	const cJSON *item = NULL;
	cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "keys"))
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

	/*
	 * Held but never armed: the stores were written and nothing was ever
	 * enacted, so there is nothing in force to undo. Carried forward
	 * instead, so that an apply which comes later still has the state to
	 * return to -- which is the whole reason the snapshot is taken by
	 * `set` rather than by `apply`.
	 */
	if (!was_armed) {
		RSS_INFO("guard: %d key(s) staged and not applied; snapshot kept", st->guard_count);
		return;
	}

	int marker_win = 0;
	uint64_t deadline = armed_read(&marker_win);
	if (marker_win > 0)
		win_sec = marker_win;
	st->guard_window_sec = win_sec > 0 ? win_sec : RCD_GUARD_NAME_SEC;

	/*
	 * No deadline beside the snapshot. /run is a tmpfs, so it is gone
	 * because the camera rebooted -- and a camera that rebooted inside the
	 * window did not confirm, whatever the reason. Put it back.
	 */
	if (!deadline) {
		rcd_guard_revert(st, "the camera rebooted before anyone confirmed");
		return;
	}

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

	/* Anything to put back, armed or merely held: cancelling a staged
	 * change that was never applied is a revert too, and reporting it as
	 * "nothing happened" would be a lie about a file that just changed. */
	bool was = rcd_guard_held(st);
	rcd_guard_revert(st, "cancelled");

	/* And whether it actually went back, which is not the same question.
	 * A revert that could not write a store leaves a retry armed behind
	 * it, and a client told "reverted, nothing armed" would go away
	 * believing the camera is on the old settings while it is not. */
	bool retrying = st->guard_deadline_ms != 0;

	cJSON *r = rcd_ok();
	if (!r)
		return NULL;
	cJSON_AddBoolToObject(r, "reverted", was && !retrying);
	cJSON_AddBoolToObject(r, "armed", retrying);
	return r;
}
