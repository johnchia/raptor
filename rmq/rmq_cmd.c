/*
 * rmq_cmd.c -- see rmq_cmd.h
 */

#include "rmq_cmd.h"
#include "rmq.h"
#include "rmq_rcd.h"

#include <rss_common.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cJSON.h>

int rmq_cmd_subscribe(rmq_state_t *st)
{
	if (!st->commands_enabled)
		return 0;
	/* QoS 1: a command silently dropped by the broker is worse than one
	 * delivered twice, and every command here is idempotent. */
	return rmq_mqtt_subscribe(st->mqtt, st->topic_cmd, 1);
}

static void publish_result(rmq_state_t *st, const char *cmd, const char *nonce, const char *err,
			   cJSON *result)
{
	cJSON *root = cJSON_CreateObject();
	if (!root) {
		cJSON_Delete(result);
		return;
	}

	cJSON_AddStringToObject(root, "status", err ? "error" : "ok");
	if (cmd)
		cJSON_AddStringToObject(root, "cmd", cmd);
	if (nonce)
		cJSON_AddStringToObject(root, "nonce", nonce);
	if (err)
		cJSON_AddStringToObject(root, "error", err);
	if (result)
		cJSON_AddItemToObject(root, "result", result);

	char *payload = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!payload)
		return;

	/* Not retained. A result describes one moment; replaying it to
	 * whoever subscribes next would present old news as current. */
	rmq_mqtt_publish(st->mqtt, st->topic_result, payload, strlen(payload), 1, false);
	free(payload);
}

/* ------------------------------------------------------------------ */
/* The three commands rcd does not own                                 */
/* ------------------------------------------------------------------ */

/*
 * Reboot the camera.
 *
 * Answered before it happens, because afterwards there is nothing left to
 * answer with -- and the answer is the only evidence the button did anything.
 * The status topic goes offline first for the same reason: the Last Will would
 * eventually say so, but only after the broker's keepalive times out, which
 * leaves every entity looking live for half a minute while the camera is
 * already down.
 */
static void do_reboot(rmq_state_t *st, const char *cmd_name, const char *nonce)
{
	cJSON *r = cJSON_CreateObject();
	if (r)
		cJSON_AddStringToObject(r, "status", "rebooting");
	publish_result(st, cmd_name, nonce, NULL, r);

	rmq_mqtt_publish(st->mqtt, st->topic_status, RMQ_STATUS_OFFLINE, strlen(RMQ_STATUS_OFFLINE),
			 1, true);
	rmq_mqtt_loop(st->mqtt, 200);

	RSS_WARN("reboot: requested over MQTT");

	/*
	 * Through init rather than the reboot(2) syscall, so the init scripts
	 * stop the daemons in their own order and the flash is unmounted
	 * cleanly. /etc is an overlay over NOR; pulling the rug is how a config
	 * file ends up half written.
	 */
	sync();
	if (system("/sbin/reboot") != 0)
		RSS_WARN("reboot: /sbin/reboot did not run");
}

static void do_snapshot(rmq_state_t *st, const char *cmd_name, const char *nonce)
{
	if (!st->snapshot_enabled) {
		publish_result(st, cmd_name, nonce,
			       "snapshots are off -- set [mqtt] snapshot = true", NULL);
		return;
	}

	int rc = rmq_snapshot_publish(st);
	cJSON *r = cJSON_CreateObject();
	if (r)
		cJSON_AddStringToObject(r, "status", rc == 0 ? "ok" : "no url");
	publish_result(st, cmd_name, nonce, rc == 0 ? NULL : "no picture URL -- is rhd running?",
		       r);
}

/* ------------------------------------------------------------------ */
/* Everything else: hand it to rcd                                     */
/* ------------------------------------------------------------------ */

/*
 * Whether a reply says a daemon is now running behind.
 *
 * `set` answers with what it did and what that left owed, and the difference
 * matters here because Home Assistant has no way to show it: a dropdown that
 * has moved looks applied whether it is or not.
 */
static bool left_stale(const cJSON *resp)
{
	const cJSON *stale = cJSON_GetObjectItemCaseSensitive(resp, "stale");
	return cJSON_IsArray(stale) && cJSON_GetArraySize(stale) > 0;
}

