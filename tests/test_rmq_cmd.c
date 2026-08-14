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

/* ------------------------------------------------------------------ */
/* Restart tier: config-set                                            */
/* ------------------------------------------------------------------ */

TEST refuses_config_writes_outside_the_key_table(void)
{
	static const char *const refused[] = {
		/* Credentials, in the two sections that hold them. Neither
		 * key is in the table, so neither is reachable. */
		"{\"cmd\":\"config-set\",\"section\":\"rtsp\",\"key\":\"password\","
		"\"value\":\"x\"}",
		"{\"cmd\":\"config-set\",\"section\":\"rtsp\",\"key\":\"username\","
		"\"value\":\"x\"}",
		"{\"cmd\":\"config-set\",\"section\":\"http\",\"key\":\"password\","
		"\"value\":\"x\"}",
		"{\"cmd\":\"config-set\",\"section\":\"webrtc\",\"key\":\"password\","
		"\"value\":\"x\"}",

		/* Paths and key material: no string type exists, so nothing
		 * that names a file can be written at all. */
		"{\"cmd\":\"config-set\",\"section\":\"recording\",\"key\":\"storage_path\","
		"\"value\":\"/etc\"}",
		"{\"cmd\":\"config-set\",\"section\":\"recording\",\"key\":\"sign_key\","
		"\"value\":\"/etc/x\"}",
		"{\"cmd\":\"config-set\",\"section\":\"rtsp\",\"key\":\"tls_key\","
		"\"value\":\"/etc/x\"}",
		"{\"cmd\":\"config-set\",\"section\":\"osd\",\"key\":\"font\","
		"\"value\":\"/tmp/f.ttf\"}",

		/* The bridge's own settings. Writing these could put the
		 * camera beyond reach of the topic that wrote them. */
		"{\"cmd\":\"config-set\",\"section\":\"mqtt\",\"key\":\"host\","
		"\"value\":\"10.0.0.1\"}",
		"{\"cmd\":\"config-set\",\"section\":\"mqtt\",\"key\":\"enabled\","
		"\"value\":false}",

		/* Sections with no owner in the write map. */
		"{\"cmd\":\"config-set\",\"section\":\"push\",\"key\":\"url\","
		"\"value\":\"rtmp://x\"}",
		"{\"cmd\":\"config-set\",\"section\":\"webtorrent\",\"key\":\"tracker\","
		"\"value\":\"wss://x\"}",
		"{\"cmd\":\"config-set\",\"section\":\"nonesuch\",\"key\":\"enabled\","
		"\"value\":true}",

		/* A real section, an invented key. */
		"{\"cmd\":\"config-set\",\"section\":\"audio\",\"key\":\"volume_boost\","
		"\"value\":9}",
		NULL,
	};

	for (int i = 0; refused[i]; i++)
		ASSERT_REFUSED(refused[i]);
	PASS();
}

TEST accepts_a_single_write_and_routes_it(void)
{
	rmq_cmd_plan_t p;

	ASSERT_ALLOWED("{\"cmd\":\"config-set\",\"section\":\"rtsp\",\"key\":\"port\","
		       "\"value\":5554}",
		       &p);
	ASSERT_EQ(RMQ_PLAN_CONFIG, p.kind);
	ASSERT_EQ(1, p.write_count);
	ASSERT_EQ(RMQ_D_RSD, p.restart_owner);
	ASSERT_STR_EQ("rtsp", p.writes[0].section);
	ASSERT_STR_EQ("port", p.writes[0].key);
	ASSERT_STR_EQ("5554", p.writes[0].value);
	PASS();
}

