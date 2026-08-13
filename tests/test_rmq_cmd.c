/*
 * test_rmq_cmd.c -- Unit tests for the MQTT command policy
 *
 * The command topic is the only route from the network into a daemon, so what
 * is under test here is a refusal as much as an acceptance. Two properties
 * carry the weight:
 *
 *   - deny by default, so a command absent from the allowlist cannot reach a
 *     control socket however it is spelled; and
 *   - rebuild rather than relay, so a field the allowlist does not name is
 *     not carried across even when it rides alongside a permitted command.
 *
 * The second is what makes the first hold as the table grows: a new entry
 * cannot accidentally widen the surface by admitting a field nobody listed.
 */
#include <stdio.h>
#include <string.h>

#include "greatest.h"
#include "../rmq/rmq_cmd.h"

static char err[192];

static int plan(const char *json, rmq_cmd_plan_t *p)
{
	err[0] = '\0';
	return rmq_cmd_plan(json, p, err, sizeof(err));
}

/* Refused, whatever the reason. */
#define ASSERT_REFUSED(json)                                                                       \
	do {                                                                                       \
		rmq_cmd_plan_t p_;                                                                 \
		ASSERT_EQm(json, -1, plan((json), &p_));                                           \
		ASSERT(err[0] != '\0');                                                            \
	} while (0)

#define ASSERT_ALLOWED(json, p)                                                                    \
	do {                                                                                       \
		ASSERT_EQm(err, 0, plan((json), (p)));                                             \
	} while (0)

/* ------------------------------------------------------------------ */
/* Deny by default                                                     */
/* ------------------------------------------------------------------ */

/*
 * The three the design called out by name. `save` takes a caller-chosen path
 * and would write a snapshot anywhere the daemon can reach; `shutdown` stops
 * a daemon from the network; and a section read is how a broker client would
 * otherwise walk off with the RTSP and HTTP passwords.
 */
TEST refuses_the_named_hazards(void)
{
	ASSERT_REFUSED("{\"cmd\":\"save\",\"format\":\"jpeg\",\"file\":\"/etc/passwd\"}");
	ASSERT_REFUSED("{\"cmd\":\"shutdown\"}");
	ASSERT_REFUSED("{\"cmd\":\"restart\"}");
	ASSERT_REFUSED("{\"cmd\":\"config-get-section\",\"section\":\"rtsp\"}");
	ASSERT_REFUSED("{\"cmd\":\"config-get-section\",\"section\":\"http\"}");
	ASSERT_REFUSED("{\"cmd\":\"config-get-section\",\"section\":\"webrtc\"}");
	ASSERT_REFUSED("{\"cmd\":\"config-get-section\",\"section\":\"mqtt\"}");
	PASS();
}

/* Everything else a daemon would answer but nobody put on the list. */
TEST refuses_unlisted_daemon_commands(void)
{
	ASSERT_REFUSED("{\"cmd\":\"config-get\",\"section\":\"mqtt\",\"key\":\"password\"}");
	ASSERT_REFUSED("{\"cmd\":\"config-save\"}");
	ASSERT_REFUSED("{\"cmd\":\"set-affinity\",\"cpu\":0}");
	ASSERT_REFUSED("{\"cmd\":\"set-log-level\",\"value\":\"trace\"}");
	ASSERT_REFUSED("{\"cmd\":\"stream-stop\",\"channel\":0}");
	ASSERT_REFUSED("{\"cmd\":\"set-resolution\",\"channel\":0,\"width\":1,\"height\":1}");
	ASSERT_REFUSED("{\"cmd\":\"osd-restart\"}");
	ASSERT_REFUSED("{\"cmd\":\"privacy\",\"value\":\"on\"}");
	PASS();
}

/* Case and whitespace are not a way in: the lookup is exact. */
TEST refuses_near_misses(void)
{
	ASSERT_REFUSED("{\"cmd\":\"SHUTDOWN\"}");
	ASSERT_REFUSED("{\"cmd\":\"Set-Bitrate\",\"channel\":0,\"value\":2000000}");
	ASSERT_REFUSED("{\"cmd\":\" set-bitrate\",\"channel\":0,\"value\":2000000}");
	ASSERT_REFUSED("{\"cmd\":\"set-bitrate \",\"channel\":0,\"value\":2000000}");
	ASSERT_REFUSED("{\"cmd\":\"\"}");
	PASS();
}

