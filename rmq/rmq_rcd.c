/*
 * rmq_rcd.c -- see rmq_rcd.h
 */

#include "rmq_rcd.h"

#include <rss_common.h>
#include <rss_ipc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *rcd_sock(void)
{
	static char sock[64];
	if (!sock[0])
		snprintf(sock, sizeof(sock), RSS_SOCK_FMT, "rcd");
	return sock;
}

cJSON *rmq_rcd_call(const char *request)
{
	/*
	 * Allocated rather than into a fixed buffer: a state document grows
	 * with the camera's stream count and the schema is larger still, and a
	 * reply cut to fit a buffer is invalid JSON with nothing in the return
	 * value to say it was cut.
	 */
	char *resp = NULL;
	if (rss_ctrl_send_command_alloc(rcd_sock(), request, &resp, RMQ_RCD_TIMEOUT_MS) < 0)
		return NULL;

	cJSON *root = cJSON_Parse(resp);
	free(resp);
	return root;
}

cJSON *rmq_rcd_send(cJSON *req)
{
	if (!req)
		return NULL;

	char *text = cJSON_PrintUnformatted(req);
	cJSON_Delete(req);
	if (!text)
		return NULL;

	cJSON *resp = rmq_rcd_call(text);
	free(text);
	return resp;
}

cJSON *rmq_rcd_cmd(const char *cmd)
{
	cJSON *req = cJSON_CreateObject();

	if (!req)
		return NULL;
	cJSON_AddStringToObject(req, "cmd", cmd);
	return rmq_rcd_send(req);
}

bool rmq_rcd_available(void)
{
	cJSON *r = rmq_rcd_cmd("hello");
	bool ok = r != NULL;
	cJSON_Delete(r);
	return ok;
}
