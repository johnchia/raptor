/*
 * rhd_portal.c -- see rhd_portal.h
 */

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rhd.h"
#include "rhd_portal.h"

/* The page, loaded on first request and kept. Same treatment as the console:
 * it is read once and served from memory, because the flash it lives on is
 * the slowest thing in the request. */
static char *portal_html;
static int portal_html_len;

void rhd_portal_free(void)
{
	free(portal_html);
	portal_html = NULL;
	portal_html_len = 0;
}

/*
 * Parse `<address>[:<port>]` out of the flag.
 *
 * Whitespace-tolerant because the file is written by a shell script, and a
 * trailing newline is what `echo` produces. Anything it cannot make sense of
 * leaves the defaults in place; see rhd_portal.h for why that is not silent
 * failure.
 */
static void parse_flag(const char *text, char *addr, size_t addrsz, int *port)
{
	char buf[64];
	size_t n = 0;

	while (*text == ' ' || *text == '\t')
		text++;
	while (text[n] && text[n] != '\n' && text[n] != '\r' && text[n] != ' ' && text[n] != '\t' &&
	       n + 1 < sizeof(buf)) {
		buf[n] = text[n];
		n++;
	}
	buf[n] = '\0';

	if (!buf[0])
		return; /* the flag on its own, which is the ordinary case */

	char *colon = strrchr(buf, ':');
	if (colon) {
		*colon = '\0';
		int p = atoi(colon + 1);
		if (p > 0 && p < 65536)
			*port = p;
		else
			RSS_WARN("portal: '%s' is not a port; serving on %d", colon + 1, *port);
	}

	struct in_addr a;
	if (inet_pton(AF_INET, buf, &a) == 1)
		rss_strlcpy(addr, buf, addrsz);
	else
		RSS_WARN("portal: '%s' is not an address; serving on %s", buf, addr);
}

void rhd_portal_init(rhd_server_t *srv)
{
	int len = 0;
	char *flag = rss_read_file(RHD_PORTAL_FLAG, &len);

	if (!flag)
		return;

	srv->portal = true;
	srv->port = RHD_PORTAL_PORT;
	rss_strlcpy(srv->portal_addr, RHD_PORTAL_ADDR, sizeof(srv->portal_addr));
	parse_flag(flag, srv->portal_addr, sizeof(srv->portal_addr), &srv->port);
	free(flag);

	/*
	 * The configuration route is the portal's only reason to exist, so
	 * setup mode turns it on whatever the config file says. A camera that
	 * came up unprovisioned with `api_enabled = false` would otherwise
	 * serve a setup page that could not set anything, and there would be
	 * no other way in to change the setting.
	 */
	srv->api_enabled = true;

	/*
	 * Said at INFO and said plainly. This is the one configuration in
	 * which anything that can reach the port can reconfigure the camera,
	 * and an operator reading a log should be able to see that the camera
	 * is in it rather than infer it from what is missing.
	 */
	RSS_INFO("setup mode: serving the portal on %s:%d, unauthenticated", srv->portal_addr,
		 srv->port);
}

/* The absolute URL of the page, which is what a Location has to be for the
 * captive-portal agents that follow it. The port is left out when it is 80,
 * because a URL that names it is one some of those agents will not match
 * against the address they probed. */
static void portal_url(const rhd_server_t *srv, char *out, size_t outsz)
{
	if (srv->port == 80)
		snprintf(out, outsz, "http://%s/", srv->portal_addr);
	else
		snprintf(out, outsz, "http://%s:%d/", srv->portal_addr, srv->port);
}

/* Whether this is a request for the page itself. The query string is ignored
 * rather than refused: a client arriving from a captive-portal sheet often
 * carries one, and it is not addressed to us. */
static bool is_root(const char *path)
{
	return path[0] == '/' && (path[1] == '\0' || path[1] == '?');
}

bool rhd_portal_route(rhd_server_t *srv, rhd_client_t *c, const char *method, const char *path)
{
	if (!srv->portal)
		return false;

	if (strcmp(method, "GET") == 0 && is_root(path)) {
		if (!portal_html) {
			portal_html = rss_read_file(RHD_PORTAL_PAGE, &portal_html_len);
			if (!portal_html)
				RSS_WARN("%s not found", RHD_PORTAL_PAGE);
		}

		/*
		 * A camera in setup mode with no page installed has nothing to
		 * offer and no other way in, so this says which file is
		 * missing rather than "not found". Nobody reaching it has a
		 * console to look in.
		 */
		if (!portal_html)
			http_error(c, "500 Internal Server Error",
				   "the setup page (" RHD_PORTAL_PAGE ") is not installed\n");
		else if (http_send_async(c, srv->epoll_fd, "text/html; charset=utf-8", portal_html,
					 (uint32_t)portal_html_len) < 0)
			http_error(c, "500 Internal Server Error", "Out of memory");
		return true;
	}

	char url[128];
	portal_url(srv, url, sizeof(url));
	http_302(c, url);
	return true;
}