TEST refuses_malformed_payloads(void)
{
	ASSERT_REFUSED("");
	ASSERT_REFUSED("not json");
	ASSERT_REFUSED("[1,2,3]");
	ASSERT_REFUSED("\"set-bitrate\"");
	ASSERT_REFUSED("{}");
	ASSERT_REFUSED("{\"cmd\":123}");
	ASSERT_REFUSED("{\"cmd\":null}");
	ASSERT_REFUSED("{\"cmd\":{\"cmd\":\"set-bitrate\"}}");
	PASS();
}

/* ------------------------------------------------------------------ */
/* Rebuild rather than relay                                           */
/* ------------------------------------------------------------------ */

/*
 * The load-bearing one. A permitted command carrying an extra field must
 * reach the daemon without it — this is what stops a hazard being smuggled
 * in beside something harmless, and it holds without anyone having to
 * enumerate the hazards.
 */
TEST drops_fields_the_table_does_not_name(void)
{
	rmq_cmd_plan_t p;
	ASSERT_ALLOWED("{\"cmd\":\"set-bitrate\",\"channel\":0,\"value\":2000000,"
		       "\"file\":\"/etc/shadow\",\"format\":\"jpeg\",\"cpu\":1,\"nonce\":\"n1\"}",
		       &p);

	ASSERT(strstr(p.request, "set-bitrate") != NULL);
	ASSERT(strstr(p.request, "2000000") != NULL);
	ASSERT_EQ(NULL, strstr(p.request, "file"));
	ASSERT_EQ(NULL, strstr(p.request, "shadow"));
	ASSERT_EQ(NULL, strstr(p.request, "format"));
	ASSERT_EQ(NULL, strstr(p.request, "cpu"));
	/* The nonce belongs to the reply, not to the daemon. */
	ASSERT_EQ(NULL, strstr(p.request, "nonce"));
	PASS();
}

/* A second "cmd" cannot survive into the request either: it is written from
 * the table, so whatever the payload said is overwritten rather than merged. */
TEST rewrites_the_command_name(void)
{
	rmq_cmd_plan_t p;

	ASSERT_ALLOWED("{\"cmd\":\"ircut-mode\",\"value\":\"day\"}", &p);
	ASSERT_STR_EQ("ric", p.daemon);
	ASSERT(strstr(p.request, "\"cmd\":\"mode\"") != NULL);
	ASSERT_EQ(NULL, strstr(p.request, "ircut-mode"));

	ASSERT_ALLOWED("{\"cmd\":\"osd-enable\"}", &p);
	ASSERT_STR_EQ("rod", p.daemon);
	ASSERT(strstr(p.request, "\"cmd\":\"enable\"") != NULL);
	PASS();
}

/* ------------------------------------------------------------------ */
/* Field validation                                                    */
/* ------------------------------------------------------------------ */

TEST enforces_integer_ranges(void)
{
	rmq_cmd_plan_t p;

	ASSERT_ALLOWED("{\"cmd\":\"set-bitrate\",\"channel\":0,\"value\":2000000}", &p);
	ASSERT_STR_EQ("rvd", p.daemon);

	ASSERT_REFUSED("{\"cmd\":\"set-bitrate\",\"channel\":0,\"value\":0}");
	ASSERT_REFUSED("{\"cmd\":\"set-bitrate\",\"channel\":0,\"value\":-2000000}");
	ASSERT_REFUSED("{\"cmd\":\"set-bitrate\",\"channel\":0,\"value\":99999999999}");
	ASSERT_REFUSED("{\"cmd\":\"set-bitrate\",\"channel\":-1,\"value\":2000000}");
	ASSERT_REFUSED("{\"cmd\":\"set-bitrate\",\"channel\":4,\"value\":2000000}");
	PASS();
}

/*
 * Range is checked before the value is narrowed to int. Checked after, a
 * magnitude past 2^31 would wrap into the permitted band and pass — which is
 * the classic way a bounds check that reads correctly still lets a value
 * through.
 */
TEST rejects_values_that_would_wrap_an_int(void)
{
	ASSERT_REFUSED("{\"cmd\":\"set-gop\",\"channel\":0,\"value\":4294967297}");
	ASSERT_REFUSED("{\"cmd\":\"set-gop\",\"channel\":0,\"value\":8589934592}");
	ASSERT_REFUSED("{\"cmd\":\"set-gop\",\"channel\":0,\"value\":1e300}");
	ASSERT_REFUSED("{\"cmd\":\"set-gop\",\"channel\":0,\"value\":-1e300}");
	PASS();
}

/* A number field takes a number. A string that looks like one is still a
 * string, and accepting it would mean guessing at a conversion. */