TEST renders_every_value_as_the_file_spells_it(void)
{
	rmq_cmd_plan_t p;

	/* A boolean, however the sender wrote it. */
	ASSERT_ALLOWED("{\"cmd\":\"config-set\",\"section\":\"motion\",\"key\":\"enabled\","
		       "\"value\":true}",
		       &p);
	ASSERT_STR_EQ("true", p.writes[0].value);
	ASSERT_ALLOWED("{\"cmd\":\"config-set\",\"section\":\"motion\",\"key\":\"enabled\","
		       "\"value\":0}",
		       &p);
	ASSERT_STR_EQ("false", p.writes[0].value);

	/* A numeric enum, whether quoted or not — a template may render
	 * either, and both have to land on the table's own spelling. */
	ASSERT_ALLOWED("{\"cmd\":\"config-set\",\"section\":\"audio\",\"key\":\"sample_rate\","
		       "\"value\":\"48000\"}",
		       &p);
	ASSERT_STR_EQ("48000", p.writes[0].value);
	ASSERT_ALLOWED("{\"cmd\":\"config-set\",\"section\":\"audio\",\"key\":\"sample_rate\","
		       "\"value\":48000}",
		       &p);
	ASSERT_STR_EQ("48000", p.writes[0].value);

	/* An unlisted rate is refused rather than written through. */
	ASSERT_REFUSED("{\"cmd\":\"config-set\",\"section\":\"audio\",\"key\":\"sample_rate\","
		       "\"value\":44100}");
	PASS();
}

TEST enforces_types_and_ranges_on_writes(void)
{
	ASSERT_REFUSED("{\"cmd\":\"config-set\",\"section\":\"jpeg\",\"key\":\"quality\","
		       "\"value\":0}");
	ASSERT_REFUSED("{\"cmd\":\"config-set\",\"section\":\"jpeg\",\"key\":\"quality\","
		       "\"value\":101}");
	ASSERT_REFUSED("{\"cmd\":\"config-set\",\"section\":\"jpeg\",\"key\":\"quality\","
		       "\"value\":75.5}");
	ASSERT_REFUSED("{\"cmd\":\"config-set\",\"section\":\"jpeg\",\"key\":\"quality\","
		       "\"value\":\"75\"}");

	/* Past 2^31: rejected on the double, before any narrowing could wrap
	 * it back into range. */
	ASSERT_REFUSED("{\"cmd\":\"config-set\",\"section\":\"rtsp\",\"key\":\"port\","
		       "\"value\":4294967296}");

	/* A boolean key given something that is neither. */
	ASSERT_REFUSED("{\"cmd\":\"config-set\",\"section\":\"motion\",\"key\":\"enabled\","
		       "\"value\":2}");
	ASSERT_REFUSED("{\"cmd\":\"config-set\",\"section\":\"motion\",\"key\":\"enabled\","
		       "\"value\":\"yes\"}");

	/* An enum key given a plausible near-miss. */
	ASSERT_REFUSED("{\"cmd\":\"config-set\",\"section\":\"stream0\",\"key\":\"codec\","
		       "\"value\":\"hevc\"}");
	PASS();
}

TEST applies_a_values_map_all_or_nothing(void)
{
	rmq_cmd_plan_t p;

	ASSERT_ALLOWED("{\"cmd\":\"config-set\",\"section\":\"stream0\",\"values\":"
		       "{\"width\":1280,\"height\":720}}",
		       &p);
	ASSERT_EQ(2, p.write_count);
	ASSERT_EQ(RMQ_D_RVD, p.restart_owner);
	ASSERT_STR_EQ("1280", p.writes[0].value);
	ASSERT_STR_EQ("720", p.writes[1].value);

	/* One bad key refuses the whole map: half a resolution is a size
	 * nobody chose, and it would be the live one. */
	ASSERT_REFUSED("{\"cmd\":\"config-set\",\"section\":\"stream0\",\"values\":"
		       "{\"width\":1280,\"height\":99}}");
	ASSERT_REFUSED("{\"cmd\":\"config-set\",\"section\":\"stream0\",\"values\":"
		       "{\"width\":1280,\"nonesuch\":1}}");

	ASSERT_REFUSED("{\"cmd\":\"config-set\",\"section\":\"stream0\",\"values\":{}}");
	ASSERT_REFUSED("{\"cmd\":\"config-set\",\"section\":\"stream0\",\"values\":[1,2]}");
	PASS();
}

