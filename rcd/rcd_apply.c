/*
 * rcd_apply.c -- see rcd_apply.h
 */

#include "rcd_apply.h"
#include "rcd.h"
#include "rcd_config.h"
#include "rcd_guard.h"
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
	cJSON *r = rcd_ask_json("rvd", "status");
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

static uint64_t now_ms(void)
{
	return (uint64_t)(rss_timestamp_us() / 1000);
}

/*
 * Poll until `probe` says yes or the budget is spent.
 *
 * Against a clock, not against a count of sleeps. The count is what this used
 * to do -- `for (waited = 0; waited < BUDGET; waited += POLL_STEP)` -- and it
 * charged the budget only for the sleeping, while each iteration also spent
 * however long the probe took. A probe here is an IPC round trip with a
 * timeout: RCD_PROBE_TIMEOUT_MS for a liveness check, RCD_CTRL_TIMEOUT_MS for
 * rvd's status. So a daemon that was listening but not answering cost 2.2 s an
 * iteration against a 0.2 s charge, and UP_WAIT_MS of 25 s took 125 iterations
 * and about 275 s -- eleven times the number in the message it then printed.
 *
 * That is not just an inaccurate log line. rcd's serve loop is single-threaded
 * and synchronous, so for as long as this runs the daemon answers nothing:
 * rcd_guard_tick does not run, and an armed network guard cannot revert on
 * time. A 90-second guard window and a four-minute wait is a camera that
 * strands itself at the exact moment the guard exists to prevent. Bounding the
 * wait to what it claims is what keeps the guard's promise true.
 *
 * `waited_ms` is the measured elapsed time, so callers report what actually
 * happened rather than the constant they were budgeted. The budget bounds when
 * a probe may *start*, so the real ceiling is one probe timeout beyond it --
 * there is no way to abandon a call already in flight, and pretending
 * otherwise is how the old arithmetic went wrong in the first place.
 *
 * Not static so the budget can be tested against a probe that is slow rather
 * than instant, which is the only shape in which the bug was visible.
 */
bool rcd_wait_until(bool (*probe)(const char *), const char *arg, unsigned int budget_ms,
		    unsigned int *waited_ms)
{
	uint64_t start = now_ms();
	uint64_t deadline = start + budget_ms;
	bool got = false;

	for (;;) {
		got = probe(arg);
		if (got || now_ms() >= deadline)
			break;
		usleep(POLL_STEP_MS * 1000);
	}

	if (waited_ms)
		*waited_ms = (unsigned int)(now_ms() - start);
	return got;
}

static bool stopped_answering(const char *daemon)
{
	return !rcd_answers(daemon);
}

/*
 * Wait for the socket to stop answering before waiting for it to come back.
 * A daemon restarts by re-execing itself, so without this the poll below would
 * see the outgoing process and call the restart complete before it began.
 */
static void wait_down(const char *daemon)
{
	unsigned int waited = 0;

	if (rcd_wait_until(stopped_answering, daemon, DOWN_WAIT_MS, &waited))
		return;

	RSS_WARN("apply: %s still answering after %u ms, proceeding anyway", daemon, waited);
}

static bool wait_up(const char *daemon, unsigned int *waited_ms)
{
	return rcd_wait_until(daemon_ready, daemon, UP_WAIT_MS, waited_ms);
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

	cJSON *r = rcd_ask_json("rad", "status");
	if (!r)
		return; /* not running -- nothing holds MI, nothing to resume */

	bool ao_up = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "ao_enabled"));
	cJSON_Delete(r);

	q->ai = rcd_ask_ok("rad", "ai-disable");
	if (ao_up)
		q->ao = rcd_ask_ok("rad", "ao-disable");

	RSS_INFO("apply: rad quiesced (input %s, output %s)", q->ai ? "released" : "untouched",
		 q->ao ? "released" : "untouched");
}

