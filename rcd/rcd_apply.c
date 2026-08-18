/*
 * rcd_apply.c -- see rcd_apply.h
 */

#include "rcd_apply.h"
#include "rcd.h"
#include "rcd_config.h"
#include "rcd_ipc.h"
#include "rcd_proto.h"
#include "rcd_schema.h"

#include <rss_common.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define POLL_STEP_MS 200

/* A daemon that has not torn down in this long is not going to. */
#define DOWN_WAIT_MS 5000

/* rvd's pipeline init dominates this: sensor, ISP and encoder from cold. */
#define UP_WAIT_MS 25000

/* ------------------------------------------------------------------ */
/* Waiting                                                             */
/* ------------------------------------------------------------------ */

/*
 * rvd binds its control socket before it initialises the pipeline, and fills
 * its stream table from the config early in that init -- so both an answer and
 * a populated `streams` array arrive seconds before MI is actually live. Only
 * `ready` means the pipeline is built.
 *
 * That distinction is the whole reason for waiting. Resuming audio against a
 * half-built pipeline is the failure this protocol exists to avoid, and every
 * cheaper signal reports success while it is still happening.
 */
static bool rvd_pipeline_up(void)
{
	cJSON *r = rcd_ask_json("rvd", "{\"cmd\":\"status\"}");
	if (!r)
		return false;
	bool up = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "ready"));
	cJSON_Delete(r);
	return up;
}

static bool daemon_ready(const char *daemon)
{
	return strcmp(daemon, "rvd") == 0 ? rvd_pipeline_up() : rcd_answers(daemon);
}

/*
 * Wait for the socket to stop answering before waiting for it to come back.
 * A daemon restarts by re-execing itself, so without this the poll below would
 * see the outgoing process and call the restart complete before it began.
 */
static void wait_down(const char *daemon)
{
	for (int waited = 0; waited < DOWN_WAIT_MS; waited += POLL_STEP_MS) {
		if (!rcd_answers(daemon))
			return;
		usleep(POLL_STEP_MS * 1000);
	}
	RSS_WARN("apply: %s still answering after %d ms, proceeding anyway", daemon, DOWN_WAIT_MS);
}

static bool wait_up(const char *daemon)
{
	for (int waited = 0; waited < UP_WAIT_MS; waited += POLL_STEP_MS) {
		if (daemon_ready(daemon))
			return true;
		usleep(POLL_STEP_MS * 1000);
	}
	return false;
}

/* ------------------------------------------------------------------ */
/* Quiesce and resume                                                  */
/* ------------------------------------------------------------------ */

/*
 * What rad was actually holding, so the resume puts back that and no more.
 * ao-enable on a camera with no speaker configured would not restore a state,
 * it would create one -- and rad records ao_enabled in its config as it goes,
 * so the invention would outlive the restart.
 */
typedef struct {
	bool ai;
	bool ao;
} quiesce_t;

static void quiesce_rad(quiesce_t *q)
{
	memset(q, 0, sizeof(*q));

	cJSON *r = rcd_ask_json("rad", "{\"cmd\":\"status\"}");
	if (!r)
		return; /* not running -- nothing holds MI, nothing to resume */

	bool ao_up = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "ao_enabled"));
	cJSON_Delete(r);

	q->ai = rcd_ask_ok("rad", "{\"cmd\":\"ai-disable\"}");
	if (ao_up)
		q->ao = rcd_ask_ok("rad", "{\"cmd\":\"ao-disable\"}");

	RSS_INFO("apply: rad quiesced (input %s, output %s)", q->ai ? "released" : "untouched",
		 q->ao ? "released" : "untouched");
}

static void resume_rad(const quiesce_t *q, char *err, size_t errsz)
{
	if (q->ai && !rcd_ask_ok("rad", "{\"cmd\":\"ai-enable\"}")) {
		snprintf(err, errsz, "rad did not resume audio input");
		return;
	}
	if (q->ao && !rcd_ask_ok("rad", "{\"cmd\":\"ao-enable\"}")) {
		/* rad cleared ao_enabled in its own config when it released
		 * the output, so a failure here is not only silent audio: the
		 * next save would make it permanent. */
		snprintf(err, errsz, "rad did not resume audio output");
		return;
	}
	if (q->ai || q->ao)
		RSS_INFO("apply: rad resumed");
}

