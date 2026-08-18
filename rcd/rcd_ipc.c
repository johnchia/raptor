/*
 * rcd_ipc.c -- see rcd_ipc.h
 */

#include "rcd_ipc.h"

#include <rss_common.h>
#include <rss_ipc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int rcd_ask(const char *daemon, const char *req, char *resp, size_t respsz, int timeout_ms)
{
	char sock[128];
	snprintf(sock, sizeof(sock), RSS_SOCK_FMT, daemon);
	return rss_ctrl_send_command(sock, req, resp, (int)respsz, (uint32_t)timeout_ms);
}

bool rcd_ask_ok_err(const char *daemon, const char *req, char *err, size_t errsz)
{
	char resp[512] = "";
	if (err && errsz)
		err[0] = '\0';

	if (rcd_ask(daemon, req, resp, sizeof(resp), RCD_CTRL_TIMEOUT_MS) < 0) {
		if (err && errsz)
			snprintf(err, errsz, "%s is not running or did not answer", daemon);
		return false;
	}

	cJSON *r = cJSON_Parse(resp);
	if (!r)
		return true; /* a bare string is an answer, not a refusal */

	const cJSON *s = cJSON_GetObjectItemCaseSensitive(r, "status");
	bool ok = !cJSON_IsString(s) || !s->valuestring || strcmp(s->valuestring, "ok") == 0;
	if (!ok && err && errsz) {
		const cJSON *e = cJSON_GetObjectItemCaseSensitive(r, "error");
		if (!cJSON_IsString(e))
			e = cJSON_GetObjectItemCaseSensitive(r, "reason");
		snprintf(err, errsz, "%s: %s", daemon,
			 cJSON_IsString(e) && e->valuestring ? e->valuestring : s->valuestring);
	}
	cJSON_Delete(r);
	return ok;
}

bool rcd_ask_ok(const char *daemon, const char *req)
{
	return rcd_ask_ok_err(daemon, req, NULL, 0);
}

cJSON *rcd_ask_json(const char *daemon, const char *req)
{
	char sock[128];
	snprintf(sock, sizeof(sock), RSS_SOCK_FMT, daemon);

	/*
	 * Allocated rather than into a buffer sized by guess: these replies are
	 * parsed, and a fixed buffer that a growing stream table outgrows would
	 * truncate the JSON into something unparseable without saying so.
	 */
	char *resp = NULL;
	if (rss_ctrl_send_command_alloc(sock, req, &resp, RCD_CTRL_TIMEOUT_MS) < 0)
		return NULL;

	cJSON *root = cJSON_Parse(resp);
	free(resp);
	return root;
}

bool rcd_answers(const char *daemon)
{
	char resp[256];
	return rcd_ask(daemon, "{\"cmd\":\"status\"}", resp, sizeof(resp), RCD_PROBE_TIMEOUT_MS) >=
	       0;
}
