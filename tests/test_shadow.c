/*
 * test_shadow.c -- the three-way answer that decides whether a camera may be
 * taken
 *
 * rhd will authenticate against this account and rcd will write it, and the
 * one thing they must agree about is which cameras are still unclaimed. The
 * obvious version of that question has two answers and is wrong twice over:
 * an account locked on purpose is not an invitation, and a file this process
 * cannot read is a broken camera rather than an available one. Both would be
 * "no password set" to a boolean.
 *
 * So the split under test is that only a genuinely empty field is claimable,
 * and only a well-formed hash authenticates.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "greatest.h"

#include <rss_shadow.h>

/* A scratch shadow file. Written rather than mocked: the parser's job is to
 * survive real lines, including the ones a vendor image ships. */
#define SHADOW_PATH "/run/rss/test-shadow"

static bool write_shadow(const char *body)
{
	FILE *f = fopen(SHADOW_PATH, "w");

	if (!f)
		return false;
	fputs(body, f);
	fclose(f);
	return true;
}

/* The line OpenIPC ships, and the reason this whole mechanism exists: the
 * password field is empty, so nobody -- including the owner -- can
 * authenticate, and the camera has to be claimable instead. */
#define SHIPPED "root::19477::::::\ndaemon:*:::::::\nnobody:*:::::::\n"

TEST an_empty_field_is_a_camera_waiting_to_be_claimed(void)
{
	if (!write_shadow(SHIPPED))
		SKIPm("no writable /run/rss -- run the suite under unshare -rm");

	ASSERT_EQ(RSS_SHADOW_UNSET, rss_shadow_state(SHADOW_PATH, "root"));

	char hash[160];
	ASSERT_FALSEm("an empty field must never authenticate",
		      rss_shadow_hash(SHADOW_PATH, "root", hash, sizeof(hash)));
	PASS();
}

TEST a_hash_is_a_camera_somebody_has(void)
{
	if (!write_shadow("root:$5$abcdefghijklmnop$ZZZZ:19477::::::\n"))
		SKIPm("no writable /run/rss -- run the suite under unshare -rm");

	ASSERT_EQ(RSS_SHADOW_SET, rss_shadow_state(SHADOW_PATH, "root"));

	char hash[160];
	ASSERT(rss_shadow_hash(SHADOW_PATH, "root", hash, sizeof(hash)));
	ASSERT_STR_EQ("$5$abcdefghijklmnop$ZZZZ", hash);
	PASS();
}

/*
 * The distinction the boolean version of this would lose. An integrator who
 * ships a locked root account has said "no password login on this device",
 * and a claim route that read that as "no password yet" would undo their
 * decision from the network.
 */
TEST a_locked_account_is_not_an_unclaimed_one(void)
{
	static const char *const locked[] = {"*", "!", "!!", "!$5$abc$def", NULL};

	for (int i = 0; locked[i]; i++) {
		char line[128];

		snprintf(line, sizeof(line), "root:%s:19477::::::\n", locked[i]);
		if (!write_shadow(line))
			SKIPm("no writable /run/rss -- run the suite under unshare -rm");

		ASSERT_EQm(locked[i], RSS_SHADOW_LOCKED, rss_shadow_state(SHADOW_PATH, "root"));

		char hash[160];
		ASSERT_FALSE(rss_shadow_hash(SHADOW_PATH, "root", hash, sizeof(hash)));
	}
	PASS();
}

/*
 * A field that is not a hash and not one of the spellings above -- a bare DES
 * entry, a truncated line, a format this libc does not implement -- is refused
 * rather than guessed at. Declining a password field is the safe way to be
 * wrong about one.
 */
TEST a_field_that_is_not_a_hash_is_refused(void)
{
	static const char *const junk[] = {
		"abcdefghijklm", /* a bare DES hash */
		"$",		 /* a lone marker */
		"$5$",		 /* a method and nothing else */
		"$5$salt",	 /* no digest */
		"$5$salt$",	 /* an empty digest */
		NULL,
	};

	for (int i = 0; junk[i]; i++) {
		char line[128];

		snprintf(line, sizeof(line), "root:%s:19477::::::\n", junk[i]);
		if (!write_shadow(line))
			SKIPm("no writable /run/rss -- run the suite under unshare -rm");

		ASSERT_EQm(junk[i], RSS_SHADOW_LOCKED, rss_shadow_state(SHADOW_PATH, "root"));
	}
	PASS();
}

TEST an_absent_account_or_file_is_neither(void)
{
	if (!write_shadow(SHIPPED))
		SKIPm("no writable /run/rss -- run the suite under unshare -rm");

	ASSERT_EQ(RSS_SHADOW_MISSING, rss_shadow_state(SHADOW_PATH, "nosuchuser"));

	/* A name carrying the field separator cannot match a line and must not
	 * be allowed to look as though it had. */
	ASSERT_EQ(RSS_SHADOW_MISSING, rss_shadow_state(SHADOW_PATH, "root:x"));
	ASSERT_EQ(RSS_SHADOW_MISSING, rss_shadow_state(SHADOW_PATH, ""));

	unlink(SHADOW_PATH);
	ASSERT_EQ(RSS_SHADOW_MISSING, rss_shadow_state(SHADOW_PATH, "root"));
	PASS();
}

/*
 * A name that is a prefix of the account's must not match it. "roo" against a
 * file holding "root" would otherwise read root's field, which on a claimable
 * camera is the difference between an account that exists and one that does
 * not.
 */
TEST a_prefix_of_the_name_is_not_the_name(void)
{
	if (!write_shadow(SHIPPED))
		SKIPm("no writable /run/rss -- run the suite under unshare -rm");

	ASSERT_EQ(RSS_SHADOW_MISSING, rss_shadow_state(SHADOW_PATH, "roo"));
	ASSERT_EQ(RSS_SHADOW_MISSING, rss_shadow_state(SHADOW_PATH, "root2"));
	PASS();
}

/*
 * The first line naming the account wins, which is what the login stack does
 * with a duplicated entry. Answering from a later line would authenticate
 * against a hash nothing else on the camera uses.
 */
TEST the_first_line_for_an_account_is_the_one(void)
{
	if (!write_shadow("root::19477::::::\nroot:$5$abc$def:19477::::::\n"))
		SKIPm("no writable /run/rss -- run the suite under unshare -rm");

	ASSERT_EQ(RSS_SHADOW_UNSET, rss_shadow_state(SHADOW_PATH, "root"));
	PASS();
}

SUITE(shadow_suite)
{
	RUN_TEST(an_empty_field_is_a_camera_waiting_to_be_claimed);
	RUN_TEST(a_hash_is_a_camera_somebody_has);
	RUN_TEST(a_locked_account_is_not_an_unclaimed_one);
	RUN_TEST(a_field_that_is_not_a_hash_is_refused);
	RUN_TEST(an_absent_account_or_file_is_neither);
	RUN_TEST(a_prefix_of_the_name_is_not_the_name);
	RUN_TEST(the_first_line_for_an_account_is_the_one);
}