/* ------------------------------------------------------------------ */
/* Restarting                                                          */
/* ------------------------------------------------------------------ */

static void note_error(rcd_state_t *st, const char *msg)
{
	rss_strlcpy(st->apply_error, msg, sizeof(st->apply_error));
	RSS_ERROR("apply: %s", msg);
}

/* Returns NULL on success, or the failure to report for this daemon. */
static const char *restart_plain(rcd_state_t *st, const char *daemon, char *msg, size_t msgsz)
{
	if (!rcd_ask_ok(daemon, "{\"cmd\":\"restart\"}")) {
		snprintf(msg, msgsz,
			 "%s refused to restart, its new settings are in the file "
			 "but not in effect",
			 daemon);
		note_error(st, msg);
		return msg;
	}

	wait_down(daemon);
	if (!wait_up(daemon)) {
		snprintf(msg, msgsz, "%s did not come back within %d s", daemon, UP_WAIT_MS / 1000);
		note_error(st, msg);
		return msg;
	}

	RSS_INFO("apply: %s restarted", daemon);
	return NULL;
}

/*
 * rvd, bracketed. Everything else on this camera that holds MI references has
 * to let go first, or it dies when rvd tears MI down.
 */
static const char *restart_rvd(rcd_state_t *st, char *msg, size_t msgsz)
{
	quiesce_t q;
	quiesce_rad(&q);

	bool ok = rcd_ask_ok("rvd", "{\"cmd\":\"restart\"}");
	if (ok) {
		wait_down("rvd");
		ok = wait_up("rvd");
	}

	/*
	 * Outside the success path on purpose. A resume skipped because the
	 * restart failed leaves audio dead indefinitely, and the operator has
	 * no way to tell from any other reading: every audio setting still
	 * reports the value it was configured with.
	 */
	char audio_err[160] = "";
	resume_rad(&q, audio_err, sizeof(audio_err));

	if (!ok) {
		snprintf(msg, msgsz, "rvd did not come back within %d s", UP_WAIT_MS / 1000);
		note_error(st, msg);
	} else {
		RSS_INFO("apply: rvd restarted");
	}

	/* Reported second so it wins the one slot: a camera whose video came
	 * back but whose audio did not is the state worth surfacing. */
	if (audio_err[0]) {
		note_error(st, audio_err);
		rss_strlcpy(msg, audio_err, msgsz);
		return msg;
	}
	return ok ? NULL : msg;
}

/* ------------------------------------------------------------------ */
/* The two verbs                                                       */
/* ------------------------------------------------------------------ */

/*
 * Which daemons a request names, or every one that is running behind when it
 * names none. An unknown name is refused rather than skipped: a caller that
 * misspells a daemon has asked for something that will not happen, and
 * answering "ok" to that is worse than answering at all.
 */
static cJSON *select_daemons(const rcd_state_t *st, const cJSON *root, bool want[RCD_D_COUNT],
			     bool default_stale)
{
	memset(want, 0, sizeof(bool) * RCD_D_COUNT);

	const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "daemons");
	if (!cJSON_IsArray(arr)) {
		if (!default_stale)
			return rcd_err(RCD_E_MALFORMED, "restart needs a 'daemons' array");
		memcpy(want, st->stale_daemon, sizeof(bool) * RCD_D_COUNT);
		return NULL;
	}

	const cJSON *n = NULL;
	int named = 0;
	cJSON_ArrayForEach(n, arr)
	{
		if (!cJSON_IsString(n))
			return rcd_err(RCD_E_MALFORMED, "'daemons' must hold names");
		rcd_daemon_t d = rcd_daemon_by_name(n->valuestring);
		if (d == RCD_D_COUNT)
			return rcd_err(RCD_E_UNKNOWN, "no such daemon");
		/* Scoping an apply cannot widen it: a daemon that is not
		 * behind has nothing to pick up, and restarting it would be an
		 * outage for no change. */
		if (default_stale && !st->stale_daemon[d])
			continue;
		want[d] = true;
		named++;
	}

	if (!named && !default_stale)
		return rcd_err(RCD_E_MALFORMED, "'daemons' names nothing");
	return NULL;
}

