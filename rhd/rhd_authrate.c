/*
 * rhd_authrate.c -- what it costs to guess the system password
 *
 * See rhd_authrate.h for why this exists and what the numbers are.
 */

#include <string.h>

#include <rss_common.h>

#include "rhd_authrate.h"

typedef struct {
	char host[64]; /* "" when the slot is free */
	unsigned int failures;
	int64_t wait_until_ms;
	int64_t seen_ms;
	bool told; /* the log already says this one is being made to wait */
} rhd_auth_source_t;

/*
 * File-static, and only the main loop ever reaches any of it -- the same
 * assumption that makes crypt()'s static storage safe in rhd_api.c. A table
 * this small is walked rather than hashed.
 */
static rhd_auth_source_t g_src[RHD_AUTH_SOURCES];
static int g_tokens = RHD_AUTH_BURST;
static int64_t g_refill_ms;

void rhd_auth_reset(void)
{
	memset(g_src, 0, sizeof(g_src));
	g_tokens = RHD_AUTH_BURST;
	g_refill_ms = 0;
}

/* The slot for this host: reused, taken free, or taken from whichever host has
 * been quiet longest. */
static rhd_auth_source_t *slot_for(const char *host, int64_t now)
{
	rhd_auth_source_t *oldest = &g_src[0];

	for (int i = 0; i < RHD_AUTH_SOURCES; i++) {
		rhd_auth_source_t *s = &g_src[i];

		if (s->host[0] && strcmp(s->host, host) == 0) {
			/* Quiet long enough to have stopped. A password
			 * mistyped this morning is not evidence about this
			 * afternoon, and holding it against the operator for
			 * the life of the process would be. */
			if (now - s->seen_ms > RHD_AUTH_FORGET_MS) {
				s->failures = 0;
				s->wait_until_ms = 0;
				s->told = false;
			}
			return s;
		}
		if (!s->host[0])
			oldest = s;
		else if (oldest->host[0] && s->seen_ms < oldest->seen_ms)
			oldest = s;
	}

	memset(oldest, 0, sizeof(*oldest));
	rss_strlcpy(oldest->host, host, sizeof(oldest->host));
	return oldest;
}

bool rhd_auth_may_hash(const char *host, int64_t now, int *retry_sec)
{
	rhd_auth_source_t *s = slot_for(host, now);
	int ignored = 0;

	if (!retry_sec)
		retry_sec = &ignored;

	s->seen_ms = now;

	if (s->wait_until_ms > now) {
		*retry_sec = (int)((s->wait_until_ms - now + 999) / 1000);
		if (!s->told) {
			RSS_WARN("api: %s has offered the wrong system password %u times; "
				 "it is being made to wait",
				 host, s->failures);
			s->told = true;
		}
		return false;
	}

	/*
	 * The ceiling. Refilled on demand rather than on a timer, because
	 * there is no timer here and a camera that nobody is attacking should
	 * not be woken to count tokens it already has.
	 */
	if (g_refill_ms == 0)
		g_refill_ms = now;
	while (g_tokens < RHD_AUTH_BURST && now - g_refill_ms >= RHD_AUTH_REFILL_MS) {
		g_tokens++;
		g_refill_ms += RHD_AUTH_REFILL_MS;
	}
	/* A clock that went backwards, or a first call long after the last:
	 * re-anchor rather than spin the loop above over the whole gap. */
	if (g_tokens >= RHD_AUTH_BURST || g_refill_ms > now)
		g_refill_ms = now;

	if (g_tokens <= 0) {
		*retry_sec = 1;
		return false;
	}
	g_tokens--;
	return true;
}

void rhd_auth_failed(const char *host, int64_t now)
{
	rhd_auth_source_t *s = slot_for(host, now);

	s->failures++;
	if (s->failures <= RHD_AUTH_FREE_TRIES)
		return;

	/*
	 * Doubling, from the first wait up to the ceiling. Counted up rather
	 * than shifted by the excess: a host that keeps going would overflow
	 * the shift long before the clamp ever saw it.
	 */
	int64_t wait = RHD_AUTH_DELAY_MS;

	for (unsigned int i = RHD_AUTH_FREE_TRIES + 1;
	     i < s->failures && wait < RHD_AUTH_DELAY_MAX_MS; i++)
		wait *= 2;
	if (wait > RHD_AUTH_DELAY_MAX_MS)
		wait = RHD_AUTH_DELAY_MAX_MS;

	s->wait_until_ms = now + wait;
	s->told = false;
}

void rhd_auth_succeeded(const char *host, int64_t now)
{
	rhd_auth_source_t *s = slot_for(host, now);

	s->failures = 0;
	s->wait_until_ms = 0;
	s->told = false;

	/*
	 * Given back. Without this the operator's own console -- which polls
	 * this route every few seconds and gets the password right every time
	 * -- would spend the ceiling that exists to slow down the people who
	 * cannot log in at all, and an attacker would only have to wait for it
	 * to do so.
	 */
	if (g_tokens < RHD_AUTH_BURST)
		g_tokens++;
}
