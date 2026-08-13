/*
 * rmq_restart.c -- Applying config writes that need a daemon restart
 */

#include "rmq_restart.h"
#include "rmq.h"

#include <rss_common.h>
#include <rss_ipc.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <cJSON.h>

#define CTRL_TIMEOUT_MS 2000

/* A liveness probe answers at once or not at all, so it waits far less than a
 * command does — this one runs in a loop. */
#define PROBE_TIMEOUT_MS 400
#define POLL_STEP_MS	 200

/* A daemon that has not torn down in this long is not going to. */
#define DOWN_WAIT_MS 5000

/* rvd's pipeline init dominates this: sensor, ISP and encoder from cold. */
#define UP_WAIT_MS 25000

/* ------------------------------------------------------------------ */
/* Talking to daemons                                                  */
/* ------------------------------------------------------------------ */

static uint64_t now_ms(void)
{
	return (uint64_t)(rss_timestamp_us() / 1000);
}

static int ask(const char *daemon, const char *req, char *resp, size_t respsz, int timeout_ms)
{
	char sock[128];
	snprintf(sock, sizeof(sock), RSS_SOCK_FMT, daemon);
	return rss_ctrl_send_command(sock, req, resp, (int)respsz, timeout_ms);
}

/*
 * A daemon answers either a JSON object carrying a status or a bare string, so
 * only an explicit non-ok status counts as a failure. `restart` itself answers
 * the bare string "restarting", which is why an unparseable reply is a success
 * rather than an error.
 */
static bool ask_ok(const char *daemon, const char *req)
{
	char resp[512] = "";
	if (ask(daemon, req, resp, sizeof(resp), CTRL_TIMEOUT_MS) < 0)
		return false;

	cJSON *r = cJSON_Parse(resp);
	if (!r)
		return true;

	const cJSON *s = cJSON_GetObjectItemCaseSensitive(r, "status");
	bool ok = !cJSON_IsString(s) || !s->valuestring || strcmp(s->valuestring, "ok") == 0;
	cJSON_Delete(r);
	return ok;
}

static bool answers(const char *daemon)
{
	char resp[256];
	return ask(daemon, "{\"cmd\":\"status\"}", resp, sizeof(resp), PROBE_TIMEOUT_MS) >= 0;
}

/*
 * rvd binds its control socket before it initialises the pipeline, and fills
 * its stream table from the config early in that init — so both an answer and
 * a populated `streams` array arrive seconds before MI is actually live. Only
 * `ready` means the pipeline is built.
 *
 * That distinction is the whole reason for waiting. Resuming audio against a
 * half-built pipeline is the failure this protocol exists to avoid, and every
 * cheaper signal reports success while it is still happening.
 */
static bool rvd_pipeline_up(void)
{
	char resp[4096];
	if (ask("rvd", "{\"cmd\":\"status\"}", resp, sizeof(resp), PROBE_TIMEOUT_MS) < 0)
		return false;

	cJSON *r = cJSON_Parse(resp);
	if (!r)
		return false;

	bool up = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "ready"));
	cJSON_Delete(r);
	return up;
}

static bool daemon_ready(const char *daemon)
{
	return strcmp(daemon, "rvd") == 0 ? rvd_pipeline_up() : answers(daemon);
}

/*
 * Wait for the socket to stop answering before waiting for it to come back.
 * A daemon restarts by re-execing itself, so without this the poll below would
 * see the outgoing process and call the restart complete before it began.
 */
static void wait_down(const char *daemon)
{
	for (int waited = 0; waited < DOWN_WAIT_MS; waited += POLL_STEP_MS) {
		if (!answers(daemon))
			return;
		usleep(POLL_STEP_MS * 1000);
	}
	RSS_WARN("restart: %s still answering after %d ms, proceeding anyway", daemon,
		 DOWN_WAIT_MS);
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
 * it would create one — and rad records ao_enabled in its config as it goes,
 * so the invention would outlive the restart.
 */
typedef struct {
	bool ai;
	bool ao;
} quiesce_t;

static void quiesce_rad(quiesce_t *q)
{
	memset(q, 0, sizeof(*q));

	char resp[2048] = "";
	if (ask("rad", "{\"cmd\":\"status\"}", resp, sizeof(resp), CTRL_TIMEOUT_MS) < 0)
		return; /* not running — nothing holds MI, nothing to resume */

	cJSON *r = cJSON_Parse(resp);
	bool ao_up = r && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "ao_enabled"));
	cJSON_Delete(r);

	q->ai = ask_ok("rad", "{\"cmd\":\"ai-disable\"}");
	if (ao_up)
		q->ao = ask_ok("rad", "{\"cmd\":\"ao-disable\"}");

	RSS_INFO("restart: rad quiesced (input %s, output %s)", q->ai ? "released" : "untouched",
		 q->ao ? "released" : "untouched");
}

