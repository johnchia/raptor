/*
 * rmq_rcd.h -- The bridge's link to the config daemon
 *
 * rmq plans nothing and validates nothing. What may be set, what a value may
 * be, which daemon owns it, whether it takes effect now or needs a restart --
 * all of that is rcd's table, and the bridge would only be keeping a second
 * copy of it to drift out of step with. So the command topic is a transport:
 * a payload is size-checked, parsed for its own bookkeeping, and handed over.
 *
 * The deny-by-default property is not weakened by that. It was never the
 * bridge's to enforce -- rcd refuses a command its table does not name, and a
 * field its entry does not name is dropped, whichever client asked.
 */

#ifndef RMQ_RCD_H
#define RMQ_RCD_H

#include <stdbool.h>

#include <cJSON.h>

/* Long enough for an apply, which blocks until the slowest daemon is back. */
#define RMQ_RCD_TIMEOUT_MS 45000

/*
 * Send one request to rcd and return its reply, which the caller deletes.
 * NULL means rcd did not answer at all -- a different thing from a refusal,
 * which comes back as a reply with status "error".
 */
cJSON *rmq_rcd_call(const char *request);

/* As above, from an object the caller built. Takes ownership of `req`. */
cJSON *rmq_rcd_send(cJSON *req);

/* The bare {"cmd":"..."} request, built by the serializer rather than
 * written out here -- see tools/conformity/json-gate.sh. */
cJSON *rmq_rcd_cmd(const char *cmd);

/* Whether rcd is answering. Used at startup to say so once, rather than
 * failing every command with the same message. */
bool rmq_rcd_available(void);

#endif /* RMQ_RCD_H */
