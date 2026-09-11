/*
 * rhd_api.h -- POST /api/v1/rcd
 *
 * The console in console.html is a browser, and a browser cannot open a unix
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
 * Claiming: the one route that answers a camera nobody can authenticate to.
 *
 * A fresh camera's root account has no password, so the route above refuses
 * every request including its owner's. This is the way out of that, and it is
 * a second path rather than a special case of the first for one reason: the
 * request that reaches rcd on it is built here, from two fields, so an
 * unauthenticated caller cannot name a command. That is the only place this
 * file's "understands none of it" property bends, and bending it here is what
 * keeps it intact on the route that carries everything else.
 *
 * GET answers whether the camera may still be claimed. POST claims it.
 */
#define RHD_API_CLAIM_PATH "/api/v1/claim"

/* Where the system account lives. Overridable so the suite can point the
 * check at a file it wrote rather than at this host's. */
#ifndef RHD_SHADOW_PATH
#define RHD_SHADOW_PATH "/etc/shadow"
#endif

/* The account rhd authenticates against, and the one a claim writes. */
#define RHD_API_USER "root"

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

/*
 * The system account as a gate elsewhere: the console page is held by it, and
 * the media routes take it as a second key, since whoever configures the
 * camera may watch it and the console's own preview arrives with that account.
 *
 * rhd_api_authenticate() reads the request's Basic credential and says what
 * it was: right, absent, wrong, or refused because the host is paying for
 * earlier guesses -- with the wait in `retry_sec`. A wrong password costs what
 * it costs on the configuration route. rhd_api_401() and rhd_api_429() are
 * the replies for the last two, in the configuration realm, so a browser that
 * answers holds the account for every route under the page.
 *
 * rhd_api_claimed() is whether there is a system account to hold anything:
 * until the camera is claimed the page has to be open, since the page is how
 * it gets one.
 */
typedef enum {
	RHD_AUTH_OK,
	RHD_AUTH_NONE,
	RHD_AUTH_BAD,
	RHD_AUTH_THROTTLED,
} rhd_auth_t;

rhd_auth_t rhd_api_authenticate(rhd_client_t *c, int *retry_sec);
void rhd_api_401(rhd_client_t *c);
void rhd_api_429(rhd_client_t *c, int retry_sec);
bool rhd_api_claimed(void);

#endif /* RHD_API_H */