static void resume_rad(const quiesce_t *q, char *err, size_t errsz)
{
	if (q->ai && !ask_ok("rad", "{\"cmd\":\"ai-enable\"}")) {
		snprintf(err, errsz, "rad did not resume audio input");
		return;
	}
	if (q->ao && !ask_ok("rad", "{\"cmd\":\"ao-enable\"}")) {
		/* rad cleared ao_enabled in its own config when it released
		 * the output, so a failure here is not only silent audio: the
		 * next save would make it permanent. */
		snprintf(err, errsz, "rad did not resume audio output");
		return;
	}
	if (q->ai || q->ao)
		RSS_INFO("restart: rad resumed");
}

/* ------------------------------------------------------------------ */
/* Restarting                                                          */
/* ------------------------------------------------------------------ */

static void note_error(rmq_state_t *st, const char *msg)
{
	rss_strlcpy(st->restart_error, msg, sizeof(st->restart_error));
	RSS_ERROR("restart: %s", msg);
}

static void restart_plain(rmq_state_t *st, const char *daemon)
{
	if (!ask_ok(daemon, "{\"cmd\":\"restart\"}")) {
		char msg[160];
		snprintf(msg, sizeof(msg),
			 "%s refused to restart, its new settings are in the "
			 "file but not in effect",
			 daemon);
		note_error(st, msg);
		return;
	}

	wait_down(daemon);
	if (!wait_up(daemon)) {
		char msg[160];
		snprintf(msg, sizeof(msg), "%s did not come back within %d s", daemon,
			 UP_WAIT_MS / 1000);
		note_error(st, msg);
		return;
	}
	RSS_INFO("restart: %s restarted", daemon);
}

/*
 * rvd, bracketed. Everything else on this camera that holds MI references has
 * to let go first, or it dies when rvd tears MI down.
 */
static void restart_rvd(rmq_state_t *st)
{
	quiesce_t q;
	quiesce_rad(&q);

	bool ok = ask_ok("rvd", "{\"cmd\":\"restart\"}");
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
	char err[160] = "";
	resume_rad(&q, err, sizeof(err));

	if (!ok) {
		char msg[160];
		snprintf(msg, sizeof(msg), "rvd did not come back within %d s", UP_WAIT_MS / 1000);
		note_error(st, msg);
	} else {
		RSS_INFO("restart: rvd restarted");
	}

	/* Reported second so it wins the one slot: a camera whose video came
	 * back but whose audio did not is the state worth surfacing. */
	if (err[0])
		note_error(st, err);
}

/* ------------------------------------------------------------------ */
/* Staging                                                             */
/* ------------------------------------------------------------------ */

static void clear_staged(rmq_state_t *st)
{
	st->cfg_write_count = 0;
	st->restart_due_ms = 0;
	st->restart_first_ms = 0;
	memset(st->restart_owed, 0, sizeof(st->restart_owed));
}

