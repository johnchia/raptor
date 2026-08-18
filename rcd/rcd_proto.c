/*
 * rcd_proto.c -- see rcd_proto.h
 */

#include "rcd_proto.h"
#include "rcd.h"
#include "rcd_apply.h"
#include "rcd_config.h"
#include "rcd_guard.h"
#include "rcd_ipc.h"
#include "rcd_schema.h"
#include "rcd_state.h"

#include <rss_common.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* A request is a form's worth of edits at most; anything approaching this is
 * not one. */
#define RCD_REQUEST_MAX 8192

cJSON *rcd_ok(void)
{
	cJSON *r = cJSON_CreateObject();
	if (!r)
		return NULL;
	cJSON_AddNumberToObject(r, "api", RCD_API_VERSION);
	cJSON_AddStringToObject(r, "status", "ok");
	return r;
}

cJSON *rcd_err(const char *code, const char *reason)
{
	cJSON *r = cJSON_CreateObject();
	if (!r)
		return NULL;
	cJSON_AddNumberToObject(r, "api", RCD_API_VERSION);
	cJSON_AddStringToObject(r, "status", "error");
	cJSON_AddStringToObject(r, "code", code);
	cJSON_AddStringToObject(r, "reason", reason ? reason : "");
	return r;
}

void rcd_err_where(cJSON *e, const char *section, const char *key)
{
	if (!e)
		return;
	if (section)
		cJSON_AddStringToObject(e, "section", section);
	if (key)
		cJSON_AddStringToObject(e, "key", key);
}

/* ------------------------------------------------------------------ */
/* Commands that need no module of their own                           */
/* ------------------------------------------------------------------ */

/*
 * What this build is and what it is talking to.
 *
 * A client's first call: it learns the protocol version before it depends on
 * anything, and which daemons exist at all -- installed and running are
 * different questions, and a control for a daemon this image does not carry
 * should not be drawn rather than drawn and broken.
 */
static cJSON *cmd_hello(rcd_state_t *st)
{
	cJSON *r = rcd_ok();
	if (!r)
		return NULL;

	cJSON_AddStringToObject(r, "daemon", "rcd");
	/* Weak symbols: a build that did not generate rss_build_info.o links
	 * without them, and the field is simply absent rather than a crash. */
	if (&rss_build_hash && rss_build_hash)
		cJSON_AddStringToObject(r, "build", rss_build_hash);
	if (&rss_build_platform && rss_build_platform)
		cJSON_AddStringToObject(r, "platform", rss_build_platform);
	cJSON_AddStringToObject(r, "config", st->config_path);

	cJSON *d = cJSON_AddObjectToObject(r, "daemons");
	for (int i = 0; d && i < RCD_D_COUNT; i++) {
		const char *name = rcd_daemon_name((rcd_daemon_t)i);
		cJSON *o = cJSON_AddObjectToObject(d, name);
		if (!o)
			continue;

		char path[64];
		snprintf(path, sizeof(path), "/usr/bin/%s", name);
		cJSON_AddBoolToObject(o, "installed", access(path, X_OK) == 0);
		cJSON_AddBoolToObject(o, "running", rss_daemon_check(name) > 0);
		cJSON_AddStringToObject(o, "impact",
					rcd_impact_name(rcd_daemon_impact((rcd_daemon_t)i)));
	}

	return r;
}

/*
 * Mark the keys this camera does not physically have. Done here rather than in
 * the table because the table is the same on every platform and this answer
 * comes from a running daemon; a client hides them, and one that ignores the
 * field is no worse off than before.
 */
static void mark_availability(rcd_state_t *st, cJSON *r)
{
	cJSON *keys = cJSON_GetObjectItemCaseSensitive(r, "keys");
	cJSON *k = NULL;
	cJSON_ArrayForEach(k, keys)
	{
		const cJSON *sec = cJSON_GetObjectItemCaseSensitive(k, "section");
		const cJSON *key = cJSON_GetObjectItemCaseSensitive(k, "key");
		if (!cJSON_IsString(sec) || !cJSON_IsString(key))
			continue;
		const rcd_key_t *e = rcd_key_find(sec->valuestring, key->valuestring);
		if (e && !rcd_key_available(st, e))
			cJSON_AddBoolToObject(k, "available", false);
	}
}

