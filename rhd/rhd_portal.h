/*
 * rhd_portal.h -- Setup mode
 *
 * A camera nobody has given a network to raises an access point of its own
 * and serves one page on it. That is a mode of this daemon rather than a
 * second daemon: two HTTP servers would have to agree about client
 * accounting, about the send path and about the route that carries
 * configuration, and the second one would only ever run on a camera that has
 * not been set up -- which is the least tested code on the device by
 * construction.
 *
 * Three things change, and they are one decision rather than three. An open
 * setup network is not a place to demand a password nobody has yet, so the
 * configuration route stops authenticating; it is therefore also not a place
 * to be reachable from anywhere else, so the listener binds the setup address
 * alone; and it is not a place to expose anything beyond setting the camera
 * up, so every other route is answered with a redirect rather than served.
 * The snapshot and stream endpoints do not need to be excluded by name --
 * they simply are not routed here, and fall to the catch-all like any other
 * path.
 *
 * That catch-all is also what makes the portal captive. Every phone and
 * desktop checks for one by fetching a known URL over plain HTTP and looking
 * at the answer; a redirect away from it is the signal that opens the setup
 * sheet. Answering every unmatched path the same way means the list of those
 * URLs -- /generate_204, /hotspot-detect.html, /ncsi.txt and the rest, which
 * changes with every OS release -- does not have to exist here at all.
 *
 * What is not here: raising the access point, serving DHCP, answering DNS,
 * and deciding at boot which mode to come up in. Those belong to the image,
 * and this file only reads their decision.
 */

#ifndef RHD_PORTAL_H
#define RHD_PORTAL_H

#include <stdbool.h>

#include "rhd.h"

/*
 * The boot path's decision, and the whole of the interface to it.
 *
 * Its existence is what selects setup mode. Its contents are a refinement:
 * `<address>[:<port>]`, the address the portal answers on. Empty, absent or
 * unparseable falls back to the defaults below -- deliberately, because the
 * decision was already made by whoever wrote the file, and a camera that
 * ignored it over a typo would come up as a client with no credentials and no
 * way in.
 */
#define RHD_PORTAL_FLAG "/run/portal_mode"

/* Where the portal lands when the flag does not say. Matches what the image
 * gives the access point. */
#define RHD_PORTAL_ADDR "172.16.0.1"

/*
 * Port 80, and it is not a preference. A connectivity check is an HTTP
 * request to port 80 by definition, so a portal on any other port is one no
 * phone will ever open a sheet for. The flag may override it anyway, because
 * a portal that can only be tested with an access point raised is a portal
 * that gets tested once.
 */
#define RHD_PORTAL_PORT 80

/* The page. Not the console: see rhd_portal.c. */
#define RHD_PORTAL_PAGE "/usr/share/raptor/portal.html"

/*
 * Read the flag and fill in srv->portal, srv->portal_addr and srv->port.
 * Called before the listener is created, because it decides what to bind.
 */
void rhd_portal_init(rhd_server_t *srv);

/*
 * Take the request if setup mode is on. Returns false when it is off, so the
 * caller falls through to its own routing; returns true for everything when
 * it is on, because in setup mode there is nothing else to fall through to.
 */
bool rhd_portal_route(rhd_server_t *srv, rhd_client_t *c, const char *method, const char *path);

/* Release the cached page. */
void rhd_portal_free(void);

#endif /* RHD_PORTAL_H */
