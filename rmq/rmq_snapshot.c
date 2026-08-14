/*
 * rmq_snapshot.c -- see rmq_snapshot.h
 */

#include "rmq_snapshot.h"
#include "rmq.h"

#include <rss_common.h>
#include <rss_ipc.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * How long to wait for a frame after opening the ring. The encoder is stopped
 * until a reader appears, so this covers rvd noticing, starting it, and one
 * frame interval — at the default [jpeg] fps of 1 that is most of a second.
 */
#define WAIT_MS	     4000
#define WAIT_STEP_MS 250

static uint64_t now_ms(void)
{
	return (uint64_t)(rss_timestamp_us() / 1000);
}

bool rmq_snapshot_due(const struct rmq_state *st, uint64_t now)
{
	if (!st->snapshot_enabled)
		return false;

	/* Zero means nothing has been captured yet. One picture is taken as
	 * soon as the bridge is up whatever the interval says, because a
	 * camera entity with no image behind it reads as broken rather than
	 * as waiting. */
	return st->snapshot_next_ms == 0 || now >= st->snapshot_next_ms;
}

int rmq_snapshot_capture(struct rmq_state *st)
{
	/*
	 * Rescheduled before the attempt, not after it, so a camera that
	 * cannot produce a frame retries on its interval rather than on every
	 * pass of the serve loop. With no interval configured the next one is
	 * whenever it is asked for.
	 */
	st->snapshot_next_ms = st->snapshot_interval_sec > 0
				       ? now_ms() + (uint64_t)st->snapshot_interval_sec * 1000
				       : UINT64_MAX;

	char name[16];
	snprintf(name, sizeof(name), "jpeg%d", st->snapshot_stream);

	rss_ring_t *ring = rss_ring_open(name);
	if (!ring) {
		RSS_WARN("snapshot: ring %s is not there — is rvd running with [jpeg] enabled?",
			 name);
		return -1;
	}

	uint8_t *buf = malloc(RMQ_SNAPSHOT_MAX);
	if (!buf) {
		rss_ring_close(ring);
		return -1;
	}

	/*
	 * Opening the ring is not demand — the header says so plainly, and
	 * rvd counts acquires rather than opens when deciding whether to run
	 * the encoder. Without this the ring is there, readable, and forever
	 * empty, which looks exactly like a broken JPEG channel.
	 */
	rss_ring_acquire(ring);

	/*
	 * Start from the next frame the producer writes, not whatever is
	 * already in the ring: with the encoder stopped between snapshots, the
	 * frame sitting there is left over from the previous capture, and
	 * publishing it would show a picture of whenever that was.
	 */
	const rss_ring_header_t *hdr = rss_ring_get_header(ring);
	uint64_t seq = hdr ? atomic_load(&hdr->write_seq) + 1 : 0;

	int rc = -1;
	uint32_t len = 0;
	for (int waited = 0; waited < WAIT_MS; waited += WAIT_STEP_MS) {
		rss_ring_slot_t meta;
		int r = rss_ring_read(ring, &seq, buf, RMQ_SNAPSHOT_MAX, &len, &meta);
		if (r == 0) {
			rc = 0;
			break;
		}
		if (r == -ENOSPC) {
			RSS_WARN("snapshot: %s frame is larger than %d bytes, dropped", name,
				 RMQ_SNAPSHOT_MAX);
			break;
		}
		/* The producer lapped us, which for a single still just means
		 * take the newest frame instead of the one we asked for. */
		if (r == RSS_EOVERFLOW && hdr)
			seq = atomic_load(&hdr->write_seq);

		rss_ring_wait(ring, WAIT_STEP_MS);
	}

	/* Released before closing, so rvd stops the encoder now rather than
	 * waiting for its dead-reader reap to notice we are gone. */
	rss_ring_release(ring);
	rss_ring_close(ring);

	if (rc != 0) {
		free(buf);
		RSS_WARN("snapshot: no frame from %s within %d ms", name, WAIT_MS);
		return -1;
	}

	/*
	 * Retained, so a dashboard opened later shows the last picture rather
	 * than an empty tile until the next capture. That does leave one image
	 * on the broker, which is the cost of the tile being useful at all.
	 */
	rc = rmq_mqtt_publish(st->mqtt, st->topic_snapshot, (const char *)buf, len, 0, true);
	free(buf);

	if (rc == 0)
		RSS_INFO("snapshot: published %u bytes from %s", len, name);
	return rc;
}
