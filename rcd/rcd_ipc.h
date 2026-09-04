/*
 * rcd_ipc.h -- Talking to the other daemons
 *
 * Every raptor daemon answers the same request/response protocol on its own
 * Unix socket, and rcd is a client of all of them. The helpers here are the
 * only place that knows a daemon may answer with either a JSON object or a
 * bare string, so nothing above has to keep guessing which.
 */

#ifndef RCD_IPC_H
#define RCD_IPC_H

#include <stdbool.h>
#include <stddef.h>

#include <cJSON.h>

/* A command waits this long; a liveness probe answers at once or not at all,
 * and runs in a loop, so it waits far less. */
#define RCD_CTRL_TIMEOUT_MS  2000
#define RCD_PROBE_TIMEOUT_MS 400

/*
 * THE REQUEST IS BUILT HERE, NOT WRITTEN AT THE CALL SITE.
 *
 * Almost everything rcd asks another daemon is a bare command with no
 * arguments, and the obvious spelling of that -- passing the four-word
 * literal {"cmd":"status"} -- is hand-assembled JSON, which the tree does
 * not allow (tools/conformity/json-gate.sh). So these take the command's
 * name and cJSON puts the quotes in. The one caller that has a request of
 * its own to send, already built, uses rcd_ask_req_ok below.
 */

/* Send the bare command `cmd` to `daemon`. Returns the response length, or
 * negative when the daemon is absent or did not answer. */
int rcd_ask(const char *daemon, const char *cmd, char *resp, size_t respsz, int timeout_ms);

/*
 * True unless the daemon answered an explicit non-ok status.
 *
 * A daemon answers either a JSON object carrying a status or a bare string, so
 * only an explicit failure counts as one. `restart` itself answers the bare
 * string "restarting", which is why an unparseable reply is a success rather
 * than an error.
 */
bool rcd_ask_ok(const char *daemon, const char *cmd);

/*
 * As above for a request the caller has already built, with `err` receiving
 * the daemon's own reason when it refused so a caller can report why rather
 * than only that. `req` must come from a serializer.
 */
bool rcd_ask_req_ok(const char *daemon, const char *req, char *err, size_t errsz);

/* Send the bare command `cmd` and parse the reply. Returns an object the
 * caller deletes, or NULL when the daemon is absent or answered unusably --
 * which are the same thing from here, and both simply mean "no state". */
cJSON *rcd_ask_json(const char *daemon, const char *cmd);

/* As above for a command carrying one string argument. */
cJSON *rcd_ask_json_str(const char *daemon, const char *cmd, const char *key, const char *value);

/* Whether the daemon's control socket answers at all. */
bool rcd_answers(const char *daemon);

#endif /* RCD_IPC_H */
