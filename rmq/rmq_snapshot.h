/*
 * rmq_snapshot.h -- The camera's picture, published as the URL rhd serves it
 *
 * rhd serves /snap.jpg and /mjpeg out of the same JPEG rings rvd fills, so the
 * picture is fetched from the camera rather than pushed through the broker.
 * What this publishes is a small document of URLs, retained on
 * <topic_prefix>/snapshot. Home Assistant's image entity refetches whenever a
 * message lands there, which makes the configured interval the refresh rate
 * and {"cmd":"snapshot"} a refresh on demand.
 *
 * This file previously read the JPEG ring itself and published the frame. That
 * cost the broker a retained message the size of the picture — ~600 KB from
 * the main stream — and every subscriber a copy of it, to deliver something
 * the camera was already able to serve. It was written that way because rhd
 * was not in the image; it is now.
 *
 * The encode is unchanged either way. rhd acquires a JPEG ring only while a
 * request is parked on it or an MJPEG client is watching, so rvd's JPEG
 * channel still runs for the fetch and is stopped the rest of the time.
 */

#ifndef RMQ_SNAPSHOT_H
#define RMQ_SNAPSHOT_H

#include <rss_common.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct rmq_state;

/* Scheme, credential, bracketed address, port, path and query. */
#define RMQ_URL_MAX 320

/*
 * Build one of rhd's URLs, addressed so that whoever reads the broker can
 * reach it. Returns 0, or -1 when rhd is not answering — a URL to a port
 * nothing is listening on is worse than no URL at all.
 *
 * `with_auth` embeds the [http] credential. Pass it only for a consumer that
 * has nowhere else to put one: anything a person is meant to read or click
 * should get the bare URL and supply the credential itself.
 */
int rmq_snapshot_url(const struct rmq_state *st, const char *path, bool with_auth, char *out,
		     size_t outsz);

/*
 * Take the [http] credential from a parsed config. Called at startup and
 * again whenever rmq rewrites the file, since a bridge holding the previous
 * password would publish a URL that 401s and say nothing about why.
 */
void rmq_http_creds(struct rmq_state *st, rss_config_t *cfg);

/* Publish the URL document, retained. Returns 0 on success. */
int rmq_snapshot_publish(struct rmq_state *st);

/* True when an automatic refresh is due. */
bool rmq_snapshot_due(const struct rmq_state *st, uint64_t now_ms);

#endif /* RMQ_SNAPSHOT_H */
