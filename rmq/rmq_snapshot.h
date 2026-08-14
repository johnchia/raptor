/*
 * rmq_snapshot.h -- JPEG stills from a ring, published as an MQTT image
 *
 * rvd already encodes JPEG on demand: the channel runs only while its ring has
 * a reader (rvd_frame_loop.c:119), so opening the ring is what starts the
 * encoder and closing it is what stops it. A snapshot therefore costs an
 * encode while it is being taken and nothing at all the rest of the time,
 * which is what makes this affordable to leave enabled.
 *
 * The alternative was Home Assistant fetching a URL from rhd, which is what
 * the plan assumed. rhd is not in this image, and a picture that needs a
 * second daemon running is a picture that is usually missing.
 */

#ifndef RMQ_SNAPSHOT_H
#define RMQ_SNAPSHOT_H

#include <stdbool.h>
#include <stdint.h>

struct rmq_state;

/*
 * A ceiling rather than a guess: the main stream at 2560x1920 measures ~600 KB
 * against the sub's ~6 KB, and a frame past this is dropped rather than
 * truncated, because half a JPEG is not a smaller picture.
 */
#define RMQ_SNAPSHOT_MAX (1024 * 1024)

/*
 * Take one frame and publish it retained. Returns 0 on success.
 *
 * Blocks the serve loop for up to RMQ_SNAPSHOT_WAIT_MS while the encoder
 * spins up and produces a frame — bounded deliberately, since the caller is
 * the same single thread that answers commands.
 */
int rmq_snapshot_capture(struct rmq_state *st);

/* True when an automatic capture is due. */
bool rmq_snapshot_due(const struct rmq_state *st, uint64_t now_ms);

#endif /* RMQ_SNAPSHOT_H */
