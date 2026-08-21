/*
 * test_rhd_auth.c -- what it costs to guess the system password
 *
 * The check itself is not under test here; rhd_api.c owns that, and it was
 * always sound. What is under test is the thing that was missing: that
 * reaching it costs something.
 *
 * Three properties carry the weight, and they pull against each other, which
 * is why all three are asserted rather than the first two:
 *
 *   - a host that keeps guessing waits longer and longer, so a dictionary
 *     cannot be worked through;
 *   - no number of hosts can make the camera hash faster than a fixed
 *     ceiling, because every one of those hashes runs on the main loop
 *     between two MJPEG frames; and
 *   - a client that gets the password right pays nothing at all, so none of
 *     the above becomes a way to lock the operator out of their own camera.
 *
 * Time is passed in rather than waited for. The longest backoff is thirty
 * seconds and the suite is not going to spend thirty seconds finding out what
 * happens at the end of it.
 */
#include <string.h>

#include "greatest.h"

#include "../rhd/rhd_authrate.h"

/* An attempt: ask, and if allowed, report that the hash did not match. Returns
 * whether a hash would have been computed. */
static bool guess(const char *host, int64_t now)
{
	if (!rhd_auth_may_hash(host, now, NULL))
		return false;
	rhd_auth_failed(host, now);
	return true;
}

TEST the_first_few_wrong_answers_are_free(void)
{
	rhd_auth_reset();

	/* Somebody mistyping a password must not be made to wait on the
	 * second attempt. The camera is on a desk as often as it is under
	 * attack. */
	for (int i = 0; i < RHD_AUTH_FREE_TRIES; i++)
		ASSERT(guess("10.0.0.5", 1000));
	PASS();
}

TEST a_host_that_keeps_guessing_is_made_to_wait(void)
{
	rhd_auth_reset();

	for (int i = 0; i < RHD_AUTH_FREE_TRIES + 1; i++)
		ASSERT(guess("10.0.0.5", 1000));

	/* Now it costs. */
	int retry = 0;
	ASSERT_FALSE(rhd_auth_may_hash("10.0.0.5", 1000, &retry));
	ASSERTm("no wait was reported", retry > 0);

	/* And waiting it out works -- a throttle nobody can ever get out of
	 * is a lockout, not a throttle. */
	ASSERT(rhd_auth_may_hash("10.0.0.5", 1000 + RHD_AUTH_DELAY_MS, NULL));
	PASS();
}

TEST the_wait_grows_and_then_stops_growing(void)
{
	rhd_auth_reset();

	int64_t now = 1000;
	int prev = 0;
	int grew = 0;

	/* Guess until the wait stops getting longer, always waiting exactly
	 * as long as it asked for. */
	for (int i = 0; i < 20; i++) {
		int retry = 0;

		if (!rhd_auth_may_hash("10.0.0.5", now, &retry)) {
			ASSERT(retry > 0);
			if (retry > prev)
				grew++;
			prev = retry;
			now += (int64_t)retry * 1000;
			continue;
		}
		rhd_auth_failed("10.0.0.5", now);
	}

	ASSERTm("the wait never got longer", grew > 1);
	ASSERT_EQm("the wait ran past its ceiling", RHD_AUTH_DELAY_MAX_MS / 1000, prev);
	PASS();
}

TEST getting_it_right_clears_the_record(void)
{
	rhd_auth_reset();

	for (int i = 0; i < RHD_AUTH_FREE_TRIES + 1; i++)
		ASSERT(guess("10.0.0.5", 1000));
	ASSERT_FALSE(rhd_auth_may_hash("10.0.0.5", 1000, NULL));

	/* Wait it out and get it right. */
	int64_t later = 1000 + RHD_AUTH_DELAY_MS;

	ASSERT(rhd_auth_may_hash("10.0.0.5", later, NULL));
	rhd_auth_succeeded("10.0.0.5", later);

	/* The count is gone, so the next mistype starts from the beginning
	 * rather than from thirty seconds. */
	for (int i = 0; i < RHD_AUTH_FREE_TRIES; i++)
		ASSERT(guess("10.0.0.5", later));
	PASS();
}

TEST one_host_being_throttled_does_not_throttle_another(void)
{
	rhd_auth_reset();

	for (int i = 0; i < RHD_AUTH_FREE_TRIES + 1; i++)
		ASSERT(guess("10.0.0.5", 1000));
	ASSERT_FALSE(rhd_auth_may_hash("10.0.0.5", 1000, NULL));

	/* The operator at another desk is not the attacker, and the whole
	 * point of tracking hosts separately is that they are told apart. */
	ASSERT(rhd_auth_may_hash("10.0.0.9", 1000, NULL));
	PASS();
}