static void relay(rmq_state_t *st, const char *json, const char *cmd_name, const char *nonce)
{
	cJSON *resp = rmq_rcd_call(json);
	if (!resp) {
		RSS_WARN("cmd: rcd did not answer '%s'", cmd_name ? cmd_name : "(none)");
		publish_result(st, cmd_name, nonce, "rcd is not running or did not answer", NULL);
		return;
	}

	const cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
	bool ok = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;

	if (!ok) {
		const cJSON *why = cJSON_GetObjectItemCaseSensitive(resp, "reason");
		char err[192];
		rss_strlcpy(err, cJSON_IsString(why) ? why->valuestring : "refused", sizeof(err));
		/* Refusals are logged, not just answered: this topic is a whole
		 * management surface, so a rejected command is the one thing
		 * worth being able to find afterwards. */
		RSS_WARN("cmd: refused '%s': %s", cmd_name ? cmd_name : "(none)", err);
		publish_result(st, cmd_name, nonce, err, resp);
		return;
	}

	/*
	 * The edit is saved and the daemon that reads it has not restarted.
	 * Whether to enact that is deliberately not this bridge's decision by
	 * default -- restarting rvd stops capture for tens of seconds, and
	 * nobody moving a dropdown in a dashboard asked for that. The Apply
	 * button exists to ask for it, and auto_apply is for installations
	 * that would rather not be asked.
	 */
	/*
	 * The bridge's copy of rhd's credential, refreshed from the file rcd
	 * just wrote. rmq does not re-read its config, so without this the
	 * picture URL would keep carrying the password that was replaced a
	 * moment ago -- and the one moment it has to be right is this one.
	 * Cheap enough to do after any accepted write rather than trying to
	 * work out which writes could have touched it.
	 */
	if (cJSON_GetObjectItemCaseSensitive(resp, "results")) {
		rss_config_t *cfg = rss_config_load(st->config_path);
		if (cfg) {
			rmq_http_creds(st, cfg);
			rss_config_free(cfg);
		}
	}

	if (st->auto_apply && left_stale(resp)) {
		RSS_INFO("cmd: auto_apply is on, enacting the saved edits");
		cJSON *applied = rmq_rcd_cmd("apply");
		if (applied)
			cJSON_AddItemToObject(resp, "applied", applied);
	}

	publish_result(st, cmd_name, nonce, NULL, resp);
}

/* ------------------------------------------------------------------ */

void rmq_cmd_handle(rmq_state_t *st, const char *topic, const uint8_t *payload, size_t len)
{
	(void)topic;

	if (len == 0 || len > RMQ_PAYLOAD_MAX) {
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

	cJSON *root = cJSON_Parse(json);

	/*
	 * The nonce and the command name are taken from the document rather
	 * than from anything downstream, so a refused command still comes back
	 * tagged. Without that, a sender waiting on its own nonce cannot tell
	 * a rejection from a timeout.
	 */
	char nonce_buf[RMQ_NONCE_MAX + 1] = "";
	char cmd_buf[64] = "";
	if (root) {
		const cJSON *n = cJSON_GetObjectItemCaseSensitive(root, "nonce");
		if (cJSON_IsString(n) && n->valuestring)
			rss_strlcpy(nonce_buf, n->valuestring, sizeof(nonce_buf));
		const cJSON *c = cJSON_GetObjectItemCaseSensitive(root, "cmd");
		if (cJSON_IsString(c) && c->valuestring)
			rss_strlcpy(cmd_buf, c->valuestring, sizeof(cmd_buf));
	}

	/* Absent rather than empty, so the result carries no key at all when
	 * the payload named neither. */
	const char *nonce = nonce_buf[0] ? nonce_buf : NULL;
	const char *cmd_name = cmd_buf[0] ? cmd_buf : NULL;

	if (!root || !cmd_name) {
		RSS_WARN("cmd: refused: %s", root ? "missing 'cmd'" : "payload is not JSON");
		publish_result(st, cmd_name, nonce, root ? "missing 'cmd'" : "payload is not JSON",
			       NULL);
		cJSON_Delete(root);
		free(json);
		return;
	}

	if (strcmp(cmd_name, "reboot") == 0)
		do_reboot(st, cmd_name, nonce);
	else if (strcmp(cmd_name, "snapshot") == 0)
		do_snapshot(st, cmd_name, nonce);
	else
		relay(st, json, cmd_name, nonce);

	cJSON_Delete(root);
	free(json);
}