TEST enforces_field_types(void)
{
	ASSERT_REFUSED("{\"cmd\":\"set-bitrate\",\"channel\":0,\"value\":\"2000000\"}");
	ASSERT_REFUSED("{\"cmd\":\"set-bitrate\",\"channel\":\"0\",\"value\":2000000}");
	ASSERT_REFUSED("{\"cmd\":\"set-bitrate\",\"channel\":0,\"value\":true}");
	ASSERT_REFUSED("{\"cmd\":\"set-bitrate\",\"channel\":0,\"value\":[2000000]}");
	ASSERT_REFUSED("{\"cmd\":\"set-bitrate\",\"channel\":0,\"value\":2000000.5}");
	ASSERT_REFUSED("{\"cmd\":\"ircut-mode\",\"value\":1}");
	PASS();
}

TEST enforces_enum_choices(void)
{
	rmq_cmd_plan_t p;

	ASSERT_ALLOWED("{\"cmd\":\"ircut-mode\",\"value\":\"auto\"}", &p);
	ASSERT_ALLOWED("{\"cmd\":\"ircut-mode\",\"value\":\"night\"}", &p);

	ASSERT_REFUSED("{\"cmd\":\"ircut-mode\",\"value\":\"DAY\"}");
	ASSERT_REFUSED("{\"cmd\":\"ircut-mode\",\"value\":\"day \"}");
	ASSERT_REFUSED("{\"cmd\":\"ircut-mode\",\"value\":\"\"}");
	ASSERT_REFUSED("{\"cmd\":\"ircut-mode\",\"value\":\"day;reboot\"}");
	ASSERT_REFUSED("{\"cmd\":\"ir850\",\"value\":\"maybe\"}");
	PASS();
}

TEST requires_required_fields(void)
{
	ASSERT_REFUSED("{\"cmd\":\"set-bitrate\",\"channel\":0}");
	ASSERT_REFUSED("{\"cmd\":\"set-bitrate\",\"value\":2000000}");
	ASSERT_REFUSED("{\"cmd\":\"set-bitrate\"}");
	ASSERT_REFUSED("{\"cmd\":\"ircut-mode\"}");
	ASSERT_REFUSED("{\"cmd\":\"set-qp-bounds\",\"channel\":0,\"min\":20}");
	/* An explicit null is an absent field, not a value. */
	ASSERT_REFUSED("{\"cmd\":\"set-bitrate\",\"channel\":0,\"value\":null}");
	PASS();
}

/* An optional field may be left out, and is carried when given. */
TEST optional_fields_are_optional(void)
{
	rmq_cmd_plan_t p;

	ASSERT_ALLOWED("{\"cmd\":\"request-idr\"}", &p);
	ASSERT_STR_EQ("rvd", p.daemon);
	ASSERT_EQ(NULL, strstr(p.request, "channel"));

	ASSERT_ALLOWED("{\"cmd\":\"request-idr\",\"channel\":1}", &p);
	ASSERT(strstr(p.request, "\"channel\":1") != NULL);

	ASSERT_REFUSED("{\"cmd\":\"request-idr\",\"channel\":9}");
	PASS();
}

/* ------------------------------------------------------------------ */
/* Section policy                                                      */
/* ------------------------------------------------------------------ */

/*
 * A readable section routes to the daemon that owns it. Every daemon parses
 * the whole file, so this is about freshness rather than access — a live
 * change not yet saved exists only in the daemon that made it.
 */
TEST routes_readable_sections_to_their_owner(void)
{
	rmq_cmd_plan_t p;

	ASSERT_ALLOWED("{\"cmd\":\"config-get-section\",\"section\":\"stream0\"}", &p);
	ASSERT_STR_EQ("rvd", p.daemon);

	ASSERT_ALLOWED("{\"cmd\":\"config-get-section\",\"section\":\"audio\"}", &p);
	ASSERT_STR_EQ("rad", p.daemon);

	ASSERT_ALLOWED("{\"cmd\":\"config-get-section\",\"section\":\"osd\"}", &p);
	ASSERT_STR_EQ("rod", p.daemon);

	ASSERT_ALLOWED("{\"cmd\":\"config-get-section\",\"section\":\"ircut\"}", &p);
	ASSERT_STR_EQ("ric", p.daemon);
	PASS();
}

TEST refuses_unreadable_and_invented_sections(void)
{
	ASSERT_REFUSED("{\"cmd\":\"config-get-section\",\"section\":\"push\"}");
	ASSERT_REFUSED("{\"cmd\":\"config-get-section\",\"section\":\"srt\"}");
	ASSERT_REFUSED("{\"cmd\":\"config-get-section\",\"section\":\"nonesuch\"}");
	ASSERT_REFUSED("{\"cmd\":\"config-get-section\",\"section\":\"\"}");
	ASSERT_REFUSED("{\"cmd\":\"config-get-section\",\"section\":\"AUDIO\"}");
	/* Nothing is concatenated into the request, but assert it anyway: the
	 * day someone builds it with sprintf, this is the test that fails. */
	ASSERT_REFUSED("{\"cmd\":\"config-get-section\",\"section\":\"audio\\\"}\"}");
	PASS();
}