void rmq_restart_stage(rmq_state_t *st, const rmq_cfg_write_t *w, rmq_daemon_t owner)
{
	rmq_cfg_write_t *slot = NULL;
	for (int i = 0; i < st->cfg_write_count; i++) {
		if (strcmp(st->cfg_writes[i].section, w->section) == 0 &&
		    strcmp(st->cfg_writes[i].key, w->key) == 0) {
			slot = &st->cfg_writes[i];
			break;
		}
	}

	if (!slot) {
		/* Full. Applying early beats dropping an edit that was already
		 * accepted and answered for. */
		if (st->cfg_write_count >= RMQ_CFG_PENDING_MAX)
			rmq_restart_apply(st);
		slot = &st->cfg_writes[st->cfg_write_count++];
	}
	*slot = *w;

	if (owner < RMQ_D_COUNT)
		st->restart_owed[owner] = true;

	uint64_t now = now_ms();
	if (!st->restart_due_ms)
		st->restart_first_ms = now;
	st->restart_due_ms = now + (uint64_t)st->restart_debounce_ms;

	/* Cap the total wait, so an edit still lands even if the burst never
	 * stops arriving. */
	uint64_t ceiling = st->restart_first_ms + RMQ_RESTART_MAX_DELAY_MS;
	if (st->restart_due_ms > ceiling)
		st->restart_due_ms = ceiling;
}

bool rmq_restart_due(const rmq_state_t *st, uint64_t now)
{
	return st->restart_due_ms && now >= st->restart_due_ms;
}

/* ------------------------------------------------------------------ */
/* Applying                                                            */
/* ------------------------------------------------------------------ */

/*
 * A fresh load rather than the bridge's own config. rss_config_save writes
 * only the keys dirtied on the copy handed to it, so starting from a clean
 * read means exactly the staged edits reach the file — the bridge's own [mqtt]
 * settings are not part of the write, and neither is anything a daemon has
 * changed in its own memory and not yet saved.
 */
static int write_config(rmq_state_t *st)
{
	rss_config_t *cfg = rss_config_load(st->config_path);
	if (!cfg)
		return -1;

	for (int i = 0; i < st->cfg_write_count; i++) {
		rss_config_set_str(cfg, st->cfg_writes[i].section, st->cfg_writes[i].key,
				   st->cfg_writes[i].value);
		RSS_INFO("restart: [%s] %s = %s", st->cfg_writes[i].section, st->cfg_writes[i].key,
			 st->cfg_writes[i].value);
	}

	int ret = rss_config_save(cfg, st->config_path);
	rss_config_free(cfg);
	return ret;
}

void rmq_restart_flush_writes(rmq_state_t *st)
{
	if (!st->restart_due_ms)
		return;

	if (write_config(st) != 0)
		RSS_ERROR("restart: could not write %s, %d edits lost", st->config_path,
			  st->cfg_write_count);
	else
		RSS_INFO("restart: %d edits written to %s, daemons left running",
			 st->cfg_write_count, st->config_path);

	clear_staged(st);
}

void rmq_restart_apply(rmq_state_t *st)
{
	if (!st->restart_due_ms)
		return;

	st->restart_error[0] = '\0';

	if (write_config(st) != 0) {
		char msg[160];
		snprintf(msg, sizeof(msg), "could not write %s, %d edits dropped", st->config_path,
			 st->cfg_write_count);
		note_error(st, msg);
		/* Nothing reached the file, so restarting would only cost an
		 * outage and change nothing. */
		clear_staged(st);
		return;
	}

	/* Taken before the restarts: a daemon coming back may stage nothing,
	 * but the flags are cleared as they are consumed either way. */
	bool owed[RMQ_D_COUNT];
	memcpy(owed, st->restart_owed, sizeof(owed));
	clear_staged(st);

	/* rvd first, so the one restart that disturbs everything else happens
	 * while the fewest other daemons are mid-restart. */
	if (owed[RMQ_D_RVD])
		restart_rvd(st);

	for (int i = 0; i < RMQ_D_COUNT; i++) {
		if (i != RMQ_D_RVD && owed[i])
			restart_plain(st, rmq_daemon_name((rmq_daemon_t)i));
	}
}

void rmq_restart_report(const rmq_state_t *st, cJSON *state)
{
	cJSON *o = cJSON_AddObjectToObject(state, "restart");
	if (!o)
		return;

	cJSON_AddBoolToObject(o, "pending", st->restart_due_ms != 0);
	cJSON_AddNumberToObject(o, "pending_edits", st->cfg_write_count);

	/*
	 * The last failure, carried until the next apply clears it. A resume
	 * that failed is the one state no other entity reveals: audio simply
	 * stops while every audio setting still reads correctly.
	 */
	if (st->restart_error[0])
		cJSON_AddStringToObject(o, "error", st->restart_error);
}