static void resume_rad(const quiesce_t *q, char *err, size_t errsz)
{
	if (q->ai && !rcd_ask_ok("rad", "ai-enable")) {
		snprintf(err, errsz, "rad did not resume audio input");
		return;
	}
	if (q->ao && !rcd_ask_ok("rad", "ao-enable")) {
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
	if (!rcd_ask_ok(daemon, "restart")) {
		snprintf(msg, msgsz,
			 "%s refused to restart, its new settings are in the file "
			 "but not in effect",
			 daemon);
		note_error(st, msg);
		return msg;
	}

	wait_down(daemon);

	unsigned int waited = 0;
	if (!wait_up(daemon, &waited)) {
		/* The measured wait, not the budget: they used to be the same
		 * number and were not the same quantity. */
		snprintf(msg, msgsz, "%s did not come back within %u s", daemon,
			 (waited + 999) / 1000);
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

	unsigned int waited = 0;
	bool ok = rcd_ask_ok("rvd", "restart");
	if (ok) {
		wait_down("rvd");
		ok = wait_up("rvd", &waited);
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
		snprintf(msg, msgsz, "rvd did not come back within %u s", (waited + 999) / 1000);
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

			/*
			 * A daemon that is not running has nothing stale in
			 * it: what was written is what it will read when it
			 * next starts, if it ever does. Skipped rather than
			 * failed -- rmr is absent from builds that do not ship
			 * recording, and an ordinary edit there must not raise
			 * an alarm about a daemon this image does not carry.
			 * `state` already clears stale on the same reasoning.
			 */
			if (rss_daemon_check(name) <= 0) {
				if (forget_stale)
					rcd_stale_clear(st, (rcd_daemon_t)d);
				if (results) {
					cJSON *o = cJSON_CreateObject();
					if (o) {
						cJSON_AddStringToObject(o, "daemon", name);
						cJSON_AddStringToObject(o, "status", "skipped");
						cJSON_AddStringToObject(o, "reason",
									"not running; it will "
									"read the file when it "
									"starts");
						cJSON_AddItemToArray(results, o);
					}
				}
				continue;
			}

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

/*
 * The settings that are not a daemon's: an address, a name. `set` wrote them
 * to their stores and this is where they take effect, which for a network
 * address means the connection carrying this request ends here.
 *
 * That is why the guard is armed first. Whatever happens to this reply -- and
 * on an address change nothing does, because the socket is gone before it can
 * be written -- the camera is now counting, and it will put the old settings
 * back unless somebody reaches it and says not to.
 */
static void enact_system(rcd_state_t *st, cJSON *results)
{
	const rcd_key_t *owed[RCD_STALE_MAX];
	int n = rcd_enact_owed(st, owed, RCD_STALE_MAX);
	if (n == 0)
		return;

	int window = 0;
	for (int i = 0; i < n; i++) {
		if (owed[i]->guard_sec > window)
			window = owed[i]->guard_sec;
	}
	if (window)
		rcd_guard_arm(st, window);

	/* One call per store, not one per key: five directives of one
	 * interface come into force together or the interface bounces five
	 * times. */
	const rcd_provider_t *done[RCD_STALE_MAX];
	int done_count = 0;

	/* The ones that actually came into force, which is a shorter list
	 * whenever anything went wrong -- and the only one the drift record
	 * may be cleared against. */
	const rcd_provider_t *ok[RCD_STALE_MAX];
	int ok_count = 0;

	for (int i = 0; i < n; i++) {
		const rcd_provider_t *p = owed[i]->provider;
		bool seen = false;
		for (int j = 0; j < done_count; j++)
			seen = seen || done[j]->enact == p->enact;
		if (seen)
			continue;
		done[done_count++] = p;

		int rc = p->enact();
		if (rc == 0) {
			ok[ok_count++] = p;
		} else {
			char msg[192];
			snprintf(msg, sizeof(msg),
				 "[%s] could not be put into force; it is stored but not in "
				 "effect",
				 owed[i]->section);
			note_error(st, msg);
		}
		if (!results)
			continue;
		cJSON *o = cJSON_CreateObject();
		if (!o)
			continue;
		cJSON_AddStringToObject(o, "daemon", "system");
		cJSON_AddStringToObject(o, "section", owed[i]->section);
		cJSON_AddStringToObject(o, "status", rc == 0 ? "ok" : "error");
		cJSON_AddItemToArray(results, o);
	}

	rcd_enact_done(st, ok, ok_count);
}

static cJSON *do_restarts(rcd_state_t *st, const cJSON *root, bool default_stale)
{
	/*
	 * Unreachable today, and kept deliberately.
	 *
	 * st->applying is raised and lowered inside one synchronous call, and
	 * rcd's serve loop is what dispatches here -- so the loop is somewhere
	 * inside this function for the whole time the flag is up, and cannot be
	 * dispatching a second apply. The same goes for the "applying" field
	 * `pending` publishes: every observer sees false.
	 *
	 * It stays because it is the guard an asynchronous apply would need,
	 * and an asynchronous apply is the natural answer to what is left of
	 * this: rcd_wait_until now bounds the wait to the budget it prints, but
	 * bounded is not the same as concurrent, and the daemon still answers
	 * nothing for up to UP_WAIT_MS per restarted daemon. Deleting this as
	 * dead code would remove the seatbelt before the crash it is for.
	 */
	if (st->applying)
		return rcd_err(RCD_E_BUSY, "an apply is already running");

	bool want[RCD_D_COUNT];
	cJSON *refusal = select_daemons(st, root, want, default_stale);
	if (refusal)
		return refusal;

	/*
	 * The camera's own settings go with an unscoped apply and with
	 * nothing else. `restart` is about daemons, and an apply narrowed to
	 * one daemon is an operator asking for that daemon -- neither is a
	 * request to change the address.
	 */
	const rcd_key_t *owed[RCD_STALE_MAX];
	bool do_enacts = default_stale &&
			 !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(root, "daemons")) &&
			 rcd_enact_owed(st, owed, RCD_STALE_MAX) > 0;

	cJSON *resp = rcd_ok();
	if (!resp)
		return NULL;

	int n = 0;
	for (int d = 0; d < RCD_D_COUNT; d++)
		n += want[d] ? 1 : 0;

	/* Nothing to do is a success, not an error: `apply` is meant to be
	 * safe to press twice. */
	if (n == 0 && !do_enacts) {
		cJSON_AddBoolToObject(resp, "restarted", false);
		rcd_config_report_stale(st, resp);
		return resp;
	}

	if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "dry_run"))) {
		cJSON_AddBoolToObject(resp, "dry_run", true);
		emit_plan(resp, want);
		cJSON_AddBoolToObject(resp, "system", do_enacts);
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

	/*
	 * Last of all. Everything else has already been restarted by the time
	 * the network goes, so a client that loses the camera here loses it
	 * having had the rest of its apply carried out.
	 */
	if (do_enacts)
		enact_system(st, results);

	st->applying = false;
	rcd_stale_save(st);

	cJSON_AddBoolToObject(resp, "restarted", true);
	rcd_config_report_stale(st, resp);
	rcd_guard_report(st, resp);
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