static void emit_plan(cJSON *out, const bool *want)
{
	cJSON *arr = cJSON_AddArrayToObject(out, "plan");
	for (int d = 0; arr && d < RCD_D_COUNT; d++) {
		if (!want[d])
			continue;
		cJSON *o = cJSON_CreateObject();
		if (!o)
			continue;
		cJSON_AddStringToObject(o, "daemon", rcd_daemon_name((rcd_daemon_t)d));
		cJSON_AddStringToObject(o, "impact",
					rcd_impact_name(rcd_daemon_impact((rcd_daemon_t)d)));
		cJSON_AddItemToArray(arr, o);
	}
}

/*
 * Run the restarts. rvd goes first, so the one restart that disturbs
 * everything else happens while the fewest other daemons are mid-restart.
 */
static void run(rcd_state_t *st, const bool *want, bool forget_stale, cJSON *results)
{
	char msg[192];

	for (int pass = 0; pass < 2; pass++) {
		for (int d = 0; d < RCD_D_COUNT; d++) {
			bool is_rvd = (d == RCD_D_RVD);
			if (!want[d] || is_rvd != (pass == 0))
				continue;

			const char *name = rcd_daemon_name((rcd_daemon_t)d);
			const char *fail = is_rvd ? restart_rvd(st, msg, sizeof(msg))
						  : restart_plain(st, name, msg, sizeof(msg));

			/* Cleared only when the daemon actually came back: one
			 * that refused is still running the old config, and
			 * saying otherwise would hide the very thing the
			 * operator needs to see. */
			if (!fail && forget_stale)
				rcd_stale_clear(st, (rcd_daemon_t)d);

			if (!results)
				continue;
			cJSON *o = cJSON_CreateObject();
			if (!o)
				continue;
			cJSON_AddStringToObject(o, "daemon", name);
			cJSON_AddStringToObject(o, "status", fail ? "error" : "ok");
			if (fail)
				cJSON_AddStringToObject(o, "reason", fail);
			cJSON_AddItemToArray(results, o);
		}
	}
}

static cJSON *do_restarts(rcd_state_t *st, const cJSON *root, bool default_stale)
{
	if (st->applying)
		return rcd_err(RCD_E_BUSY, "an apply is already running");

	bool want[RCD_D_COUNT];
	cJSON *refusal = select_daemons(st, root, want, default_stale);
	if (refusal)
		return refusal;

	cJSON *resp = rcd_ok();
	if (!resp)
		return NULL;

	int n = 0;
	for (int d = 0; d < RCD_D_COUNT; d++)
		n += want[d] ? 1 : 0;

	/* Nothing to do is a success, not an error: `apply` is meant to be
	 * safe to press twice. */
	if (n == 0) {
		cJSON_AddBoolToObject(resp, "restarted", false);
		rcd_config_report_stale(st, resp);
		return resp;
	}

	if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "dry_run"))) {
		cJSON_AddBoolToObject(resp, "dry_run", true);
		emit_plan(resp, want);
		return resp;
	}

	/*
	 * Owed saves first. A live edit still sitting in a daemon's memory is
	 * lost when that daemon re-execs, and an apply is exactly when that
	 * happens -- so the debounce is cut short here rather than allowed to
	 * expire into a process that no longer exists.
	 */
	rcd_save_flush(st);

	st->apply_error[0] = '\0';
	st->applying = true;

	cJSON *results = cJSON_AddArrayToObject(resp, "results");
	emit_plan(resp, want);
	run(st, want, default_stale, results);

	st->applying = false;
	rcd_stale_save(st);

	cJSON_AddBoolToObject(resp, "restarted", true);
	rcd_config_report_stale(st, resp);
	return resp;
}

cJSON *rcd_cmd_apply(rcd_state_t *st, const cJSON *root)
{
	return do_restarts(st, root, true);
}

cJSON *rcd_cmd_restart(rcd_state_t *st, const cJSON *root)
{
	return do_restarts(st, root, false);
}