static cJSON *cmd_schema(rcd_state_t *st, const cJSON *root)
{
	cJSON *r = rcd_ok();
	if (!r)
		return NULL;

	const cJSON *sec = cJSON_GetObjectItemCaseSensitive(root, "section");
	const char *filter = cJSON_IsString(sec) ? sec->valuestring : NULL;

	/* Bumped when an entry changes meaning, so a client that caches the
	 * table can tell that its copy is stale without diffing it. */
	cJSON_AddNumberToObject(r, "rev", RCD_API_VERSION);
	rcd_schema_emit(r, filter);
	mark_availability(st, r);
	return r;
}

/* What is owed, and what paying it would cost. Deliberately cheap: no daemon
 * is asked anything, so a client may poll this between edits. */
static cJSON *cmd_pending(rcd_state_t *st)
{
	cJSON *r = rcd_ok();
	if (!r)
		return NULL;
	cJSON_AddBoolToObject(r, "save_pending", st->save_due_ms != 0);
	cJSON_AddBoolToObject(r, "applying", st->applying);
	rcd_config_report_stale(st, r);
	/* The cheap poll is where a client watching a countdown looks, and a
	 * client that has just reconnected is exactly that client. */
	rcd_guard_report(st, r);
	return r;
}

static cJSON *cmd_flush(rcd_state_t *st)
{
	rcd_save_flush(st);
	return rcd_ok();
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

static cJSON *dispatch(rcd_state_t *st, const char *name, const cJSON *root)
{
	if (strcmp(name, "hello") == 0 || strcmp(name, "status") == 0)
		return cmd_hello(st);
	if (strcmp(name, "schema") == 0)
		return cmd_schema(st, root);
	if (strcmp(name, "get") == 0)
		return rcd_cmd_get(st, root);
	if (strcmp(name, "set") == 0)
		return rcd_cmd_set(st, root);
	if (strcmp(name, "credentials") == 0)
		return rcd_cmd_credentials(st, root);
	if (strcmp(name, "action") == 0)
		return rcd_cmd_action(st, root);
	if (strcmp(name, "apply") == 0)
		return rcd_cmd_apply(st, root);
	if (strcmp(name, "restart") == 0)
		return rcd_cmd_restart(st, root);
	if (strcmp(name, "confirm") == 0)
		return rcd_cmd_confirm(st, root);
	if (strcmp(name, "cancel") == 0)
		return rcd_cmd_cancel(st, root);
	if (strcmp(name, "state") == 0)
		return rcd_cmd_state(st, root);
	if (strcmp(name, "pending") == 0)
		return cmd_pending(st);
	if (strcmp(name, "flush") == 0)
		return cmd_flush(st);

	/* Deny by default, and name nothing else: a refusal that lists the
	 * alternatives is a directory of what would have worked. */
	return rcd_err(RCD_E_UNKNOWN, "no such command");
}

int rcd_handle(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	rcd_state_t *st = userdata;

	if (!cmd_json || strlen(cmd_json) > RCD_REQUEST_MAX) {
		cJSON *e = rcd_err(RCD_E_MALFORMED, "request empty or too large");
		int n = e && cJSON_PrintPreallocated(e, resp_buf, resp_buf_size, 0)
				? (int)strlen(resp_buf)
				: 0;
		cJSON_Delete(e);
		return n;
	}

	cJSON *root = cJSON_Parse(cmd_json);
	cJSON *resp = NULL;

	if (!root) {
		resp = rcd_err(RCD_E_MALFORMED, "request is not JSON");
	} else {
		const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
		if (!cJSON_IsString(cmd) || !cmd->valuestring)
			resp = rcd_err(RCD_E_MALFORMED, "missing 'cmd'");
		else
			resp = dispatch(st, cmd->valuestring, root);
	}

	cJSON_Delete(root);

	if (!resp) {
		resp_buf[0] = '\0';
		return 0;
	}

	/*
	 * The one place a reply can still be lost. rss_ctrl hands us 64 KB and
	 * the schema is the only thing that comes close, but a truncated reply
	 * would be invalid JSON with nothing to say it was cut -- so it is
	 * replaced by an error that says exactly that.
	 */
	if (!cJSON_PrintPreallocated(resp, resp_buf, resp_buf_size, 0)) {
		cJSON_Delete(resp);
		resp = rcd_err(RCD_E_IO, "the reply did not fit; ask for one section at a time");
		if (!resp || !cJSON_PrintPreallocated(resp, resp_buf, resp_buf_size, 0))
			resp_buf[0] = '\0';
	}

	cJSON_Delete(resp);
	return (int)strlen(resp_buf);
}
