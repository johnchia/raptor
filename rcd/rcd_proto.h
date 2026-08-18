/*
 * rcd_proto.h -- The wire contract
 *
 * One envelope for every reply, whatever the command and whatever went wrong:
 *
 *     {"api":1,"status":"ok", ...}
 *     {"api":1,"status":"error","code":"range","reason":"...", ...}
 *
 * No command answers a bare value or a bare word. That costs a few bytes over
 * the older daemon protocol, where `config-get` answers a raw string and
 * `restart` answers "restarting", and buys a client that parses one shape
 * instead of one per command -- which is the difference between a C or
 * browser client and a pipeline of `tr -d`.
 *
 * `code` is a closed set, for clients; `reason` is prose, for people. Nothing
 * should ever parse `reason`.
 */

#ifndef RCD_PROTO_H
#define RCD_PROTO_H

#include <cJSON.h>

struct rcd_state;

/* Refusal codes. Adding one is a compatible change; changing what an existing
 * one means is not. */
#define RCD_E_MALFORMED "malformed" /* not JSON, or not the shape asked for */
#define RCD_E_UNKNOWN	"unknown"   /* no such command, key or action */
#define RCD_E_TYPE	"type"	    /* right key, wrong kind of value */
#define RCD_E_RANGE	"range"	    /* number outside the key's bounds */
#define RCD_E_CHOICE	"choice"    /* not one of the key's choices */
#define RCD_E_TOOMANY	"too-many"  /* more edits than one request may carry */
#define RCD_E_IO	"io"	    /* the config file could not be written */
#define RCD_E_DAEMON	"daemon"    /* the owning daemon refused or is absent */
#define RCD_E_BUSY	"busy"	    /* an apply is already running */

/* A fresh success envelope, or a refusal. Both are owned by the caller. */
cJSON *rcd_ok(void);
cJSON *rcd_err(const char *code, const char *reason);

/* Attach the key a refusal is about, when there is one. */
void rcd_err_where(cJSON *e, const char *section, const char *key);

/* Handle one request. Matches rss_ctrl_accept_and_handle's callback. */
int rcd_handle(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata);

#endif /* RCD_PROTO_H */