/*
 * The ceiling, which is the half that bounds what the main loop can be made to
 * spend. Per-host backoff cannot do it on its own: a host that has never been
 * seen before starts with its free tries, so somebody with enough addresses
 * would never be slowed by the first mechanism at all.
 */
TEST no_number_of_hosts_gets_past_the_ceiling(void)
{
	rhd_auth_reset();

	int hashed = 0;

	/* Every request from a different address, all in the same
	 * millisecond, so nothing is refilled while it runs. */
	for (int i = 0; i < 200; i++) {
		char host[32];

		snprintf(host, sizeof(host), "10.0.%d.%d", i / 250, i % 250);
		if (rhd_auth_may_hash(host, 5000, NULL)) {
			hashed++;
			rhd_auth_failed(host, 5000);
		}
	}

	ASSERT_EQm("the ceiling did not hold", RHD_AUTH_BURST, hashed);
	PASS();
}

TEST the_ceiling_refills_over_time(void)
{
	rhd_auth_reset();

	for (int i = 0; i < RHD_AUTH_BURST; i++)
		ASSERT(rhd_auth_may_hash("10.0.0.5", 5000, NULL));
	ASSERT_FALSE(rhd_auth_may_hash("10.0.0.5", 5000, NULL));

	/* One back per refill interval, and no more than that: a camera left
	 * alone for an hour does not hand out an hour's worth at once. */
	ASSERT(rhd_auth_may_hash("10.0.0.5", 5000 + RHD_AUTH_REFILL_MS, NULL));
	ASSERT_FALSE(rhd_auth_may_hash("10.0.0.5", 5000 + RHD_AUTH_REFILL_MS, NULL));
	PASS();
}

/*
 * And the property that keeps the ceiling from being an outage of its own. The
 * console polls this route every few seconds with the right password; if that
 * spent from the ceiling, an attacker would only have to wait for the operator
 * to lock themselves out.
 */
TEST a_client_that_knows_the_password_never_spends_the_ceiling(void)
{
	rhd_auth_reset();

	for (int i = 0; i < 500; i++) {
		int64_t now = 5000 + i; /* far faster than the refill */

		ASSERTm("the console was throttled", rhd_auth_may_hash("10.0.0.5", now, NULL));
		rhd_auth_succeeded("10.0.0.5", now);
	}

	/* And the ceiling is still whole for everybody else. */
	int hashed = 0;

	for (int i = 0; i < RHD_AUTH_BURST + 4; i++) {
		if (rhd_auth_may_hash("10.0.0.9", 5500, NULL))
			hashed++;
	}
	ASSERT_EQ(RHD_AUTH_BURST, hashed);
	PASS();
}

/*
 * A host quiet for long enough is forgotten. Without this the table is a
 * grudge: the operator who fat-fingered their password before lunch is still
 * paying for it at the end of the day.
 */
TEST a_host_that_stops_is_forgotten(void)
{
	rhd_auth_reset();

	for (int i = 0; i < RHD_AUTH_FREE_TRIES + 1; i++)
		ASSERT(guess("10.0.0.5", 1000));
	ASSERT_FALSE(rhd_auth_may_hash("10.0.0.5", 1000, NULL));

	int64_t much_later = 1000 + RHD_AUTH_FORGET_MS + 1;

	for (int i = 0; i < RHD_AUTH_FREE_TRIES; i++)
		ASSERTm("it was still being held against them", guess("10.0.0.5", much_later + i));
	PASS();
}

SUITE(rhd_auth_suite)
{
	RUN_TEST(the_first_few_wrong_answers_are_free);
	RUN_TEST(a_host_that_keeps_guessing_is_made_to_wait);
	RUN_TEST(the_wait_grows_and_then_stops_growing);
	RUN_TEST(getting_it_right_clears_the_record);
	RUN_TEST(one_host_being_throttled_does_not_throttle_another);
	RUN_TEST(no_number_of_hosts_gets_past_the_ceiling);
	RUN_TEST(the_ceiling_refills_over_time);
	RUN_TEST(a_client_that_knows_the_password_never_spends_the_ceiling);
	RUN_TEST(a_host_that_stops_is_forgotten);
}