TEST names_writes_from_the_table_not_the_payload(void)
{
	rmq_cmd_plan_t p;

	ASSERT_ALLOWED("{\"cmd\":\"config-set\",\"section\":\"osd\",\"key\":\"font_size\","
		       "\"value\":32,\"file\":\"/etc/passwd\",\"nonce\":\"n1\"}",
		       &p);

	/* Only the named key crossed; the smuggled fields are not writes and
	 * cannot become ones. */
	ASSERT_EQ(1, p.write_count);
	ASSERT_STR_EQ("osd", p.writes[0].section);
	ASSERT_STR_EQ("font_size", p.writes[0].key);

	/* The strings stored are the table's, so a write cannot introduce a
	 * spelling this build does not know. */
	ASSERT(p.writes[0].section != NULL && p.writes[0].key != NULL);
	PASS();
}

TEST config_set_needs_a_section_and_a_key(void)
{
	ASSERT_REFUSED("{\"cmd\":\"config-set\"}");
	ASSERT_REFUSED("{\"cmd\":\"config-set\",\"section\":\"osd\"}");
	ASSERT_REFUSED("{\"cmd\":\"config-set\",\"key\":\"font_size\",\"value\":32}");
	ASSERT_REFUSED("{\"cmd\":\"config-set\",\"section\":\"osd\",\"key\":\"font_size\"}");
	ASSERT_REFUSED("{\"cmd\":\"config-set\",\"section\":\"osd\",\"key\":\"font_size\","
		       "\"value\":null}");
	ASSERT_REFUSED("{\"cmd\":\"config-set\",\"section\":5,\"key\":\"font_size\",\"value\":9}");
	PASS();
}

TEST every_writable_section_has_an_owner(void)
{
	/* Each section named in the key table must resolve to a daemon, or a
	 * write would be staged that nothing ever restarts. Sampled through
	 * the planner, which is the only door in. */
	static const struct {
		const char *json;
		rmq_daemon_t owner;
	} cases[] = {
		{"{\"cmd\":\"config-set\",\"section\":\"sensor\",\"key\":\"fps\",\"value\":25}",
		 RMQ_D_RVD},
		{"{\"cmd\":\"config-set\",\"section\":\"jpeg\",\"key\":\"quality\",\"value\":80}",
		 RMQ_D_RVD},
		{"{\"cmd\":\"config-set\",\"section\":\"audio\",\"key\":\"enabled\","
		 "\"value\":true}",
		 RMQ_D_RAD},
		{"{\"cmd\":\"config-set\",\"section\":\"http\",\"key\":\"port\",\"value\":8081}",
		 RMQ_D_RHD},
		{"{\"cmd\":\"config-set\",\"section\":\"osd\",\"key\":\"font_size\","
		 "\"value\":20}",
		 RMQ_D_ROD},
		{"{\"cmd\":\"config-set\",\"section\":\"ircut\",\"key\":\"pulse_ms\","
		 "\"value\":20}",
		 RMQ_D_RIC},
		{"{\"cmd\":\"config-set\",\"section\":\"motion\",\"key\":\"sensitivity\","
		 "\"value\":3}",
		 RMQ_D_RMD},
		{"{\"cmd\":\"config-set\",\"section\":\"recording\",\"key\":\"stream\","
		 "\"value\":1}",
		 RMQ_D_RMR},
		{"{\"cmd\":\"config-set\",\"section\":\"timelapse\",\"key\":\"interval\","
		 "\"value\":30}",
		 RMQ_D_RMR},
		{NULL, RMQ_D_COUNT},
	};

	for (int i = 0; cases[i].json; i++) {
		rmq_cmd_plan_t p;
		ASSERT_ALLOWED(cases[i].json, &p);
		ASSERT_EQm(cases[i].json, cases[i].owner, p.restart_owner);
		ASSERTm(cases[i].json, p.restart_owner != RMQ_D_COUNT);
	}
	PASS();
}

