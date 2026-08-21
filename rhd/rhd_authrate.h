/*
 * rhd_authrate.h -- what it costs to guess the system password
 *
 * The check itself lives in rhd_api.c and is sound: one crypt() against the
 * /etc/shadow entry, compared in constant time. The problem this file answers
 * is that reaching it was free. Anyone who could open a socket could ask rhd
 * to hash a password against the root account as fast as the camera would
 * answer, for as long as they liked.
 *
 * Two consequences, needing different answers.
 *
 * The first is an online brute force against the only secret on the camera
 * that is not also handed out to watch video -- the media credentials are in
 * raptor.conf in cleartext, this one is not.
 *
 * The second is that every hash runs on the main loop, between one MJPEG frame
 * and the next. Measured on an SSC377QE: 4 ms against this image's md5crypt
 * entry, 32 ms against sha256, 91 ms against sha512. Which of those an image
 * ships is not rhd's to choose, so nothing here may assume the cheap one.
 *
 * So a host that keeps guessing waits longer and longer, and separately no
 * more than a couple of hashes a second are computed for anybody at all. The
 * first stops one host working through a dictionary; the second bounds what
 * the main loop can be made to spend, however many hosts are asking.
 *
 * A hash that succeeds gives back what it took, which is what keeps this from
 * being a way to lock the operator out: the console polls this route every few
 * seconds and never spends anything, while a client that has never once got
 * the password right pays for every attempt.
 *
 * `now` is passed in rather than read here so the policy can be tested against
 * a clock the test controls -- waiting out a thirty-second backoff is not a
 * thing a suite should do.
 */

#ifndef RHD_AUTHRATE_H
#define RHD_AUTHRATE_H

#include <stdbool.h>
#include <stdint.h>

/* Hosts remembered at once, wrong answers allowed before the waiting starts,
 * the first wait and the longest, and how long a quiet host is remembered. */
#define RHD_AUTH_SOURCES      8
#define RHD_AUTH_FREE_TRIES   3
#define RHD_AUTH_DELAY_MS     1000
#define RHD_AUTH_DELAY_MAX_MS 30000
#define RHD_AUTH_FORGET_MS    300000

/* The ceiling, for everyone together: six to spend at once and one back every
 * half second, so a sustained attack from any number of hosts costs the main
 * loop two hashes a second and no more. */
#define RHD_AUTH_BURST	   6
#define RHD_AUTH_REFILL_MS 500

/*
 * Whether a hash may be computed for `host` now, and how many seconds it must
 * wait if not.
 *
 * Call only once credentials have actually been offered. A browser's first
 * request to a protected route carries none and is answered with a challenge;
 * charging for that would put every ordinary visitor one step nearer being
 * throttled.
 *
 * A permitted call spends from the ceiling, so it must be followed by exactly
 * one of the two below.
 */
bool rhd_auth_may_hash(const char *host, int64_t now, int *retry_sec);

/* The hash was computed and did not match. */
void rhd_auth_failed(const char *host, int64_t now);

/* It matched: this host's record is cleared and the ceiling is given back. */
void rhd_auth_succeeded(const char *host, int64_t now);

/* Forget every host and refill the ceiling. For tests, and for nothing else --
 * rhd has no route that resets its own throttle. */
void rhd_auth_reset(void);

#endif /* RHD_AUTHRATE_H */