/* ------------------------------------------------------------------ */
/* Persistence                                                         */
/* ------------------------------------------------------------------ */

/*
 * Which commands owe a config save is a property of the daemon on the far
 * side, not a preference here: rvd records a new bitrate in its config and
 * rod's pause is deliberately transient. Getting this wrong is silent —
 * either a setting is lost at reboot, or flash is written for something that
 * was never meant to outlive the session.
 */
TEST marks_only_persisting_commands(void)
{
	rmq_cmd_plan_t p;

	ASSERT_ALLOWED("{\"cmd\":\"set-bitrate\",\"channel\":0,\"value\":2000000}", &p);
	ASSERT(p.persists);
	ASSERT_ALLOWED("{\"cmd\":\"ircut-mode\",\"value\":\"day\"}", &p);
	ASSERT(p.persists);
	ASSERT_ALLOWED("{\"cmd\":\"set-volume\",\"value\":80}", &p);
	ASSERT(p.persists);

	ASSERT_ALLOWED("{\"cmd\":\"request-idr\"}", &p);
	ASSERT_FALSE(p.persists);
	ASSERT_ALLOWED("{\"cmd\":\"osd-disable\"}", &p);
	ASSERT_FALSE(p.persists);
	ASSERT_ALLOWED("{\"cmd\":\"ir850\",\"value\":\"on\"}", &p);
	ASSERT_FALSE(p.persists);
	ASSERT_ALLOWED("{\"cmd\":\"config-get-section\",\"section\":\"audio\"}", &p);
	ASSERT_FALSE(p.persists);
	PASS();
}

/* The two audio stages have different scales, and swapping them is easy to
 * do and hard to notice: 31 is full analog gain but a third of volume. */
TEST audio_stages_keep_their_own_ranges(void)
{
	rmq_cmd_plan_t p;

	ASSERT_ALLOWED("{\"cmd\":\"set-volume\",\"value\":100}", &p);
	ASSERT_ALLOWED("{\"cmd\":\"set-gain\",\"value\":31}", &p);
	ASSERT_REFUSED("{\"cmd\":\"set-volume\",\"value\":101}");
	ASSERT_REFUSED("{\"cmd\":\"set-gain\",\"value\":32}");
	PASS();
}

/* A refusal always leaves a message: it is published to whoever sent the
 * command, and an empty one would report a failure with no cause. */
TEST every_refusal_explains_itself(void)
{
	static const char *const refused[] = {
		"",
		"{}",
		"{\"cmd\":\"shutdown\"}",
		"{\"cmd\":\"set-bitrate\",\"channel\":0}",
		"{\"cmd\":\"set-bitrate\",\"channel\":0,\"value\":1}",
		"{\"cmd\":\"ircut-mode\",\"value\":\"noon\"}",
		"{\"cmd\":\"config-get-section\",\"section\":\"http\"}",
		NULL,
	};

	for (int i = 0; refused[i]; i++) {
		rmq_cmd_plan_t p;
		ASSERT_EQm(refused[i], -1, plan(refused[i], &p));
		ASSERTm(refused[i], strlen(err) > 0);
	}
	PASS();
}

SUITE(rmq_cmd_suite)
{
	RUN_TEST(refuses_the_named_hazards);
	RUN_TEST(refuses_unlisted_daemon_commands);
	RUN_TEST(refuses_near_misses);
	RUN_TEST(refuses_malformed_payloads);
	RUN_TEST(drops_fields_the_table_does_not_name);
	RUN_TEST(rewrites_the_command_name);
	RUN_TEST(enforces_integer_ranges);
	RUN_TEST(rejects_values_that_would_wrap_an_int);
	RUN_TEST(enforces_field_types);
	RUN_TEST(enforces_enum_choices);
	RUN_TEST(requires_required_fields);
	RUN_TEST(optional_fields_are_optional);
	RUN_TEST(routes_readable_sections_to_their_owner);
	RUN_TEST(refuses_unreadable_and_invented_sections);
	RUN_TEST(marks_only_persisting_commands);
	RUN_TEST(audio_stages_keep_their_own_ranges);
	RUN_TEST(every_refusal_explains_itself);
}