TEST live_commands_stay_out_of_the_config_plan(void)
{
	rmq_cmd_plan_t p;

	/* A live command still produces a daemon request and no writes, so
	 * the two tiers cannot be confused by whoever handles the plan. */
	ASSERT_ALLOWED("{\"cmd\":\"set-volume\",\"value\":50}", &p);
	ASSERT_EQ(RMQ_PLAN_DAEMON, p.kind);
	ASSERT_EQ(0, p.write_count);
	PASS();
}

/*
 * [image] is split across both tiers: the keys a running ISP channel refuses
 * are config writes, and every other tuning knob stays a live command. A key
 * landing in the wrong tier is the failure this guards — a live setter routed
 * through the file would restart the video to change the brightness, and a
 * creation-time key left live would report success and change nothing.
 */
TEST splits_the_image_section_across_the_two_tiers(void)
{
	rmq_cmd_plan_t p;

	ASSERT_ALLOWED("{\"cmd\":\"config-set\",\"section\":\"image\",\"key\":\"hflip\","
		       "\"value\":1}",
		       &p);
	ASSERT_EQ(RMQ_PLAN_CONFIG, p.kind);
	ASSERT_EQ(RMQ_D_RVD, p.restart_owner);
	ASSERT_EQ(1, p.write_count);
	ASSERT_STR_EQ("image", p.writes[0].section);
	ASSERT_STR_EQ("hflip", p.writes[0].key);
	ASSERT_STR_EQ("1", p.writes[0].value);

	/*
	 * Orientation reaches the file as a number, never as `true`. rvd reads
	 * it with rss_config_get_int, where `true` is not a number and falls
	 * back to the default — a flip that writes cleanly, restarts the video
	 * and changes nothing. Typing it as V_BOOL here is what produced that.
	 */
	ASSERT_REFUSED("{\"cmd\":\"config-set\",\"section\":\"image\",\"key\":\"hflip\","
		       "\"value\":true}");

	ASSERT_ALLOWED("{\"cmd\":\"config-set\",\"section\":\"image\",\"key\":\"temper\","
		       "\"value\":200}",
		       &p);
	ASSERT_STR_EQ("200", p.writes[0].value);

	/* Brightness is live, so the file is not where it goes. */
	ASSERT_REFUSED("{\"cmd\":\"config-set\",\"section\":\"image\",\"key\":\"brightness\","
		       "\"value\":128}");
	ASSERT_ALLOWED("{\"cmd\":\"set-brightness\",\"value\":128}", &p);
	ASSERT_EQ(RMQ_PLAN_DAEMON, p.kind);
	ASSERT_EQ(0, p.write_count);
	PASS();
}

/*
 * `snapshot` is the bridge's own work, like config-set: it asks no daemon, so
 * it must not acquire a daemon or a request on the way through. It also takes
 * no arguments — which ring and how often are config, and a broker client that
 * could pick the ring could pick the 600 KB one every second.
 */
TEST snapshot_is_the_bridges_own_work(void)
{
	rmq_cmd_plan_t p;

	ASSERT_ALLOWED("{\"cmd\":\"snapshot\"}", &p);
	ASSERT_EQ(RMQ_PLAN_SNAPSHOT, p.kind);
	ASSERT_EQ(0, p.write_count);

	/* Extra fields are dropped rather than honoured, the same as any other
	 * command — there is no argument that reaches the capture. */
	ASSERT_ALLOWED("{\"cmd\":\"snapshot\",\"stream\":0,\"channel\":2}", &p);
	ASSERT_EQ(RMQ_PLAN_SNAPSHOT, p.kind);
	PASS();
}

/*
 * The /etc tier. Its two keys need a string to travel, which is exactly what
 * the config tier refuses to have a type for — so the check is that each key
 * carries its own grammar rather than opening a general one.
 */
