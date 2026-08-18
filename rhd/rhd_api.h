/*
 * rhd_api.h -- POST /api/v1/rcd
 *
 * The console in index.html is a browser, and a browser cannot open a unix
 * socket. This is the whole bridge: the request body is handed to rcd
 * unread and rcd's reply is handed back unread.
 *
 * rhd deliberately understands none of it. Every rule about what may be set,
 * to what, by whom and at what cost lives in rcd's table, and a second copy
 * here -- even a well-meaning one that only checked the section name -- would
 * be a copy to drift. The one thing this file does decide is that the round
 * trip must not happen on the main loop: an apply restarts rvd and takes
 * seconds, and the main loop is also driving MJPEG.
 */

#ifndef RHD_API_H
#define RHD_API_H

#include <stdbool.h>
#include <stddef.h>

#include "rhd.h"

/* rcd's control socket and how long to wait for it. An apply that restarts
 * the pipeline is bounded by rcd's own 25s bring-up ceiling, not by this. */
#define RHD_API_PATH	   "/api/v1/rcd"
#define RHD_API_TIMEOUT_MS 45000
#define RHD_API_MAX_BODY   4096

/*
 * True once the buffer holds a whole request. A GET ends at the blank line;
 * a POST does not, and acting on a half-arrived body would hand rcd a
 * truncated object to refuse.
 */
bool rhd_request_complete(const char *buf, size_t len);

/*
 * Take the request if it is one of ours. Returns false when it is not, so the
 * caller falls through to its own routing. The reply is not sent here: a
 * worker is started and the answer goes out from rhd_api_poll().
 */
bool rhd_api_handle(rhd_server_t *srv, rhd_client_t *c, const char *method, const char *path);

/* Finish the round trips that have come back. Once per main-loop pass. */
void rhd_api_poll(rhd_server_t *srv);

/* True while any client is waiting on rcd, so the loop keeps a short tick. */
bool rhd_api_waiting(const rhd_server_t *srv);

/* Let go of a job whose client is going away. The worker may still be inside
 * a blocking read; it frees what is left when it returns. */
void rhd_api_release(rhd_client_t *c);

#endif /* RHD_API_H */