TEST system_set_carries_a_grammar_per_key(void)
{
	rmq_cmd_plan_t p;

	ASSERT_ALLOWED("{\"cmd\":\"system-set\",\"key\":\"timezone\","
		       "\"value\":\"America/New_York\"}",
		       &p);
	ASSERT_EQ(RMQ_PLAN_SYSTEM, p.kind);
	ASSERT_EQ(1, p.write_count);
	ASSERT_STR_EQ("system", p.writes[0].section);
	ASSERT_STR_EQ("timezone", p.writes[0].key);
	ASSERT_STR_EQ("America/New_York", p.writes[0].value);

	/* A timezone is an enum: a POSIX string is not a member even though it
	 * is what ends up in the file, so nothing free-form reaches /etc/TZ. */
	ASSERT_REFUSED("{\"cmd\":\"system-set\",\"key\":\"timezone\",\"value\":\"EST5EDT\"}");
	ASSERT_REFUSED("{\"cmd\":\"system-set\",\"key\":\"timezone\",\"value\":\"Mars/Olympus\"}");

	ASSERT_ALLOWED("{\"cmd\":\"system-set\",\"key\":\"ntp_server\","
		       "\"value\":\"192.168.1.254\"}",
		       &p);
	ASSERT_STR_EQ("192.168.1.254", p.writes[0].value);
	ASSERT_ALLOWED("{\"cmd\":\"system-set\",\"key\":\"ntp_server\","
		       "\"value\":\"pool.ntp.org\"}",
		       &p);

	/*
	 * The host grammar is the whole defence for the one real string in the
	 * bridge. It is written into ntp.conf on a line of its own, so a value
	 * carrying whitespace could add a second directive, and one carrying a
	 * slash or a quote could become something other than a hostname.
	 */
	ASSERT_REFUSED("{\"cmd\":\"system-set\",\"key\":\"ntp_server\","
		       "\"value\":\"host\\nrestrict default\"}");
	ASSERT_REFUSED("{\"cmd\":\"system-set\",\"key\":\"ntp_server\",\"value\":\"a b\"}");
	ASSERT_REFUSED(
		"{\"cmd\":\"system-set\",\"key\":\"ntp_server\",\"value\":\"../../etc/passwd\"}");
	ASSERT_REFUSED("{\"cmd\":\"system-set\",\"key\":\"ntp_server\",\"value\":\"h;reboot\"}");
	ASSERT_REFUSED("{\"cmd\":\"system-set\",\"key\":\"ntp_server\",\"value\":\"$(reboot)\"}");
	ASSERT_REFUSED("{\"cmd\":\"system-set\",\"key\":\"ntp_server\",\"value\":\"\"}");
	ASSERT_REFUSED("{\"cmd\":\"system-set\",\"key\":\"ntp_server\",\"value\":\"-lead\"}");
	ASSERT_REFUSED("{\"cmd\":\"system-set\",\"key\":\"ntp_server\",\"value\":\"trail.\"}");

	/* Nothing else in /etc is reachable — hostname and resolv.conf are out
	 * of scope, and being out of scope means being absent, not defended. */
	ASSERT_REFUSED("{\"cmd\":\"system-set\",\"key\":\"hostname\",\"value\":\"cam1\"}");
	ASSERT_REFUSED("{\"cmd\":\"system-set\",\"key\":\"nameserver\",\"value\":\"1.1.1.1\"}");
	ASSERT_REFUSED("{\"cmd\":\"system-set\",\"key\":\"timezone\"}");
	ASSERT_REFUSED("{\"cmd\":\"system-set\",\"value\":\"UTC\"}");
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

	RUN_TEST(refuses_config_writes_outside_the_key_table);
	RUN_TEST(accepts_a_single_write_and_routes_it);
	RUN_TEST(renders_every_value_as_the_file_spells_it);
	RUN_TEST(enforces_types_and_ranges_on_writes);
	RUN_TEST(applies_a_values_map_all_or_nothing);
	RUN_TEST(names_writes_from_the_table_not_the_payload);
	RUN_TEST(config_set_needs_a_section_and_a_key);
	RUN_TEST(every_writable_section_has_an_owner);
	RUN_TEST(live_commands_stay_out_of_the_config_plan);
	RUN_TEST(splits_the_image_section_across_the_two_tiers);
	RUN_TEST(snapshot_is_the_bridges_own_work);
	RUN_TEST(system_set_carries_a_grammar_per_key);
}
