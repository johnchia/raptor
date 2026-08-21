/*
 * test_rcd_cmd.c -- Unit tests for the config daemon's policy
 *
 * rcd's table is the only route from any network transport into the
 * configuration, so what is under test here is a refusal as much as an
 * acceptance. Three properties carry the weight:
 *
 *   - deny by default, so a key or an action absent from the table cannot
 *     reach a control socket however it is spelled;
 *   - rebuild rather than relay, so a field the table does not name is not
 *     carried across even when it rides alongside a permitted action; and
 *   - the tier is the table's to decide, so no caller can turn a restart into
 *     a live change or the other way round by how it asks.
 *
 * The second is what makes the first hold as the table grows: a new entry
 * cannot accidentally widen the surface by admitting a field nobody listed.
 *
 * Everything here runs against the validator alone -- no socket, no config
 * file, no daemon. That split is deliberate: the policy is the half worth
 * testing, and it is worth being able to test it without a camera.
 */
#include <stdio.h>
#include <string.h>

#include "greatest.h"
#include "../rcd/rcd.h"
#include "../rcd/rcd_apply.h"
#include "../rcd/rcd_config.h"
#include "../rcd/rcd_guard.h"
#include "../rcd/rcd_network.h"
#include "../rcd/rcd_proto.h"
#include "../rcd/rcd_schema.h"
#include "../rcd/rcd_state.h"
#include "../rcd/rcd_system.h"

#include <sys/stat.h>
#include <unistd.h>

static char code[64];
static char reason[256];

/* Run the validator over a JSON string. Returns 0 when accepted, -1 when
 * refused, with `code` and `reason` filled from the refusal. */
static int validate_set(const char *json, rcd_edit_t *edits, int *count)
{
	code[0] = reason[0] = '\0';
	*count = 0;

	cJSON *root = cJSON_Parse(json);
	if (!root) {
		snprintf(code, sizeof(code), "%s", RCD_E_MALFORMED);
		snprintf(reason, sizeof(reason), "not JSON");
		return -1;
	}

	cJSON *refusal = rcd_set_validate(root, edits, count);
	cJSON_Delete(root);
	if (!refusal)
		return 0;

	const cJSON *c = cJSON_GetObjectItemCaseSensitive(refusal, "code");
	const cJSON *r = cJSON_GetObjectItemCaseSensitive(refusal, "reason");
	if (cJSON_IsString(c))
		snprintf(code, sizeof(code), "%s", c->valuestring);
	if (cJSON_IsString(r))
		snprintf(reason, sizeof(reason), "%s", r->valuestring);
	cJSON_Delete(refusal);
	return -1;
}

static int validate_action(const char *json, char *wire, size_t wiresz, const char **owner)
{
	code[0] = reason[0] = '\0';
	wire[0] = '\0';
	*owner = NULL;

	cJSON *root = cJSON_Parse(json);
	if (!root) {
		snprintf(code, sizeof(code), "%s", RCD_E_MALFORMED);
		return -1;
	}

	cJSON *refusal = rcd_action_validate(root, wire, wiresz, owner);
	cJSON_Delete(root);
	if (!refusal)
		return 0;

	const cJSON *c = cJSON_GetObjectItemCaseSensitive(refusal, "code");
	const cJSON *r = cJSON_GetObjectItemCaseSensitive(refusal, "reason");
	if (cJSON_IsString(c))
		snprintf(code, sizeof(code), "%s", c->valuestring);
	if (cJSON_IsString(r))
		snprintf(reason, sizeof(reason), "%s", r->valuestring);
	cJSON_Delete(refusal);
	return -1;
}

#define ASSERT_SET_REFUSED(json)                                                                   \
	do {                                                                                       \
		rcd_edit_t e_[RCD_EDITS_MAX];                                                      \
		int n_;                                                                            \
		ASSERT_EQm(json, -1, validate_set((json), e_, &n_));                               \
		ASSERT(code[0] != '\0');                                                           \
	} while (0)

#define ASSERT_SET_OK(json, e_, n_) ASSERT_EQm(reason, 0, validate_set((json), (e_), (n_)))

#define ASSERT_ACTION_REFUSED(json)                                                                \
	do {                                                                                       \
		char w_[RCD_REQ_MAX];                                                              \
		const char *o_;                                                                    \
		ASSERT_EQm(json, -1, validate_action((json), w_, sizeof(w_), &o_));                \
		ASSERT(code[0] != '\0');                                                           \
	} while (0)

/* ------------------------------------------------------------------ */
/* Deny by default                                                     */
/* ------------------------------------------------------------------ */

/*
 * The hazards the design called out by name. `save` takes a caller-chosen path
 * and would write a snapshot anywhere the daemon can reach; `shutdown` stops a
 * daemon from the network; `restart` on a daemon socket is not an action but a
 * protocol command with a bracket around it.
 */
TEST refuses_the_named_hazards(void)
{
	ASSERT_ACTION_REFUSED("{\"action\":\"save\",\"format\":\"jpeg\",\"file\":\"/etc/passwd\"}");
	ASSERT_ACTION_REFUSED("{\"action\":\"shutdown\"}");
	ASSERT_ACTION_REFUSED("{\"action\":\"restart\"}");
	ASSERT_ACTION_REFUSED("{\"action\":\"config-save\"}");
	PASS();
}

/* A section holding a credential is not readable, and neither is one that was
 * never a section. From out here they are the same answer. */
TEST keeps_credential_sections_unreadable(void)
{
	/* Sections no daemon answers for. A section absent from the table
	 * entirely is unreadable for the same reason. */
	ASSERT_EQ(NULL, rcd_section_reader("webrtc"));
	ASSERT_EQ(NULL, rcd_section_reader("mqtt"));
	ASSERT_EQ(NULL, rcd_section_reader("not-a-section"));

	/* Readable ones route to their owner. [rtsp] and [http] are among
	 * them despite holding a password: the section being unreadable was
	 * never what protected it, and it cost every client the truth about
	 * whether the server was running. */
	ASSERT_STR_EQ("rvd", rcd_section_reader("image"));
	ASSERT_STR_EQ("rad", rcd_section_reader("audio"));
	ASSERT_STR_EQ("ric", rcd_section_reader("ircut"));
	ASSERT_STR_EQ("rsd", rcd_section_reader("rtsp"));
	ASSERT_STR_EQ("rhd", rcd_section_reader("http"));
	PASS();
}

/*
 * What actually keeps a password off the wire: every credential in the table
 * is marked unreadable in the schema, whatever section it sits in and whether
 * or not a daemon would happily hand it over. Walked over the whole table so
 * a credential added to a readable section cannot quietly become reportable.
 */
TEST no_credential_is_ever_readable(void)
{
	cJSON *out = cJSON_CreateObject();
	rcd_schema_emit(out, NULL);
	const cJSON *keys = cJSON_GetObjectItemCaseSensitive(out, "keys");
	ASSERT(cJSON_IsArray(keys));

	int creds = 0;
	const cJSON *k = NULL;
	cJSON_ArrayForEach(k, keys)
	{
		const cJSON *type = cJSON_GetObjectItemCaseSensitive(k, "type");
		if (!cJSON_IsString(type) || strcmp(type->valuestring, "credential") != 0)
			continue;
		const cJSON *r = cJSON_GetObjectItemCaseSensitive(k, "readable");
		ASSERT(cJSON_IsFalse(r));
		creds++;
	}
	ASSERT_EQ(4, creds); /* a username and a password for [rtsp] and [http] */
	cJSON_Delete(out);
	PASS();
}

TEST refuses_unlisted_actions(void)
{
	ASSERT_ACTION_REFUSED("{\"action\":\"set-brightness\",\"value\":10}");
	ASSERT_ACTION_REFUSED("{\"action\":\"reboot\"}");
	ASSERT_ACTION_REFUSED("{\"action\":\"\"}");
	ASSERT_ACTION_REFUSED("{}");
	PASS();
}

TEST refuses_near_misses(void)
{
	ASSERT_ACTION_REFUSED("{\"action\":\"ircut-mode \"}");
	ASSERT_ACTION_REFUSED("{\"action\":\"IRCUT-MODE\",\"value\":\"day\"}");
	ASSERT_ACTION_REFUSED("{\"action\":\"osd-enable2\"}");
	PASS();
}

TEST refuses_malformed_payloads(void)
{
	ASSERT_SET_REFUSED("not json at all");
	ASSERT_SET_REFUSED("[]");
	ASSERT_SET_REFUSED("{}");
	ASSERT_SET_REFUSED("{\"edits\":[]}");
	ASSERT_SET_REFUSED("{\"edits\":{}}");
	ASSERT_SET_REFUSED("{\"edits\":[{\"section\":\"image\"}]}");
	ASSERT_SET_REFUSED("{\"edits\":[{\"key\":\"brightness\",\"value\":1}]}");
	PASS();
}

/* ------------------------------------------------------------------ */
/* Rebuild, never relay                                                */
/* ------------------------------------------------------------------ */

TEST drops_fields_the_table_does_not_name(void)
{
	char wire[RCD_REQ_MAX];
	const char *owner = NULL;

	ASSERT_EQ(0, validate_action("{\"action\":\"request-idr\",\"channel\":1,"
				     "\"file\":\"/etc/shadow\",\"nonce\":\"x\"}",
				     wire, sizeof(wire), &owner));
	ASSERT(strstr(wire, "\"channel\":1") != NULL);
	ASSERT_EQ(NULL, strstr(wire, "file"));
	ASSERT_EQ(NULL, strstr(wire, "shadow"));
	ASSERT_EQ(NULL, strstr(wire, "nonce"));
	PASS();
}

/* The name the transport uses and the name the daemon answers to are allowed
 * to differ, and the table is what maps them. */
TEST rewrites_the_action_name(void)
{
	char wire[RCD_REQ_MAX];
	const char *owner = NULL;

	ASSERT_EQ(0, validate_action("{\"action\":\"ircut-mode\",\"value\":\"night\"}", wire,
				     sizeof(wire), &owner));
	ASSERT_STR_EQ("ric", owner);
	ASSERT(strstr(wire, "\"cmd\":\"mode\"") != NULL);
	ASSERT_EQ(NULL, strstr(wire, "ircut-mode"));
	PASS();
}

/* An enum's value is written from the table's own copy, so no byte of the
 * payload reaches a daemon even when it compares equal. */
TEST enforces_enum_choices(void)
{
	char wire[RCD_REQ_MAX];
	const char *owner = NULL;

	ASSERT_EQ(0, validate_action("{\"action\":\"ir850\",\"value\":\"on\"}", wire, sizeof(wire),
				     &owner));
	ASSERT(strstr(wire, "\"value\":\"on\"") != NULL);

	ASSERT_ACTION_REFUSED("{\"action\":\"ir850\",\"value\":\"ON\"}");
	ASSERT_ACTION_REFUSED("{\"action\":\"ir850\",\"value\":\"yes\"}");
	ASSERT_ACTION_REFUSED("{\"action\":\"ircut-mode\",\"value\":\"dusk\"}");
	ASSERT_STR_EQ(RCD_E_CHOICE, code);
	PASS();
}

TEST enforces_action_ranges_and_types(void)
{
	ASSERT_ACTION_REFUSED("{\"action\":\"request-idr\",\"channel\":9}");
	ASSERT_STR_EQ(RCD_E_RANGE, code);
	ASSERT_ACTION_REFUSED("{\"action\":\"request-idr\",\"channel\":-1}");
	ASSERT_ACTION_REFUSED("{\"action\":\"request-idr\",\"channel\":\"0\"}");
	ASSERT_STR_EQ(RCD_E_TYPE, code);
	ASSERT_ACTION_REFUSED("{\"action\":\"request-idr\",\"channel\":1.5}");
	ASSERT_STR_EQ(RCD_E_TYPE, code);
	PASS();
}

/* A number far outside int wraps into range once narrowed, so the bound has to
 * be checked while the value is still a double. */
TEST rejects_values_that_would_wrap_an_int(void)
{
	ASSERT_ACTION_REFUSED("{\"action\":\"set-qp-bounds\",\"channel\":0,"
			      "\"min\":4294967296,\"max\":10}");
	ASSERT_SET_REFUSED("{\"edits\":[{\"section\":\"image\",\"key\":\"brightness\","
			   "\"value\":4294967296}]}");
	ASSERT_SET_REFUSED("{\"edits\":[{\"section\":\"image\",\"key\":\"brightness\","
			   "\"value\":-4294967296}]}");
	PASS();
}

TEST requires_required_fields_and_allows_optional_ones(void)
{
	char wire[RCD_REQ_MAX];
	const char *owner = NULL;

	ASSERT_ACTION_REFUSED("{\"action\":\"set-qp-bounds\",\"channel\":0,\"min\":10}");
	ASSERT_ACTION_REFUSED("{\"action\":\"ircut-mode\"}");

	/* request-idr's channel is optional and its absence is not a refusal. */
	ASSERT_EQ(0, validate_action("{\"action\":\"request-idr\"}", wire, sizeof(wire), &owner));
	ASSERT_EQ(NULL, strstr(wire, "channel"));

	/* set-rc-mode's bitrate is optional beside two required fields. */
	ASSERT_EQ(0, validate_action("{\"action\":\"set-rc-mode\",\"channel\":0,"
				     "\"mode\":\"vbr\"}",
				     wire, sizeof(wire), &owner));
	ASSERT_EQ(NULL, strstr(wire, "bitrate"));
	PASS();
}

/* ------------------------------------------------------------------ */
/* The key table                                                       */
/* ------------------------------------------------------------------ */

/*
 * The keys deliberately absent: every path, format string and endpoint alias
 * in raptor.conf. None of them is refused by a rule naming it -- they are
 * refused because a key is unwritable until it is listed.
 */
TEST refuses_writes_outside_the_key_table(void)
{
	ASSERT_SET_REFUSED("{\"section\":\"recording\",\"key\":\"storage_path\","
			   "\"value\":\"/etc\"}");
	ASSERT_SET_REFUSED("{\"section\":\"recording\",\"key\":\"sign_key\",\"value\":\"/k\"}");
	ASSERT_SET_REFUSED("{\"section\":\"rtsp\",\"key\":\"tls_key\",\"value\":\"/k\"}");
	ASSERT_SET_REFUSED("{\"section\":\"osd\",\"key\":\"font\",\"value\":\"/f.ttf\"}");
	ASSERT_SET_REFUSED("{\"section\":\"mqtt\",\"key\":\"host\",\"value\":\"evil\"}");
	ASSERT_SET_REFUSED("{\"section\":\"mqtt\",\"key\":\"enabled\",\"value\":false}");
	ASSERT_SET_REFUSED("{\"section\":\"push\",\"key\":\"url\",\"value\":\"rtmp://x\"}");
	ASSERT_SET_REFUSED("{\"section\":\"webrtc\",\"key\":\"password\",\"value\":\"x\"}");
	ASSERT_STR_EQ(RCD_E_UNKNOWN, code);
	PASS();
}

TEST accepts_a_single_edit_in_either_shape(void)
{
	rcd_edit_t e[RCD_EDITS_MAX];
	int n = 0;

	ASSERT_SET_OK("{\"section\":\"jpeg\",\"key\":\"quality\",\"value\":80}", e, &n);
	ASSERT_EQ(1, n);
	ASSERT_STR_EQ("jpeg", e[0].k->section);
	ASSERT_STR_EQ("quality", e[0].k->key);
	ASSERT_STR_EQ("80", e[0].rendered);

	ASSERT_SET_OK("{\"edits\":[{\"section\":\"jpeg\",\"key\":\"quality\",\"value\":80}]}", e,
		      &n);
	ASSERT_EQ(1, n);
	ASSERT_STR_EQ("80", e[0].rendered);
	PASS();
}

/*
 * A whole form in one request, and all of it refused when any of it is.
 * A half-applied form is a configuration nobody chose.
 */
TEST applies_a_batch_all_or_nothing(void)
{
	rcd_edit_t e[RCD_EDITS_MAX];
	int n = 0;

	ASSERT_SET_OK("{\"edits\":["
		      "{\"section\":\"stream0\",\"key\":\"width\",\"value\":1920},"
		      "{\"section\":\"stream0\",\"key\":\"height\",\"value\":1080}]}",
		      e, &n);
	ASSERT_EQ(2, n);
	ASSERT_STR_EQ("1920", e[0].rendered);
	ASSERT_STR_EQ("1080", e[1].rendered);

	/* One bad member refuses the lot, wherever it sits in the array. */
	ASSERT_SET_REFUSED("{\"edits\":["
			   "{\"section\":\"stream0\",\"key\":\"width\",\"value\":1920},"
			   "{\"section\":\"stream0\",\"key\":\"height\",\"value\":99999}]}");
	ASSERT_SET_REFUSED("{\"edits\":["
			   "{\"section\":\"stream0\",\"key\":\"nope\",\"value\":1},"
			   "{\"section\":\"stream0\",\"key\":\"height\",\"value\":1080}]}");
	PASS();
}

TEST enforces_types_and_ranges_on_edits(void)
{
	ASSERT_SET_REFUSED("{\"section\":\"jpeg\",\"key\":\"quality\",\"value\":0}");
	ASSERT_SET_REFUSED("{\"section\":\"jpeg\",\"key\":\"quality\",\"value\":101}");
	ASSERT_STR_EQ(RCD_E_RANGE, code);
	ASSERT_SET_REFUSED("{\"section\":\"jpeg\",\"key\":\"quality\",\"value\":\"80\"}");
	ASSERT_STR_EQ(RCD_E_TYPE, code);
	ASSERT_SET_REFUSED("{\"section\":\"jpeg\",\"key\":\"enabled\",\"value\":\"yes\"}");
	ASSERT_SET_REFUSED("{\"section\":\"audio\",\"key\":\"codec\",\"value\":\"mp3\"}");
	ASSERT_STR_EQ(RCD_E_CHOICE, code);
	ASSERT_SET_REFUSED("{\"section\":\"image\",\"key\":\"brightness\"}");
	PASS();
}

/* What reaches the file is the table's spelling or a number rcd formatted --
 * never the caller's bytes. */
TEST renders_every_value_as_the_file_spells_it(void)
{
	rcd_edit_t e[RCD_EDITS_MAX];
	int n = 0;

	ASSERT_SET_OK("{\"section\":\"jpeg\",\"key\":\"enabled\",\"value\":true}", e, &n);
	ASSERT_STR_EQ("true", e[0].rendered);

	/* The 0/1 a templating client renders instead of a JSON boolean. */
	ASSERT_SET_OK("{\"section\":\"jpeg\",\"key\":\"enabled\",\"value\":0}", e, &n);
	ASSERT_STR_EQ("false", e[0].rendered);

	/* A numeric enum still matches when rendered as a number. */
	ASSERT_SET_OK("{\"section\":\"audio\",\"key\":\"sample_rate\",\"value\":16000}", e, &n);
	ASSERT_STR_EQ("16000", e[0].rendered);
	ASSERT_SET_OK("{\"section\":\"audio\",\"key\":\"sample_rate\",\"value\":\"16000\"}", e, &n);
	ASSERT_STR_EQ("16000", e[0].rendered);

	/* Orientation is 0/1 and not a boolean, because rvd reads it with
	 * rss_config_get_int: `true` parses as no number and falls back to the
	 * default, so a flip written that way is silently not applied. */
	ASSERT_SET_OK("{\"section\":\"image\",\"key\":\"hflip\",\"value\":1}", e, &n);
	ASSERT_STR_EQ("1", e[0].rendered);
	ASSERT_SET_REFUSED("{\"section\":\"image\",\"key\":\"hflip\",\"value\":true}");
	PASS();
}

/* The section and key written are the table's, not the payload's, so a line
 * reaching raptor.conf is spelled the way this build spells it. */
TEST names_edits_from_the_table_not_the_payload(void)
{
	rcd_edit_t e[RCD_EDITS_MAX];
	int n = 0;

	ASSERT_SET_OK("{\"section\":\"jpeg\",\"key\":\"quality\",\"value\":50}", e, &n);
	ASSERT_EQ(rcd_key_find("jpeg", "quality"), e[0].k);
	PASS();
}

/* ------------------------------------------------------------------ */
/* Credentials                                                         */
/* ------------------------------------------------------------------ */

TEST accepts_credentials_within_their_grammar(void)
{
	rcd_edit_t e[RCD_EDITS_MAX];
	int n = 0;

	ASSERT_SET_OK("{\"section\":\"rtsp\",\"key\":\"username\",\"value\":\"admin\"}", e, &n);
	ASSERT_STR_EQ("admin", e[0].rendered);

	ASSERT_SET_OK("{\"section\":\"http\",\"key\":\"password\",\"value\":\"a-b_c.d~9\"}", e, &n);
	ASSERT_STR_EQ("a-b_c.d~9", e[0].rendered);

	/* Empty is how authentication is turned off, and is the only way, so
	 * it has to be accepted. */
	ASSERT_SET_OK("{\"section\":\"rtsp\",\"key\":\"password\",\"value\":\"\"}", e, &n);
	ASSERT_STR_EQ("", e[0].rendered);
	PASS();
}

/*
 * The grammar is RFC 3986's unreserved set, which an RTSP URL, a Digest
 * header, an INI value and a shell word all accept unescaped -- so a
 * credential cannot become a second config directive, a path, or a URL that
 * parses as something else.
 */
TEST refuses_credentials_that_could_mean_something_else(void)
{
	ASSERT_SET_REFUSED("{\"section\":\"rtsp\",\"key\":\"password\",\"value\":\"a b\"}");
	ASSERT_SET_REFUSED("{\"section\":\"rtsp\",\"key\":\"password\",\"value\":\"a\\nb\"}");
	ASSERT_SET_REFUSED("{\"section\":\"rtsp\",\"key\":\"password\",\"value\":\"a/b\"}");
	ASSERT_SET_REFUSED("{\"section\":\"rtsp\",\"key\":\"password\",\"value\":\"a=b\"}");
	ASSERT_SET_REFUSED("{\"section\":\"rtsp\",\"key\":\"password\",\"value\":\"a;b\"}");
	ASSERT_SET_REFUSED("{\"section\":\"rtsp\",\"key\":\"password\",\"value\":\"a$b\"}");
	ASSERT_SET_REFUSED("{\"section\":\"rtsp\",\"key\":\"password\",\"value\":\"a@b\"}");
	ASSERT_SET_REFUSED("{\"section\":\"rtsp\",\"key\":\"password\",\"value\":\"a:b\"}");
	ASSERT_SET_REFUSED("{\"section\":\"rtsp\",\"key\":\"password\",\"value\":\"a#b\"}");
	ASSERT_SET_REFUSED(
		"{\"section\":\"rtsp\",\"key\":\"password\",\"value\":"
		"\"0123456789012345678901234567890123456789012345678901234567890123456789\"}");
	ASSERT_SET_REFUSED("{\"section\":\"rtsp\",\"key\":\"password\",\"value\":123}");
	PASS();
}

/* A rejected credential must not come back over the wire, not even one
 * character of it: the refusal names the permitted set instead. */
TEST never_quotes_a_rejected_credential_back(void)
{
	rcd_edit_t e[RCD_EDITS_MAX];
	int n;

	ASSERT_EQ(-1, validate_set("{\"section\":\"rtsp\",\"key\":\"password\","
				   "\"value\":\"sup3r$ecret\"}",
				   e, &n));
	ASSERT_EQ(NULL, strstr(reason, "sup3r"));
	ASSERT_EQ(NULL, strstr(reason, "ecret"));
	ASSERT(strstr(reason, "letters") != NULL);
	PASS();
}

/* ------------------------------------------------------------------ */
/* Tiers and ownership                                                 */
/* ------------------------------------------------------------------ */

/*
 * Which tier a key is on belongs to the table, and a caller cannot move it.
 * [image] is live because tuning is done by looking at the picture; geometry
 * is not, because an encoder is created at its size and cannot be resized.
 */
TEST the_table_decides_the_tier(void)
{
	ASSERT(rcd_key_find("image", "brightness")->live_cmd != NULL);
	ASSERT(rcd_key_find("audio", "volume")->live_cmd != NULL);
	ASSERT(rcd_key_find("stream0", "bitrate")->live_cmd != NULL);

	ASSERT_EQ(NULL, rcd_key_find("stream0", "width")->live_cmd);
	ASSERT_EQ(NULL, rcd_key_find("stream0", "codec")->live_cmd);
	ASSERT_EQ(NULL, rcd_key_find("sensor", "fps")->live_cmd);
	ASSERT_EQ(NULL, rcd_key_find("rtsp", "port")->live_cmd);
	PASS();
}

/* A per-channel live command needs the channel, and it comes from the key's
 * own section rather than from anything the caller sent. */
TEST channelled_keys_carry_their_own_channel(void)
{
	ASSERT_EQ(0, rcd_key_find("stream0", "bitrate")->live_chn);
	ASSERT_EQ(1, rcd_key_find("stream1", "bitrate")->live_chn);
	ASSERT_EQ(0, rcd_key_find("stream0", "gop")->live_chn);
	ASSERT_EQ(1, rcd_key_find("stream1", "fps")->live_chn);
	ASSERT_EQ(-1, rcd_key_find("image", "brightness")->live_chn);
	PASS();
}

/*
 * Every writable key has an owner that can be restarted, or an edit would be
 * saved with nothing able to pick it up. Walked over the whole table so a key
 * added to a new section cannot quietly land in that state.
 */
TEST every_writable_key_has_an_owner(void)
{
	for (int i = 0;; i++) {
		const rcd_key_t *k = rcd_key_at(i);
		if (!k)
			break;
		/* A key belongs to a daemon that re-reads it, or to a provider
		 * that stores it. A key with neither is one nothing enacts. */
		if (k->provider)
			continue;
		ASSERT_EQm(k->section, 1, rcd_section_owner(k->section) != RCD_D_COUNT);
	}
	PASS();
}

/* Restarting rvd stops capture; restarting rsd drops viewers; restarting rod
 * interrupts a feature nobody holds a socket to. A client is told which before
 * it commits, so these must not collapse into one another. */
TEST impact_separates_the_pipeline_from_the_stream(void)
{
	ASSERT_EQ(RCD_IMPACT_PIPELINE, rcd_daemon_impact(RCD_D_RVD));
	ASSERT_EQ(RCD_IMPACT_STREAM, rcd_daemon_impact(RCD_D_RSD));
	ASSERT_EQ(RCD_IMPACT_STREAM, rcd_daemon_impact(RCD_D_RHD));
	ASSERT_EQ(RCD_IMPACT_STREAM, rcd_daemon_impact(RCD_D_RWD));
	ASSERT_EQ(RCD_IMPACT_SERVICE, rcd_daemon_impact(RCD_D_ROD));
	ASSERT_EQ(RCD_IMPACT_SERVICE, rcd_daemon_impact(RCD_D_RAD));
	ASSERT_STR_EQ("pipeline", rcd_impact_name(RCD_IMPACT_PIPELINE));
	ASSERT_STR_EQ("none", rcd_impact_name(RCD_IMPACT_NONE));
	ASSERT_STR_EQ("reboot", rcd_impact_name(RCD_IMPACT_REBOOT));
	PASS();
}

/* ------------------------------------------------------------------ */
/* [system]: keys whose store is a file rather than raptor.conf        */
/* ------------------------------------------------------------------ */

/*
 * The providers write real files. RCD_SYSCONF_DIR points them at a scratch
 * directory under /run for the test build -- see tests/Makefile -- so what is
 * exercised here is the writer itself, not a stand-in for it.
 */
static int sysconf_dir_ready(void)
{
	if (mkdir(RCD_SYSCONF_DIR, 0755) == 0)
		return 1;
	return access(RCD_SYSCONF_DIR, W_OK) == 0;
}

TEST the_zone_table_is_two_arrays_of_one_length(void)
{
	int n = 0;
	while (rcd_zone_names[n])
		n++;

	/* Not a tautology: the names are the schema's choices and the rules
	 * are indexed by the same subscript, so a generator that dropped a
	 * line from one array would otherwise hand out the wrong rule for
	 * every zone after it. */
	ASSERT(n > 100);
	for (int i = 0; i < n; i++) {
		const char *posix = rcd_zone_posix(rcd_zone_names[i]);
		ASSERT_EQm(rcd_zone_names[i], 1, posix != NULL);
		ASSERT(posix[0] != '\0');
	}
	ASSERT_EQ(NULL, rcd_zone_posix("Mars/Olympus"));
	ASSERT_EQ(NULL, rcd_zone_posix(""));
	PASS();
}

TEST the_timezone_is_an_enum_over_the_zone_table(void)
{
	rcd_edit_t e[RCD_EDITS_MAX];
	int n = 0;

	ASSERT_EQ(0, validate_set("{\"section\":\"device\",\"key\":\"timezone\","
				  "\"value\":\"America/Los_Angeles\"}",
				  e, &n));
	ASSERT_STR_EQ("America/Los_Angeles", e[0].rendered);

	/* A zone this build does not carry is refused by name rather than
	 * written and discovered at the next boot. */
	ASSERT_EQ(-1, validate_set("{\"section\":\"device\",\"key\":\"timezone\","
				   "\"value\":\"Mars/Olympus\"}",
				   e, &n));
	ASSERT_STR_EQ(RCD_E_CHOICE, code);
	PASS();
}

TEST a_host_is_a_hostname_or_an_address_and_nothing_else(void)
{
	rcd_edit_t e[RCD_EDITS_MAX];
	int n = 0;

	static const char *const good[] = {"pool.ntp.org", "192.168.1.1", "ntp", "a-b.example.com",
					   NULL};
	for (int i = 0; good[i]; i++) {
		char req[192];
		snprintf(req, sizeof(req),
			 "{\"section\":\"device\",\"key\":\"ntp_server\",\"value\":\"%s\"}",
			 good[i]);
		ASSERT_EQm(good[i], 0, validate_set(req, e, &n));
		ASSERT_STR_EQ(good[i], e[0].rendered);
	}

	/* Each of these could become something other than a hostname on the
	 * line it is written to: a path, a second directive, a shell word, an
	 * option. None of them reaches the file. */
	static const char *const bad[] = {
		"/etc/passwd", "host name",	     "host;reboot", "-host", "host-", ".host",
		"host.",       "host\\nserver evil", "$(reboot)",   "",	     NULL};
	for (int i = 0; bad[i]; i++) {
		char req[192];
		snprintf(req, sizeof(req),
			 "{\"section\":\"device\",\"key\":\"ntp_server\",\"value\":\"%s\"}",
			 bad[i]);
		ASSERT_EQm(bad[i], -1, validate_set(req, e, &n));
	}

	/* And it is a string: a number is not a lenient spelling of one. */
	ASSERT_EQ(-1, validate_set("{\"section\":\"device\",\"key\":\"ntp_server\","
				   "\"value\":8}",
				   e, &n));
	ASSERT_STR_EQ(RCD_E_TYPE, code);
	PASS();
}

TEST a_provider_key_round_trips_through_its_store(void)
{
	if (!sysconf_dir_ready())
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	ASSERT_EQ(0, rcd_provider_timezone.set("Asia/Tokyo"));

	char out[RCD_VAL_MAX] = "";
	ASSERT_EQ(0, rcd_provider_timezone.get(out, sizeof(out)));
	ASSERT_STR_EQ("Asia/Tokyo", out);

	ASSERT_EQ(0, rcd_provider_ntp_server.set("pool.ntp.org"));
	out[0] = '\0';
	ASSERT_EQ(0, rcd_provider_ntp_server.get(out, sizeof(out)));
	ASSERT_STR_EQ("pool.ntp.org", out);

	/* The rule reaches the file the C library reads, not just the name. */
	FILE *f = fopen(RCD_SYSCONF_DIR "/TZ", "r");
	ASSERT(f != NULL);
	char rule[64] = "";
	ASSERT(fgets(rule, sizeof(rule), f) != NULL);
	fclose(f);
	rule[strcspn(rule, "\r\n")] = '\0';
	ASSERT_STR_EQ(rcd_zone_posix("Asia/Tokyo"), rule);
	PASS();
}

/* ------------------------------------------------------------------ */
/* Confirm-or-revert                                                    */
/* ------------------------------------------------------------------ */

/*
 * The guard is exercised through the hostname provider rather than a mock of
 * one, because the property under test is that the *store* goes back. A mock
 * would prove that a string was copied around.
 *
 * The camera keeps the snapshot on flash and the deadline on a tmpfs, which is
 * what makes a reboot revert; here both land in the scratch directory, so the
 * reboot test unlinks the deadline by hand. What is being tested is the rule
 * rcd applies when it finds one without the other.
 */
static int guard_ready(rcd_state_t *st, const char *name)
{
	if (!sysconf_dir_ready())
		return 0;
	memset(st, 0, sizeof(*st));
	unlink(RCD_GUARD_ARMED_PATH);
	unlink(RCD_SYSCONF_DIR "/" RCD_GUARD_RECORD_NAME);
	return rcd_provider_hostname.set(name) == 0;
}

/*
 * What a client actually does: `set` stores the value over a held snapshot,
 * and `apply` starts the clock. Written out here rather than hidden in a
 * helper because the order is the property -- a snapshot taken after the
 * store has been written holds the new value and reverts to nothing.
 */
static int guard_change(rcd_state_t *st, const char *name, int window)
{
	rcd_guard_hold(st);
	if (rcd_provider_hostname.set(name) != 0)
		return -1;
	rcd_guard_arm(st, window);
	return 0;
}

static const char *hostname_now(char *buf, size_t sz)
{
	buf[0] = '\0';
	rcd_provider_hostname.get(buf, sz);
	return buf;
}

/* Time is passed in rather than waited for: the window is 90 seconds and the
 * suite may not take 90 seconds to find out what happens at the end of it. */
#define GUARD_LATER ((uint64_t)-1)

TEST an_unconfirmed_change_goes_back_when_the_window_ends(void)
{
	rcd_state_t st;
	char now[RCD_VAL_MAX];

	if (!guard_ready(&st, "camera-before"))
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	ASSERT_EQ(0, guard_change(&st, "camera-after", 90));
	ASSERT(rcd_guard_remaining(&st) > 0);
	ASSERT(rcd_guard_remaining(&st) <= 90);
	ASSERT_STR_EQ("camera-after", hostname_now(now, sizeof(now)));

	/* Not yet: an armed guard that reverted early would be a countdown
	 * nobody could ever beat. */
	rcd_guard_tick(&st, 0);
	ASSERT_STR_EQ("camera-after", hostname_now(now, sizeof(now)));

	rcd_guard_tick(&st, GUARD_LATER);
	ASSERT_STR_EQ("camera-before", hostname_now(now, sizeof(now)));
	ASSERT_EQ(0, rcd_guard_remaining(&st));
	PASS();
}

TEST a_confirmed_change_stays_and_leaves_nothing_armed(void)
{
	rcd_state_t st;
	char now[RCD_VAL_MAX];

	if (!guard_ready(&st, "camera-before"))
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	ASSERT_EQ(0, guard_change(&st, "camera-after", 90));

	cJSON *r = rcd_cmd_confirm(&st, NULL);
	ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "confirmed")));
	cJSON_Delete(r);

	rcd_guard_tick(&st, GUARD_LATER);
	ASSERT_STR_EQ("camera-after", hostname_now(now, sizeof(now)));

	/* Both files are gone, so a later rcd start finds nothing to undo. */
	ASSERT(access(RCD_SYSCONF_DIR "/" RCD_GUARD_RECORD_NAME, F_OK) != 0);
	ASSERT(access(RCD_GUARD_ARMED_PATH, F_OK) != 0);

	/* And confirming again is not an error: a client that reconnected
	 * cannot know whether it made it inside the window. */
	r = rcd_cmd_confirm(&st, NULL);
	ASSERT(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(r, "confirmed")));
	cJSON_Delete(r);
	PASS();
}

TEST cancelling_puts_it_back_without_waiting(void)
{
	rcd_state_t st;
	char now[RCD_VAL_MAX];

	if (!guard_ready(&st, "camera-before"))
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	ASSERT_EQ(0, guard_change(&st, "camera-after", 90));

	cJSON *r = rcd_cmd_cancel(&st, NULL);
	ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "reverted")));
	cJSON_Delete(r);

	ASSERT_STR_EQ("camera-before", hostname_now(now, sizeof(now)));
	ASSERT_EQ(0, rcd_guard_remaining(&st));
	PASS();
}

/*
 * The reason the deadline is on a tmpfs and the snapshot is not. A camera
 * power-cycled by somebody who has lost it must come back as it was, and the
 * absence of a deadline it cannot have kept is the only evidence rcd has.
 */
TEST a_reboot_inside_the_window_reverts(void)
{
	rcd_state_t st;
	char now[RCD_VAL_MAX];

	if (!guard_ready(&st, "camera-before"))
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	ASSERT_EQ(0, guard_change(&st, "camera-after", 90));

	/* What a boot looks like from here: the tmpfs is empty and rcd starts
	 * with no memory of anything. */
	unlink(RCD_GUARD_ARMED_PATH);

	rcd_state_t fresh;
	memset(&fresh, 0, sizeof(fresh));
	rcd_guard_load(&fresh);

	ASSERT_STR_EQ("camera-before", hostname_now(now, sizeof(now)));
	ASSERT_EQ(0, rcd_guard_remaining(&fresh));
	PASS();
}

/* An rcd that merely restarted is not a camera that rebooted: the deadline is
 * still there, so the window carries on rather than reverting under a client
 * that is still watching the clock. */
TEST an_rcd_restart_inside_the_window_keeps_it_armed(void)
{
	rcd_state_t st;
	char now[RCD_VAL_MAX];

	if (!guard_ready(&st, "camera-before"))
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	ASSERT_EQ(0, guard_change(&st, "camera-after", 90));

	rcd_state_t fresh;
	memset(&fresh, 0, sizeof(fresh));
	rcd_guard_load(&fresh);

	ASSERT_STR_EQ("camera-after", hostname_now(now, sizeof(now)));
	ASSERT(rcd_guard_remaining(&fresh) > 0);

	/* And the snapshot survived the restart, so the revert still works. */
	rcd_guard_tick(&fresh, GUARD_LATER);
	ASSERT_STR_EQ("camera-before", hostname_now(now, sizeof(now)));
	PASS();
}

/*
 * Two edits into the dark are one experiment. Re-snapshotting on the second
 * would quietly make the first one permanent -- and the first one is the one
 * that may already have cost the client its route back.
 */
TEST a_second_change_inside_the_window_keeps_the_first_snapshot(void)
{
	rcd_state_t st;
	char now[RCD_VAL_MAX];

	if (!guard_ready(&st, "camera-before"))
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	rcd_guard_arm(&st, 90);
	ASSERT_EQ(0, rcd_provider_hostname.set("camera-middle"));

	ASSERT_EQ(0, guard_change(&st, "camera-after", 90));

	rcd_guard_tick(&st, GUARD_LATER);
	ASSERT_STR_EQ("camera-before", hostname_now(now, sizeof(now)));
	PASS();
}

/*
 * A revert that cannot finish must not forget what it was reverting to.
 *
 * The stores are on an overlay over NOR, and the reasons a write to one fails
 * are mostly temporary: a filesystem that is momentarily full, an overlay that
 * has not finished coming back. Clearing the snapshot on the first of those
 * left the camera holding a setting nobody confirmed with no record of what it
 * had before -- which is the one failure the guard exists to prevent, arriving
 * by way of the guard itself.
 */
#define HOSTNAME_TMP RCD_SYSCONF_DIR "/hostname.tmp"
#define GUARD_RECORD RCD_SYSCONF_DIR "/" RCD_GUARD_RECORD_NAME

/*
 * write_file() writes through <path>.tmp and renames, so a directory in that
 * name makes the write fail. Chmod would not: the suite runs in a user
 * namespace where it is root, and root is not stopped by a mode bit.
 */
static int block_hostname_writes(void)
{
	/* Defensively, in the same spirit as guard_ready(): a test that fails
	 * partway through leaves this behind, and the next one skipping
	 * because of it would hide the second failure behind the first. */
	rmdir(HOSTNAME_TMP);
	return mkdir(HOSTNAME_TMP, 0755) == 0;
}

static void unblock_hostname_writes(void)
{
	rmdir(HOSTNAME_TMP);
}

TEST a_revert_that_cannot_write_keeps_the_record_and_tries_again(void)
{
	rcd_state_t st;
	char now[RCD_VAL_MAX];

	if (!guard_ready(&st, "camera-before"))
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	ASSERT_EQ(0, guard_change(&st, "camera-after", 90));

	if (!block_hostname_writes())
		SKIPm("cannot make " HOSTNAME_TMP " refuse a write");

	rcd_guard_tick(&st, GUARD_LATER);

	/*
	 * Everything the blocked pass has to say, read out before the block is
	 * lifted. An assertion firing while the store is still unwritable
	 * would leave it that way for every test after this one, and the
	 * cascade of skips would bury the failure that caused it.
	 */
	int record_kept = access(GUARD_RECORD, F_OK) == 0;
	int rearmed = rcd_guard_remaining(&st) > 0;
	hostname_now(now, sizeof(now));
	unblock_hostname_writes();

	/* It did not go back. */
	ASSERT_STR_EQ("camera-after", now);

	/* So the snapshot is still on flash and the clock is running again.
	 * Both of those were cleared unconditionally before this fix. */
	ASSERTm("the record was cleared by a revert that did not happen", record_kept);
	ASSERTm("nothing is going to try again", rearmed);

	rcd_guard_tick(&st, GUARD_LATER);

	ASSERT_STR_EQ("camera-before", hostname_now(now, sizeof(now)));
	ASSERT(access(GUARD_RECORD, F_OK) != 0);
	PASS();
}

/*
 * And when it never lands, what stops is the retrying and not the guard.
 *
 * A record with no deadline beside it is what rcd reads as "armed, and the
 * camera has rebooted since" -- so leaving the pair in that shape is what
 * keeps power-cycling a recovery for somebody who has lost the camera
 * entirely. It is the last thing that still works, and it costs one unlink to
 * keep.
 */
TEST a_revert_that_never_lands_leaves_the_record_for_a_power_cycle(void)
{
	rcd_state_t st;
	char now[RCD_VAL_MAX];

	if (!guard_ready(&st, "camera-before"))
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	ASSERT_EQ(0, guard_change(&st, "camera-after", 90));

	if (!block_hostname_writes())
		SKIPm("cannot make " HOSTNAME_TMP " refuse a write");

	/* Every attempt the guard allows itself. The cap is the guard's own
	 * business; what is asserted is that there is one, because a revert
	 * that can never succeed should not log until the camera is
	 * rebooted. */
	for (int i = 0; i < 16 && rcd_guard_remaining(&st) > 0; i++)
		rcd_guard_tick(&st, GUARD_LATER);

	/* Read out before the block is lifted, for the reason above. */
	int still_armed = rcd_guard_remaining(&st) > 0;
	int record_kept = access(GUARD_RECORD, F_OK) == 0;
	int marker_gone = access(RCD_GUARD_ARMED_PATH, F_OK) != 0;
	hostname_now(now, sizeof(now));
	unblock_hostname_writes();

	ASSERTm("it is still retrying after every attempt it allows itself", !still_armed);
	ASSERT_STR_EQ("camera-after", now);

	ASSERTm("nothing is left to put the camera back", record_kept);
	ASSERTm("the deadline outlived the retrying", marker_gone);

	/* Which is exactly the state a reboot is read from. */

	rcd_state_t fresh;
	memset(&fresh, 0, sizeof(fresh));
	rcd_guard_load(&fresh);

	ASSERT_STR_EQ("camera-before", hostname_now(now, sizeof(now)));
	PASS();
}

/*
 * An enact that failed leaves its key owed.
 *
 * The store holds the new address and the interface is still running the old
 * one, which is precisely the drift `pending` exists to show. Forgetting it
 * because an enact was attempted made the report say nothing was owed and the
 * next `apply` a no-op -- so the camera stayed on the old address with
 * everything claiming it had taken the new one.
 */
/*
 * And `cancel` says which of those happened.
 *
 * "Reverted, nothing armed" is what a client goes away believing, so it has to
 * be true. A cancel whose write failed leaves a retry behind it and the camera
 * still on the settings the operator just asked to be rid of -- reporting that
 * as done is the same class of lie as the record that was cleared.
 */
TEST a_cancel_that_could_not_write_says_so(void)
{
	rcd_state_t st;

	if (!guard_ready(&st, "camera-before"))
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	ASSERT_EQ(0, guard_change(&st, "camera-after", 90));

	if (!block_hostname_writes())
		SKIPm("cannot make " HOSTNAME_TMP " refuse a write");

	cJSON *r = rcd_cmd_cancel(&st, NULL);
	int said_reverted = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "reverted"));
	int said_armed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "armed"));
	cJSON_Delete(r);
	unblock_hostname_writes();

	ASSERTm("it reported a revert that did not happen", !said_reverted);
	ASSERTm("it reported nothing armed while a retry is pending", said_armed);

	/* And the retry still puts it back. */
	rcd_guard_tick(&st, GUARD_LATER);

	char now[RCD_VAL_MAX];
	ASSERT_STR_EQ("camera-before", hostname_now(now, sizeof(now)));
	PASS();
}

TEST an_enact_that_did_not_take_stays_owed(void)
{
	rcd_state_t st;
	memset(&st, 0, sizeof(st));

	rcd_stale_add(&st, "network", "address", RCD_D_COUNT);
	ASSERT_EQ(1, st.stale_count);

	/* What apply hands back when the interface would not come up. */
	rcd_enact_done(&st, NULL, 0);
	ASSERT_EQ(1, st.stale_count);

	/* And what it hands back when it did. One enact settles every key of
	 * the stanza it brought up, because they share the call. */
	rcd_stale_add(&st, "network", "netmask", RCD_D_COUNT);
	ASSERT_EQ(2, st.stale_count);

	const rcd_provider_t *ok[1] = {&rcd_provider_net_address};
	rcd_enact_done(&st, ok, 1);
	ASSERT_EQ(0, st.stale_count);
	PASS();
}

/*
 * `state` writes out the drift it decides is over.
 *
 * A daemon that is not running is not running behind: it reads the file on its
 * own way up. Dropping that from memory and leaving the /run record saying
 * otherwise meant the next rcd start read the drift back and offered to enact
 * it -- against a daemon that was never behind. `state` is polled constantly,
 * so the file would have been wrong far more often than right.
 */
TEST state_writes_out_the_drift_it_clears(void)
{
	rcd_state_t st;
	memset(&st, 0, sizeof(st));

	rcd_stale_add(&st, "image", "brightness", RCD_D_RVD);
	rcd_stale_save(&st);

	rcd_state_t reloaded;
	memset(&reloaded, 0, sizeof(reloaded));
	rcd_stale_load(&reloaded);
	if (reloaded.stale_count != 1)
		SKIPm("no writable /run/rss -- run the suite under unshare -rm");

	/* No daemon is running under the suite, so the poll finds rvd down. */
	cJSON *r = rcd_cmd_state(&st, NULL);
	ASSERT(r != NULL);
	cJSON_Delete(r);
	ASSERT_EQ(0, st.stale_count);

	memset(&reloaded, 0, sizeof(reloaded));
	rcd_stale_load(&reloaded);
	ASSERT_EQ(0, reloaded.stale_count);
	PASS();
}

/*
 * A staged change is not in force, so cancelling it is a file write and not an
 * outage -- and afterwards there must be nothing left for an apply to do. A
 * key still listed as drift here is a pending change the operator is invited
 * to enact, which would put back the value they just took back.
 */
TEST cancelling_a_staged_change_leaves_nothing_owed(void)
{
	rcd_state_t st;
	char now[RCD_VAL_MAX];

	if (!guard_ready(&st, "camera-before"))
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	/* What `set` does for a provider that can enact. */
	rcd_guard_hold(&st);
	ASSERT_EQ(0, rcd_provider_hostname.set("camera-staged"));
	rcd_stale_add(&st, "device", "hostname", RCD_D_COUNT);
	ASSERT_EQ(1, st.stale_count);

	cJSON *r = rcd_cmd_cancel(&st, NULL);
	ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "reverted")));
	cJSON_Delete(r);

	ASSERT_STR_EQ("camera-before", hostname_now(now, sizeof(now)));
	ASSERT_EQ(0, st.stale_count);
	PASS();
}

/* What a client polls while it counts down. Absent when nothing is armed, so
 * a page that has never seen a guard has nothing to draw. */
TEST the_guard_reports_itself_only_while_it_is_armed(void)
{
	rcd_state_t st;

	if (!guard_ready(&st, "camera-before"))
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	cJSON *out = cJSON_CreateObject();
	rcd_guard_report(&st, out);
	ASSERT_EQ(NULL, cJSON_GetObjectItemCaseSensitive(out, "guard"));
	cJSON_Delete(out);

	rcd_guard_arm(&st, 90);

	out = cJSON_CreateObject();
	rcd_guard_report(&st, out);
	const cJSON *g = cJSON_GetObjectItemCaseSensitive(out, "guard");
	ASSERT(cJSON_IsObject(g));
	ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "armed")));
	ASSERT(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(g, "revert_in_sec")) > 0);
	/*
	 * Named, so the page can say which settings it is asking about -- and
	 * every guarded key is there, not only the one that changed: they
	 * share files, so they are put back together or not at all.
	 */
	const cJSON *keys = cJSON_GetObjectItemCaseSensitive(g, "keys");
	ASSERT(cJSON_IsArray(keys));
	int guarded = 0;
	for (int i = 0; rcd_key_at(i); i++)
		guarded += rcd_key_at(i)->guard_sec > 0 ? 1 : 0;
	ASSERT(guarded > 1);
	ASSERT_EQ(guarded, cJSON_GetArraySize(keys));
	cJSON_Delete(out);

	rcd_guard_confirm(&st);
	PASS();
}

/* ------------------------------------------------------------------ */
/* The camera's address                                                 */
/* ------------------------------------------------------------------ */

/* The shipped stanza, and the reason this rewrites lines rather than files:
 * the second one is where the interface's MAC comes from. */
static const char *const SHIPPED_ETH0 =
	"iface eth0 inet dhcp\n"
	"    hwaddress ether $(fw_printenv -n ethaddr || echo 00:00:23:34:45:66)\n";

static void iface_path(char *out, size_t outsz)
{
	snprintf(out, outsz, "%s/network/interfaces.d/%s", RCD_SYSCONF_DIR, rcd_net_iface());
}

static int iface_ready(void)
{
	char dir[256], path[320];

	if (!sysconf_dir_ready())
		return 0;

	snprintf(dir, sizeof(dir), "%s/network", RCD_SYSCONF_DIR);
	mkdir(dir, 0755);
	snprintf(dir, sizeof(dir), "%s/network/interfaces.d", RCD_SYSCONF_DIR);
	mkdir(dir, 0755);

	iface_path(path, sizeof(path));
	FILE *f = fopen(path, "w");
	if (!f)
		return 0;
	fputs(SHIPPED_ETH0, f);
	fclose(f);
	return 1;
}

static void iface_read(char *out, size_t outsz)
{
	char path[320];
	iface_path(path, sizeof(path));

	out[0] = '\0';
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	size_t n = fread(out, 1, outsz - 1, f);
	out[n] = '\0';
	fclose(f);
}

TEST an_address_is_four_octets_and_nothing_else(void)
{
	rcd_edit_t e[RCD_EDITS_MAX];
	int n = 0;

	static const char *const good[] = {"192.168.1.50", "0.0.0.0", "255.255.255.255", "10.0.0.1",
					   NULL};
	for (int i = 0; good[i]; i++) {
		char req[192];
		snprintf(req, sizeof(req),
			 "{\"section\":\"network\",\"key\":\"address\",\"value\":\"%s\"}", good[i]);
		ASSERT_EQm(good[i], 0, validate_set(req, e, &n));
		ASSERT_STR_EQ(good[i], e[0].rendered);
	}

	/* 256 is not an octet; a leading zero is octal to most things that
	 * parse this file and decimal to everyone who types it; and the rest
	 * are not addresses at all. */
	static const char *const bad[] = {
		"192.168.1.256", "192.168.1",	 "192.168.1.1.1",  "192.168.01.1", "192.168.1.",
		".1.2.3",	 "192.168.1.1 ", "1.2.3.4;reboot", "localhost",	   NULL};
	for (int i = 0; bad[i]; i++) {
		char req[192];
		snprintf(req, sizeof(req),
			 "{\"section\":\"network\",\"key\":\"address\",\"value\":\"%s\"}", bad[i]);
		ASSERT_EQm(bad[i], -1, validate_set(req, e, &n));
	}

	/* A camera on a flat network has no gateway and no name server, and
	 * a form that submits every field has to be able to say so. The
	 * address itself is not optional in the same way: an interface
	 * configured static without one does not come up. */
	ASSERT_EQ(0, validate_set("{\"section\":\"network\",\"key\":\"gateway\",\"value\":\"\"}", e,
				  &n));
	ASSERT_STR_EQ("", e[0].rendered);
	ASSERT_EQ(-1, validate_set("{\"section\":\"network\",\"key\":\"address\",\"value\":\"\"}",
				   e, &n));
	PASS();
}

/*
 * The line rcd must not lose. `hwaddress ether $(fw_printenv -n ethaddr ...)`
 * is where this camera's MAC comes from, evaluated by the shell at ifup --
 * so a provider that rewrote the stanza as "iface + address + netmask" would
 * bring the interface back on a different MAC and a different lease.
 */
TEST the_interface_stanza_keeps_what_it_did_not_write(void)
{
	char file[1024];

	if (!iface_ready())
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	ASSERT_EQ(0, rcd_provider_net_dhcp.set("false"));
	ASSERT_EQ(0, rcd_provider_net_address.set("192.168.1.50"));
	ASSERT_EQ(0, rcd_provider_net_netmask.set("255.255.255.0"));
	ASSERT_EQ(0, rcd_provider_net_gateway.set("192.168.1.1"));
	ASSERT_EQ(0, rcd_provider_net_dns.set("192.168.1.1"));

	iface_read(file, sizeof(file));
	ASSERT(strstr(file, "hwaddress ether $(fw_printenv -n ethaddr") != NULL);
	ASSERT(strstr(file, "iface eth0 inet static") != NULL);
	ASSERT(strstr(file, "address 192.168.1.50") != NULL);
	ASSERT(strstr(file, "netmask 255.255.255.0") != NULL);

	/* Read back through the same providers, so the pair is what is
	 * tested rather than the spelling of one file. */
	char v[RCD_VAL_MAX];
	ASSERT_EQ(0, rcd_provider_net_address.get(v, sizeof(v)));
	ASSERT_STR_EQ("192.168.1.50", v);
	ASSERT_EQ(0, rcd_provider_net_dhcp.get(v, sizeof(v)));
	ASSERT_STR_EQ("false", v);

	/* Setting one twice must not leave two of it: ifupdown refuses a
	 * duplicate option outright, so the interface would stop coming up. */
	ASSERT_EQ(0, rcd_provider_net_address.set("192.168.1.51"));
	iface_read(file, sizeof(file));
	/* Indented, so that `hwaddress ether ...` -- which ends in the same
	 * eight characters -- is not counted as one of them. */
	int copies = 0;
	for (const char *at = file; (at = strstr(at, "\n    address ")) != NULL; at++)
		copies++;
	ASSERT_EQ(1, copies);

	/* And back to DHCP, which changes one word and keeps the static
	 * settings for the next time somebody wants them. */
	ASSERT_EQ(0, rcd_provider_net_dhcp.set("true"));
	iface_read(file, sizeof(file));
	ASSERT(strstr(file, "iface eth0 inet dhcp") != NULL);
	ASSERT(strstr(file, "address 192.168.1.51") != NULL);
	ASSERT(strstr(file, "hwaddress ether $(fw_printenv") != NULL);
	PASS();
}

/* An empty value removes the directive rather than writing a bare one, which
 * ifupdown reads as a parse error and refuses the whole interface for. */
TEST clearing_an_address_removes_its_line(void)
{
	char file[1024];

	if (!iface_ready())
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	ASSERT_EQ(0, rcd_provider_net_gateway.set("192.168.1.1"));
	iface_read(file, sizeof(file));
	ASSERT(strstr(file, "gateway 192.168.1.1") != NULL);

	ASSERT_EQ(0, rcd_provider_net_gateway.set(""));
	iface_read(file, sizeof(file));
	ASSERT_EQ(NULL, strstr(file, "gateway"));
	ASSERT(strstr(file, "hwaddress ether $(fw_printenv") != NULL);
	PASS();
}

/* Every one of them is on the same clock, and none of them is live: a change
 * of address is applied, never typed into effect. */
/*
 * A snapshot records the store, not what the store falls back to.
 *
 * `dns` answers from /etc/resolv.conf when the interface stanza is silent,
 * which is the right answer to an operator asking what the camera resolves
 * with. It is the wrong thing to write back: a revert would put this network's
 * DHCP resolver into the stanza as a setting, where it outlives the lease it
 * came from and follows the camera onto every network it is moved to
 * afterwards.
 */
TEST a_dns_snapshot_does_not_pin_what_dhcp_supplied(void)
{
	rcd_state_t st;
	memset(&st, 0, sizeof(st));

	if (!iface_ready())
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");
	unlink(GUARD_RECORD);

	/* A camera that has only ever used DHCP: nothing in the stanza -- the
	 * shipped one iface_ready() lays down has no dns-nameserver line --
	 * and a resolver the lease supplied. */
	ASSERT_EQ(0, rcd_provider_net_dns.set(""));

	FILE *f = fopen(RCD_SYSCONF_DIR "/resolv.conf", "w");
	if (!f)
		SKIPm("cannot write resolv.conf");
	fputs("nameserver 10.9.9.9\n", f);
	fclose(f);

	/* The fallback is live, and it is what a client is told. */
	char out[RCD_VAL_MAX] = "";
	ASSERT_EQ(0, rcd_provider_net_dns.get(out, sizeof(out)));
	ASSERT_STR_EQ("10.9.9.9", out);

	/* The snapshot is told the truth instead. */
	rcd_guard_hold(&st);

	int found = 0;
	for (int i = 0; i < st.guard_count; i++) {
		if (strcmp(st.guard[i].section, "network") != 0 ||
		    strcmp(st.guard[i].key, "dns") != 0)
			continue;
		found = 1;
		ASSERTm("the snapshot recorded resolv.conf as a setting", !st.guard[i].had);
	}
	ASSERT_EQm("the dns key was not in the snapshot at all", 1, found);

	unlink(GUARD_RECORD);
	PASS();
}
TEST every_network_key_is_guarded_and_waits_for_apply(void)
{
	int seen = 0;
	for (int i = 0; rcd_key_at(i); i++) {
		const rcd_key_t *k = rcd_key_at(i);
		if (strcmp(k->section, "network") != 0)
			continue;
		seen++;
		ASSERT_EQm(k->key, RCD_GUARD_NET_SEC, k->guard_sec);
		ASSERT_EQm(k->key, false, rcd_key_live(k));
		ASSERT_EQm(k->key, RCD_IMPACT_NETWORK, rcd_key_impact(k));
		/* And each of them can be put into force, which is what makes
		 * them the restart tier rather than a declaration about them. */
		ASSERT(k->provider != NULL && k->provider->enact != NULL);
	}
	ASSERT_EQ(5, seen);
	PASS();
}

/*
 * The snapshot covers every guarded key; the revert writes back only the ones
 * that moved. A store nobody touched must come out of a revert byte for byte
 * -- and `network.dns` is the case that makes it matter, since its `get` falls
 * back to /etc/resolv.conf when its own store is silent, so putting the
 * "previous value" back would write a setting that was never set.
 */
TEST a_revert_leaves_the_stores_it_did_not_change_alone(void)
{
	rcd_state_t st;
	char before[1024], after[1024];

	if (!guard_ready(&st, "camera-before") || !iface_ready())
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	iface_read(before, sizeof(before));

	/* One key changes; the other five guarded ones do not. */
	rcd_guard_hold(&st);
	ASSERT_EQ(0, rcd_provider_hostname.set("camera-staged"));

	cJSON *r = rcd_cmd_cancel(&st, NULL);
	cJSON_Delete(r);

	char now[RCD_VAL_MAX];
	ASSERT_STR_EQ("camera-before", hostname_now(now, sizeof(now)));

	iface_read(after, sizeof(after));
	ASSERT_STR_EQ(before, after);
	PASS();
}

/*
 * The ordinary case for a camera that has only ever used DHCP: the stanza has
 * no address at all. Reverting has to put it back to not having one, which is
 * a different thing from putting an empty one there -- so `set("")` means
 * unset, and a store that cannot be emptied refuses it.
 */
TEST a_revert_can_unset_a_value_that_was_never_there(void)
{
	rcd_state_t st;
	char before[1024], after[1024];

	if (!guard_ready(&st, "camera-before") || !iface_ready())
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	iface_read(before, sizeof(before));
	/* Indented, so `hwaddress ether ...` is not mistaken for one. */
	ASSERT_EQ(NULL, strstr(before, "\n    address "));

	rcd_guard_hold(&st);
	ASSERT_EQ(0, rcd_provider_net_address.set("192.168.1.99"));
	rcd_stale_add(&st, "network", "address", RCD_D_COUNT);
	iface_read(after, sizeof(after));
	ASSERT(strstr(after, "address 192.168.1.99") != NULL);

	cJSON *r = rcd_cmd_cancel(&st, NULL);
	cJSON_Delete(r);

	/* Byte for byte, and nothing left for an apply to enact. */
	iface_read(after, sizeof(after));
	ASSERT_STR_EQ(before, after);
	ASSERT_EQ(0, st.stale_count);

	/* Setting a key back to what it already held is settled too: the
	 * revert writes nothing and the drift goes with it. */
	rcd_guard_hold(&st);
	ASSERT_EQ(0, rcd_provider_hostname.set("camera-before"));
	rcd_stale_add(&st, "device", "hostname", RCD_D_COUNT);
	cJSON *same = rcd_cmd_cancel(&st, NULL);
	cJSON_Delete(same);
	ASSERT_EQ(0, st.stale_count);

	/* And the stores that cannot be emptied say so rather than writing a
	 * blank. A hostname is the case: every camera has one. */
	ASSERT_EQ(-1, rcd_provider_hostname.set(""));
	ASSERT_EQ(-1, rcd_provider_timezone.set(""));
	ASSERT_EQ(-1, rcd_provider_ntp_server.set(""));

	/* The interface method is the exception among these: the stanza has
	 * to name one, and the one an unconfigured camera runs on is dhcp. */
	ASSERT_EQ(0, rcd_provider_net_dhcp.set("false"));
	ASSERT_EQ(0, rcd_provider_net_dhcp.set(""));
	char method[16] = "";
	ASSERT_EQ(0, rcd_provider_net_dhcp.get(method, sizeof(method)));
	ASSERT_STR_EQ("true", method);
	PASS();
}

TEST the_schema_says_where_a_system_key_takes_effect(void)
{
	cJSON *out = cJSON_CreateObject();
	rcd_schema_emit(out, "device");
	const cJSON *keys = cJSON_GetObjectItemCaseSensitive(out, "keys");
	ASSERT(cJSON_IsArray(keys));
	ASSERT_EQ(3, cJSON_GetArraySize(keys));

	int checked = 0;
	const cJSON *k = NULL;
	cJSON_ArrayForEach(k, keys)
	{
		const cJSON *key = cJSON_GetObjectItemCaseSensitive(k, "key");
		const cJSON *tier = cJSON_GetObjectItemCaseSensitive(k, "tier");
		const cJSON *imp = cJSON_GetObjectItemCaseSensitive(k, "impact");
		const cJSON *ro = cJSON_GetObjectItemCaseSensitive(k, "readable");
		const cJSON *own = cJSON_GetObjectItemCaseSensitive(k, "owner");
		ASSERT(cJSON_IsString(key));

		/* A provider-backed key is read from its store, so it must not
		 * be advertised as write-only the way a credential is. */
		ASSERT_EQ(NULL, ro);
		/* And it is owned by the camera, not by a daemon a client
		 * could be invited to restart. */
		ASSERT_STR_EQ("system", own->valuestring);

		if (strcmp(key->valuestring, "timezone") == 0) {
			/* Live because nothing is owed to `apply` -- the file
			 * is written and rcd is done. The reboot is what the
			 * running system needs, and the impact is where that
			 * is said. */
			ASSERT_STR_EQ("live", tier->valuestring);
			ASSERT_STR_EQ("reboot", imp->valuestring);
			/* The whole table, offered as choices. */
			const cJSON *ch = cJSON_GetObjectItemCaseSensitive(k, "choices");
			ASSERT(cJSON_IsArray(ch));
			ASSERT(cJSON_GetArraySize(ch) > 100);
			checked++;
		}
		if (strcmp(key->valuestring, "ntp_server") == 0) {
			/* Written and in force at once: ntpd is restarted by
			 * the setter, so nothing is owed and nothing waits. */
			ASSERT_STR_EQ("live", tier->valuestring);
			ASSERT_STR_EQ("none", imp->valuestring);
			ASSERT_STR_EQ("host",
				      cJSON_GetObjectItemCaseSensitive(k, "type")->valuestring);
			/* Nothing about the time server can cost a client its
			 * way back, so it is not on a clock. */
			ASSERT_EQ(NULL, cJSON_GetObjectItemCaseSensitive(k, "guard_sec"));
			checked++;
		}
		if (strcmp(key->valuestring, "hostname") == 0) {
			/* The one key a client is told to come back and
			 * confirm, and how long it has to do it in. */
			const cJSON *g = cJSON_GetObjectItemCaseSensitive(k, "guard_sec");
			ASSERT(cJSON_IsNumber(g));
			ASSERT_EQ(RCD_GUARD_NAME_SEC, (int)cJSON_GetNumberValue(g));
			/* The restart tier, because the provider can enact and
			 * has not: `set` wrote the file, and the running host
			 * is renamed by `apply`. A key that costs the operator
			 * their way in must be something they press a button
			 * for. */
			ASSERT_STR_EQ("restart", tier->valuestring);
			checked++;
		}
	}
	ASSERT_EQ(3, checked);
	cJSON_Delete(out);
	PASS();
}

/* One request is a form, not a section tree. */
/* ------------------------------------------------------------------ */
/* Labelled integers                                                   */
/* ------------------------------------------------------------------ */

/*
 * A profile or an anti-flicker mode is a name, and a client handed a bare
 * 0-2 can only draw a slider over it. The table carries the names, and the
 * number is still what is written -- rvd and rmr read these back with
 * rss_config_get_int, so a spelled-out value would come back as the default.
 */
TEST labelled_integers_are_written_as_numbers(void)
{
	rcd_edit_t e[RCD_EDITS_MAX];
	int n;

	ASSERT_SET_OK("{\"section\":\"stream0\",\"key\":\"profile\",\"value\":\"high\"}", e, &n);
	ASSERT_EQ(1, n);
	ASSERT_STR_EQ("2", e[0].rendered);

	ASSERT_SET_OK("{\"section\":\"stream0\",\"key\":\"profile\",\"value\":\"baseline\"}", e,
		      &n);
	ASSERT_STR_EQ("0", e[0].rendered);

	ASSERT_SET_OK("{\"section\":\"sensor\",\"key\":\"antiflicker\",\"value\":\"60hz\"}", e, &n);
	ASSERT_STR_EQ("2", e[0].rendered);

	ASSERT_SET_OK("{\"section\":\"recording\",\"key\":\"stream\",\"value\":\"sub\"}", e, &n);
	ASSERT_STR_EQ("1", e[0].rendered);
	PASS();
}

/* The number still works: a client that ignores the labels is not broken by
 * their arrival, which is what makes adding them a compatible change. */
TEST labelled_integers_still_take_the_number(void)
{
	rcd_edit_t e[RCD_EDITS_MAX];
	int n;

	ASSERT_SET_OK("{\"section\":\"stream0\",\"key\":\"profile\",\"value\":2}", e, &n);
	ASSERT_STR_EQ("2", e[0].rendered);
	ASSERT_SET_OK("{\"section\":\"sensor\",\"key\":\"antiflicker\",\"value\":0}", e, &n);
	ASSERT_STR_EQ("0", e[0].rendered);
	PASS();
}

/* An unknown name is a choice refusal, not a type one, and the range still
 * bounds the number. */
TEST labelled_integers_refuse_anything_else(void)
{
	ASSERT_SET_REFUSED("{\"section\":\"stream0\",\"key\":\"profile\",\"value\":\"extended\"}");
	ASSERT_STR_EQ(RCD_E_CHOICE, code);
	ASSERT_SET_REFUSED("{\"section\":\"sensor\",\"key\":\"antiflicker\",\"value\":\"50\"}");
	ASSERT_STR_EQ(RCD_E_CHOICE, code);
	ASSERT_SET_REFUSED("{\"section\":\"stream0\",\"key\":\"profile\",\"value\":3}");
	ASSERT_STR_EQ(RCD_E_RANGE, code);
	PASS();
}

/*
 * An unlabelled integer must keep refusing a string, or every numeric key
 * would start accepting quoted values by accident.
 */
TEST unlabelled_integers_still_refuse_a_string(void)
{
	ASSERT_SET_REFUSED("{\"section\":\"jpeg\",\"key\":\"quality\",\"value\":\"80\"}");
	ASSERT_STR_EQ(RCD_E_TYPE, code);
	PASS();
}

/*
 * An ISP knob takes the word as well as a number, and keeps it as the word.
 *
 * "auto" means the tuning file's own curve -- what the module does at each
 * sensor gain -- and that is not a point on the knob's scale. It used to be
 * said by writing the neutral, which cost the tuner the one value they might
 * have chosen on purpose. Sent to the daemon as a string, so rvd can tell the
 * two requests apart, and written to the file as the same five characters.
 */
TEST an_isp_knob_takes_the_word_auto(void)
{
	rcd_edit_t e[RCD_EDITS_MAX];
	int n = 0;

	ASSERT_SET_OK("{\"section\":\"image\",\"key\":\"contrast\",\"value\":\"auto\"}", e, &n);
	ASSERT_STR_EQ("auto", e[0].rendered);
	ASSERTm("auto reached the daemon as a number", !e[0].is_num);

	/* The numbers still are numbers. */
	ASSERT_SET_OK("{\"section\":\"image\",\"key\":\"contrast\",\"value\":61}", e, &n);
	ASSERT_STR_EQ("61", e[0].rendered);
	ASSERT(e[0].is_num);
	PASS();
}

/* And only where a tuning has a curve to hand back. The gain ceilings and the
 * orientation are this daemon's policy, with no representation in the binary,
 * so the word means nothing there and is a typo rather than a request. */
TEST only_the_isp_knobs_take_it(void)
{
	ASSERT_SET_REFUSED("{\"section\":\"image\",\"key\":\"max_again\",\"value\":\"auto\"}");
	ASSERT_STR_EQ(RCD_E_TYPE, code);
	ASSERT_SET_REFUSED("{\"section\":\"image\",\"key\":\"hflip\",\"value\":\"auto\"}");
	ASSERT_STR_EQ(RCD_E_TYPE, code);
	ASSERT_SET_REFUSED("{\"section\":\"jpeg\",\"key\":\"quality\",\"value\":\"auto\"}");
	ASSERT_STR_EQ(RCD_E_TYPE, code);

	/* The word and nothing near it: a knob cannot be set to "automatic",
	 * and a client that sends one is told so rather than having it
	 * guessed at. */
	ASSERT_SET_REFUSED("{\"section\":\"image\",\"key\":\"contrast\",\"value\":\"Auto\"}");
	ASSERT_SET_REFUSED("{\"section\":\"image\",\"key\":\"contrast\",\"value\":\"automatic\"}");
	PASS();
}

/*
 * A client has no other way to learn the word exists: it is not a number in
 * the range, and a form drawn from the range alone can never say "leave this
 * to the tuning". Said only where it is true.
 */
TEST the_schema_says_which_keys_take_auto(void)
{
	cJSON *out = cJSON_CreateObject();
	rcd_schema_emit(out, NULL);
	const cJSON *keys = cJSON_GetObjectItemCaseSensitive(out, "keys");
	ASSERT(cJSON_IsArray(keys));

	int checked = 0;
	const cJSON *k = NULL;
	cJSON_ArrayForEach(k, keys)
	{
		const cJSON *sec = cJSON_GetObjectItemCaseSensitive(k, "section");
		const cJSON *key = cJSON_GetObjectItemCaseSensitive(k, "key");
		const cJSON *au = cJSON_GetObjectItemCaseSensitive(k, "auto");
		if (!cJSON_IsString(sec) || !cJSON_IsString(key))
			continue;
		if (strcmp(sec->valuestring, "image") != 0)
			continue;
		if (strcmp(key->valuestring, "contrast") == 0) {
			ASSERT(cJSON_IsTrue(au));
			checked++;
		}
		if (strcmp(key->valuestring, "hflip") == 0) {
			ASSERT_EQ(NULL, au);
			checked++;
		}
	}
	ASSERT_EQ(2, checked);
	cJSON_Delete(out);
	PASS();
}

/*
 * Exposure compensation is the one signed knob: it biases the AE target either
 * way, and SigmaStar states it in EV steps around zero. Bounded at 0 here, the
 * whole darker half was unreachable through rcd -- rvd took -3 from the CLI
 * and rcd refused the same edit as out of range.
 */
TEST exposure_compensation_goes_both_ways(void)
{
	rcd_edit_t e[RCD_EDITS_MAX];
	int n = 0;

	ASSERT_SET_OK("{\"section\":\"image\",\"key\":\"ae_comp\",\"value\":-3}", e, &n);
	ASSERT_STR_EQ("-3", e[0].rendered);

	const rcd_key_t *k = rcd_key_find("image", "ae_comp");
	ASSERT(k);
	ASSERTm("ae_comp cannot express a negative bias", k->min < 0);
	PASS();
}

/*
 * And the word survives the round trip. Read as a number it is 0 -- in range,
 * on the scale, and wrong: a knob following the tuning would be reported as
 * one pinned at its floor, and a client writing that value back would pin it
 * there for real.
 */
TEST a_knob_left_on_auto_reads_back_as_auto(void)
{
	rcd_state_t st;
	char path[320];

	if (!sysconf_dir_ready())
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	snprintf(path, sizeof(path), "%s/raptor.conf", RCD_SYSCONF_DIR);
	FILE *f = fopen(path, "w");
	ASSERT(f);
	fputs("[image]\ncontrast = auto\nsharpness = 90\n", f);
	fclose(f);

	memset(&st, 0, sizeof(st));
	st.config_path = path;

	cJSON *req = cJSON_Parse("{\"keys\":[{\"section\":\"image\",\"key\":\"contrast\"},"
				 "{\"section\":\"image\",\"key\":\"sharpness\"}]}");
	cJSON *resp = rcd_cmd_get(&st, req);
	cJSON_Delete(req);
	ASSERT(resp);

	const cJSON *vals = cJSON_GetObjectItemCaseSensitive(resp, "values");
	const cJSON *con = cJSON_GetArrayItem(vals, 0);
	const cJSON *shp = cJSON_GetArrayItem(vals, 1);
	const cJSON *cv = cJSON_GetObjectItemCaseSensitive(con, "value");
	const cJSON *sv = cJSON_GetObjectItemCaseSensitive(shp, "value");

	ASSERTm("a knob on auto read back as a number", cJSON_IsString(cv));
	ASSERT_STR_EQ("auto", cv->valuestring);
	ASSERTm("a knob with a value read back as a string", cJSON_IsNumber(sv));
	ASSERT_EQ(90, (int)cJSON_GetNumberValue(sv));

	cJSON_Delete(resp);
	unlink(path);
	PASS();
}

/* Every label array must cover exactly the key's range, or a value in the
 * middle of it would have no name and a client would render a gap. */
TEST every_label_array_spans_its_range(void)
{
	for (int i = 0;; i++) {
		const rcd_key_t *k = rcd_key_at(i);
		if (!k)
			break;
		if (k->type != V_INT || !k->choices)
			continue;
		int n = 0;
		while (k->choices[n])
			n++;
		ASSERT_EQm(k->key, k->max - k->min + 1, n);
	}
	PASS();
}

/*
 * The labels reach the wire, and only where there are any. A client renders
 * this key as named choices and every other integer as a number, without
 * being told which is which anywhere but here.
 */
TEST schema_carries_the_labels(void)
{
	cJSON *out = cJSON_CreateObject();
	rcd_schema_emit(out, NULL);
	const cJSON *keys = cJSON_GetObjectItemCaseSensitive(out, "keys");
	ASSERT(cJSON_IsArray(keys));

	int checked = 0;
	const cJSON *k = NULL;
	cJSON_ArrayForEach(k, keys)
	{
		const cJSON *sec = cJSON_GetObjectItemCaseSensitive(k, "section");
		const cJSON *key = cJSON_GetObjectItemCaseSensitive(k, "key");
		const cJSON *lab = cJSON_GetObjectItemCaseSensitive(k, "labels");
		if (!cJSON_IsString(sec) || !cJSON_IsString(key))
			continue;
		if (strcmp(sec->valuestring, "stream0") == 0 &&
		    strcmp(key->valuestring, "profile") == 0) {
			ASSERT(cJSON_IsArray(lab));
			ASSERT_EQ(3, cJSON_GetArraySize(lab));
			ASSERT_STR_EQ("baseline", cJSON_GetArrayItem(lab, 0)->valuestring);
			ASSERT_STR_EQ("high", cJSON_GetArrayItem(lab, 2)->valuestring);
			checked++;
		}
		if (strcmp(sec->valuestring, "jpeg") == 0 &&
		    strcmp(key->valuestring, "quality") == 0) {
			ASSERT_EQ(NULL, lab);
			checked++;
		}
	}
	ASSERT_EQ(2, checked);
	cJSON_Delete(out);
	PASS();
}

/* ------------------------------------------------------------------ */
/* What the silicon actually has                                       */
/* ------------------------------------------------------------------ */

/*
 * The table is the same on every platform and more than half of [image] is
 * absent on some of them. Nothing is hidden until rvd has said so, because
 * hiding a working control is worse than showing one that turns out to
 * refuse -- and a key rvd publishes but rejects while the channel is up
 * (orientation, on SigmaStar) is available and must stay visible.
 */
TEST nothing_is_unavailable_until_the_camera_says_so(void)
{
	rcd_state_t st = {0};

	/* rvd has not answered: everything stands. */
	ASSERT(rcd_key_available(&st, rcd_key_find("image", "sinter")));
	ASSERT(rcd_key_available(&st, rcd_key_find("image", "dpc_strength")));

	/* What an i6c reports. */
	snprintf(st.isp_settable, sizeof(st.isp_settable),
		 ",brightness,contrast,saturation,sharpness,temper,hflip,vflip,ae_comp,"
		 "defog_strength,");

	ASSERT(rcd_key_available(&st, rcd_key_find("image", "brightness")));
	ASSERT(rcd_key_available(&st, rcd_key_find("image", "defog_strength")));
	ASSERT_EQ(false, rcd_key_available(&st, rcd_key_find("image", "sinter")));
	ASSERT_EQ(false, rcd_key_available(&st, rcd_key_find("image", "hue")));
	ASSERT_EQ(false, rcd_key_available(&st, rcd_key_find("image", "max_again")));
	ASSERT_EQ(false, rcd_key_available(&st, rcd_key_find("image", "dpc_strength")));

	/* Published and refused live is not the same as absent. */
	ASSERT(rcd_key_available(&st, rcd_key_find("image", "hflip")));
	ASSERT(rcd_key_available(&st, rcd_key_find("image", "vflip")));

	/* The list answers for [image] and nothing else. */
	ASSERT(rcd_key_available(&st, rcd_key_find("stream0", "bitrate")));
	ASSERT(rcd_key_available(&st, rcd_key_find("sensor", "antiflicker")));
	ASSERT(rcd_key_available(&st, rcd_key_find("audio", "volume")));
	PASS();
}

/* A key name that is a suffix of another must not match it: the list is
 * comma-terminated on both sides so ",again," cannot find "max_again". */
TEST availability_matches_whole_key_names(void)
{
	rcd_state_t st = {0};
	snprintf(st.isp_settable, sizeof(st.isp_settable), ",max_again,");
	ASSERT(rcd_key_available(&st, rcd_key_find("image", "max_again")));
	ASSERT_EQ(false, rcd_key_available(&st, rcd_key_find("image", "max_dgain")));
	PASS();
}

TEST refuses_more_edits_than_a_request_may_carry(void)
{
	char json[8192];
	int n = snprintf(json, sizeof(json), "{\"edits\":[");
	for (int i = 0; i < RCD_EDITS_MAX + 4; i++)
		n += snprintf(json + n, sizeof(json) - (size_t)n,
			      "%s{\"section\":\"image\",\"key\":\"brightness\",\"value\":%d}",
			      i ? "," : "", i);
	snprintf(json + n, sizeof(json) - (size_t)n, "]}");

	ASSERT_SET_REFUSED(json);
	ASSERT_STR_EQ(RCD_E_TOOMANY, code);
	PASS();
}

/* ------------------------------------------------------------------ */
/* The refusal itself                                                  */
/* ------------------------------------------------------------------ */

/*
 * A refusal reaches whoever sent the command, so it has to say what was wrong
 * with this request -- and a machine-readable code beside the prose, because
 * nothing should ever have to parse the prose.
 */
TEST every_refusal_explains_itself(void)
{
	static const char *const bad[] = {
		"{\"section\":\"nope\",\"key\":\"x\",\"value\":1}",
		"{\"section\":\"jpeg\",\"key\":\"quality\",\"value\":999}",
		"{\"section\":\"jpeg\",\"key\":\"quality\",\"value\":\"x\"}",
		"{\"section\":\"audio\",\"key\":\"codec\",\"value\":\"flac\"}",
		"{\"edits\":[]}",
		NULL,
	};

	for (int i = 0; bad[i]; i++) {
		rcd_edit_t e[RCD_EDITS_MAX];
		int n;
		ASSERT_EQm(bad[i], -1, validate_set(bad[i], e, &n));
		ASSERT_EQm(bad[i], 1, code[0] != '\0');
		ASSERT_EQm(bad[i], 1, reason[0] != '\0');
		/* The code is one of the closed set, not free-form. */
		ASSERT(strcmp(code, RCD_E_MALFORMED) == 0 || strcmp(code, RCD_E_UNKNOWN) == 0 ||
		       strcmp(code, RCD_E_TYPE) == 0 || strcmp(code, RCD_E_RANGE) == 0 ||
		       strcmp(code, RCD_E_CHOICE) == 0 || strcmp(code, RCD_E_TOOMANY) == 0);
	}
	PASS();
}

/* ------------------------------------------------------------------ */
/* Reset: putting a key back to its default                             */
/* ------------------------------------------------------------------ */

/*
 * A null value is a reset, and it is the only thing that is. rcd holds no
 * defaults to write -- every one of them is the argument at its own read site
 * inside the owning daemon -- so what a reset does is take the key out and let
 * that read site answer again.
 */
TEST a_null_value_asks_for_the_default(void)
{
	rcd_edit_t edits[RCD_EDITS_MAX];
	int n = 0;

	ASSERT_EQ(0, validate_set("{\"section\":\"stream0\",\"key\":\"gop\",\"value\":null}", edits,
				  &n));
	ASSERT_EQ(1, n);
	ASSERT_EQ(true, edits[0].reset);
	ASSERTm("a reset rendered a value to write", edits[0].rendered[0] == '\0');

	/* An ordinary edit is not a reset, and says so. */
	ASSERT_EQ(0, validate_set("{\"section\":\"stream0\",\"key\":\"gop\",\"value\":50}", edits,
				  &n));
	ASSERT_EQ(false, edits[0].reset);

	/* A request that simply forgot the value still means what it always
	 * did. The two are distinguishable on the wire and mean opposite
	 * things, so they are not folded together. */
	ASSERT_EQ(-1, validate_set("{\"section\":\"stream0\",\"key\":\"gop\"}", edits, &n));
	ASSERT_STR_EQ(RCD_E_TYPE, code);
	PASS();
}

/* A store that must always hold something has nothing to go back to, and the
 * refusal says which key rather than failing the whole form silently. */
TEST a_key_with_no_default_refuses_the_reset(void)
{
	rcd_edit_t edits[RCD_EDITS_MAX];
	int n = 0;

	ASSERT_EQ(-1, validate_set("{\"section\":\"device\",\"key\":\"hostname\",\"value\":null}",
				   edits, &n));
	ASSERT(strstr(reason, "hostname") != NULL);
	ASSERT_EQ(-1, validate_set("{\"section\":\"device\",\"key\":\"timezone\",\"value\":null}",
				   edits, &n));
	ASSERT_EQ(-1, validate_set("{\"section\":\"device\",\"key\":\"ntp_server\",\"value\":null}",
				   edits, &n));

	/* The address keys do have one: not being configured is a
	 * configuration, and it is the one the camera ships with. */
	ASSERT_EQ(0, validate_set("{\"section\":\"network\",\"key\":\"address\",\"value\":null}",
				  edits, &n));
	ASSERT_EQ(true, edits[0].reset);
	PASS();
}

/* A batch is still all-or-nothing: one key that cannot be reset refuses the
 * whole form rather than resetting the rest of it. */
TEST one_key_with_no_default_refuses_the_whole_batch(void)
{
	rcd_edit_t edits[RCD_EDITS_MAX];
	int n = 0;

	ASSERT_EQ(-1, validate_set("{\"edits\":["
				   "{\"section\":\"network\",\"key\":\"address\",\"value\":null},"
				   "{\"section\":\"device\",\"key\":\"hostname\",\"value\":null}]}",
				   edits, &n));
	PASS();
}

/* The schema says so, and says it only where the answer is no: a client that
 * has never heard of reset draws every key exactly as it did before. */
TEST the_schema_names_the_keys_that_cannot_be_reset(void)
{
	cJSON *out = cJSON_CreateObject();
	rcd_schema_emit(out, "device");
	const cJSON *keys = cJSON_GetObjectItemCaseSensitive(out, "keys");
	ASSERT(cJSON_IsArray(keys));

	int seen = 0;
	const cJSON *k = NULL;
	cJSON_ArrayForEach(k, keys)
	{
		const cJSON *r = cJSON_GetObjectItemCaseSensitive(k, "resettable");
		ASSERT(cJSON_IsFalse(r));
		seen++;
	}
	ASSERT_EQ(3, seen);
	cJSON_Delete(out);

	out = cJSON_CreateObject();
	rcd_schema_emit(out, "network");
	keys = cJSON_GetObjectItemCaseSensitive(out, "keys");
	ASSERT(cJSON_IsArray(keys));
	ASSERT_EQ(5, cJSON_GetArraySize(keys));
	cJSON_ArrayForEach(k, keys)
		ASSERTm("an address key was advertised as unresettable",
			cJSON_GetObjectItemCaseSensitive(k, "resettable") == NULL);
	cJSON_Delete(out);

	/* And a key kept in raptor.conf is always resettable: removing its
	 * line is available whatever the key is. */
	out = cJSON_CreateObject();
	rcd_schema_emit(out, "stream0");
	keys = cJSON_GetObjectItemCaseSensitive(out, "keys");
	cJSON_ArrayForEach(k, keys)
		ASSERT(cJSON_GetObjectItemCaseSensitive(k, "resettable") == NULL);
	cJSON_Delete(out);
	PASS();
}

/*
 * End to end over a real config file: the line goes, the owner is recorded
 * behind, and a second reset of the same key costs nothing. That last part is
 * what makes a whole-section reset usable -- most of a section is already at
 * its default, and charging a restart for each of those would make the button
 * something nobody dares press.
 */
TEST a_reset_removes_the_line_and_only_charges_for_what_moved(void)
{
	rcd_state_t st;
	char path[320];

	if (!sysconf_dir_ready())
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	snprintf(path, sizeof(path), "%s/raptor.conf", RCD_SYSCONF_DIR);
	FILE *f = fopen(path, "w");
	ASSERT(f);
	fputs("[stream0]\n"
	      "gop = 50    # keyframe interval\n"
	      "width = 1920\n",
	      f);
	fclose(f);

	memset(&st, 0, sizeof(st));
	st.config_path = path;

	cJSON *req = cJSON_Parse("{\"section\":\"stream0\",\"key\":\"gop\",\"value\":null}");
	ASSERT(req);
	cJSON *resp = rcd_cmd_set(&st, req);
	cJSON_Delete(req);
	ASSERT(resp);

	const cJSON *res = cJSON_GetObjectItemCaseSensitive(resp, "results");
	const cJSON *one = cJSON_GetArrayItem(res, 0);
	ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(one, "reset")));
	ASSERTm("a reset echoed a value rcd does not have",
		cJSON_GetObjectItemCaseSensitive(one, "value") == NULL);
	ASSERT_STR_EQ("saved", cJSON_GetObjectItemCaseSensitive(one, "applied")->valuestring);
	cJSON_Delete(resp);

	/* The key is gone; its neighbour and the comment on it are not. */
	char text[512] = "";
	f = fopen(path, "r");
	ASSERT(f);
	fread(text, 1, sizeof(text) - 1, f);
	fclose(f);
	ASSERTm("the reset key kept its line", strstr(text, "gop = 50") == NULL);
	ASSERT(strstr(text, "width = 1920") != NULL);

	ASSERT_EQ(1, st.stale_count);

	/* Again, on a key that is now already at its default. */
	st.stale_count = 0;
	req = cJSON_Parse("{\"section\":\"stream0\",\"key\":\"gop\",\"value\":null}");
	resp = rcd_cmd_set(&st, req);
	cJSON_Delete(req);
	ASSERT(resp);
	res = cJSON_GetObjectItemCaseSensitive(resp, "results");
	one = cJSON_GetArrayItem(res, 0);
	ASSERT_STR_EQ("live", cJSON_GetObjectItemCaseSensitive(one, "applied")->valuestring);
	cJSON_Delete(resp);
	ASSERTm("resetting a key already at its default asked for a restart", 0 == st.stale_count);

	unlink(path);
	PASS();
}

/* A reset of a live key is not live: there is no value to hand the running
 * daemon, and the default it will read is in the daemon, not in rcd. So it
 * goes to the file and the owner is recorded behind, whatever the key's tier
 * is when it carries a value. */
TEST a_reset_is_never_live(void)
{
	rcd_state_t st;
	char path[320];

	if (!sysconf_dir_ready())
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	snprintf(path, sizeof(path), "%s/raptor.conf", RCD_SYSCONF_DIR);
	FILE *f = fopen(path, "w");
	ASSERT(f);
	fputs("[image]\nbrightness = 140\n", f);
	fclose(f);

	memset(&st, 0, sizeof(st));
	st.config_path = path;

	const rcd_key_t *k = rcd_key_find("image", "brightness");
	ASSERT(k);
	ASSERTm("the key under test is not a live one", rcd_key_live(k));

	cJSON *req = cJSON_Parse("{\"section\":\"image\",\"key\":\"brightness\",\"value\":null}");
	cJSON *resp = rcd_cmd_set(&st, req);
	cJSON_Delete(req);
	ASSERT(resp);

	const cJSON *one = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(resp, "results"), 0);
	ASSERT_STR_EQ("saved", cJSON_GetObjectItemCaseSensitive(one, "applied")->valuestring);
	cJSON_Delete(resp);

	char text[256] = "";
	f = fopen(path, "r");
	ASSERT(f);
	fread(text, 1, sizeof(text) - 1, f);
	fclose(f);
	ASSERT(strstr(text, "brightness") == NULL);
	ASSERT_EQ(1, st.stale_count);

	unlink(path);
	PASS();
}

/* Resetting the address keys puts the stanza back to the one that shipped --
 * including the method, because an interface nobody has configured does DHCP,
 * and a section left half-reset is a camera with a gateway it cannot use. */
TEST resetting_the_network_puts_the_shipped_stanza_back(void)
{
	char before[512], after[512];

	if (!iface_ready())
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	iface_read(before, sizeof(before));

	ASSERT_EQ(0, rcd_provider_net_dhcp.set("false"));
	ASSERT_EQ(0, rcd_provider_net_address.set("192.168.1.99"));
	ASSERT_EQ(0, rcd_provider_net_netmask.set("255.255.255.0"));
	ASSERT_EQ(0, rcd_provider_net_gateway.set("192.168.1.1"));

	iface_read(after, sizeof(after));
	ASSERT(strcmp(before, after) != 0);

	/* What a reset of each of them comes to. */
	ASSERT_EQ(0, rcd_provider_net_gateway.set(""));
	ASSERT_EQ(0, rcd_provider_net_netmask.set(""));
	ASSERT_EQ(0, rcd_provider_net_address.set(""));
	ASSERT_EQ(0, rcd_provider_net_dhcp.set(""));

	iface_read(after, sizeof(after));
	ASSERT_STR_EQm("the shipped stanza did not come back", before, after);
	PASS();
}

/* ================================================================
 * Waiting for a daemon to come back
 * ================================================================ */

/*
 * A probe that is slow rather than instant, which is the only shape in which
 * the bug these tests exist for was visible.
 *
 * `wait_probe_ms` is what one call costs -- standing in for the IPC round trip
 * a real probe makes, which blocks for RCD_PROBE_TIMEOUT_MS or, for rvd's
 * status, RCD_CTRL_TIMEOUT_MS whenever the daemon is listening but not
 * answering. That is the state an apply is most likely to hit, since it is what
 * a daemon mid-restart looks like.
 */
static int wait_probe_ms;
static int wait_probe_calls;
static int wait_probe_true_after;

static bool slow_probe(const char *arg)
{
	(void)arg;
	if (wait_probe_ms > 0)
		usleep((useconds_t)wait_probe_ms * 1000);
	return ++wait_probe_calls >= wait_probe_true_after;
}

static void wait_probe_reset(int cost_ms, int true_after)
{
	wait_probe_ms = cost_ms;
	wait_probe_calls = 0;
	wait_probe_true_after = true_after;
}

static unsigned int elapsed_ms_of(void (*run)(void))
{
	int64_t t0 = rss_timestamp_us();
	run();
	return (unsigned int)((rss_timestamp_us() - t0) / 1000);
}

static bool wait_result;
static unsigned int wait_reported;

static void run_never_ready(void)
{
	wait_result = rcd_wait_until(slow_probe, "rvd", 300, &wait_reported);
}

/* A budget deliberately smaller than one probe, so the measured time and the
 * budget are far apart and cannot be confused for one another. */
static void run_probe_outlasts_budget(void)
{
	wait_result = rcd_wait_until(slow_probe, "rvd", 200, &wait_reported);
}

/*
 * The budget is wall time, not a count of sleeps.
 *
 * It used to be the count: `for (waited = 0; waited < BUDGET; waited +=
 * POLL_STEP)`, charging the budget for the sleeping only while each iteration
 * also spent however long the probe took. With POLL_STEP at 200 ms and rvd's
 * status probe blocking for 2000, a 25 s budget bought 125 iterations and ran
 * for about 275 s -- and rcd's serve loop is synchronous, so it answered
 * nothing for those four minutes, guard ticks included.
 *
 * The numbers here are that arithmetic in miniature: a 300 ms budget, a 300 ms
 * probe and the real 200 ms step. Counting sleeps gives two iterations and
 * about 1000 ms; against a clock it is one probe and about 300. The bound
 * below sits between the two, well clear of either.
 */
TEST the_wait_budget_is_wall_time_not_a_count_of_sleeps(void)
{
	wait_probe_reset(300, 1000000); /* never ready */

	unsigned int real = elapsed_ms_of(run_never_ready);

	ASSERT_FALSE(wait_result);
	ASSERT(real < 700);
	/* One probe, then the deadline is already spent. Counting sleeps would
	 * have taken two. */
	ASSERT_EQ(1, wait_probe_calls);
	PASS();
}

/*
 * And the caller is told what it actually waited, not what it was budgeted.
 *
 * The two used to be the same number -- the failure message printed
 * UP_WAIT_MS / 1000 whatever had happened -- which is how a 275 s wait reported
 * itself as 25 s. They were never the same quantity.
 */
TEST the_wait_reports_the_time_it_measured(void)
{
	/*
	 * A 500 ms probe against a 200 ms budget, which is the real shape of
	 * this: rvd's status probe blocks for RCD_CTRL_TIMEOUT_MS, far longer
	 * than the step the loop is paced by. One probe runs, the budget is
	 * already spent when it returns, and the honest answer is about 500 --
	 * a number the budget cannot produce, which is the point. Reporting the
	 * constant would say 200.
	 */
	wait_probe_reset(500, 1000000);

	unsigned int real = elapsed_ms_of(run_probe_outlasts_budget);

	ASSERT_FALSE(wait_result);
	ASSERT_EQ(1, wait_probe_calls);
	ASSERT(wait_reported >= 400);
	ASSERT(wait_reported < 900);

	/*
	 * And it also documents the ceiling: a budget bounds when a probe may
	 * start, not when one already in flight must return, so the wait can
	 * exceed its budget by one probe. There is no way to abandon a call
	 * mid-flight, and the arithmetic this replaced went wrong by pretending
	 * otherwise.
	 */
	ASSERT(real > 200);
	PASS();
}

static void run_ready_second_try(void)
{
	wait_result = rcd_wait_until(slow_probe, "rvd", 5000, &wait_reported);
}

/*
 * A daemon that comes back is not made to wait out the budget, and the probe
 * runs before any sleep -- a restart that finished while the request was in
 * flight should cost nothing at all.
 */
TEST a_daemon_that_answers_is_not_waited_out(void)
{
	wait_probe_reset(0, 1); /* ready on the first call */

	unsigned int real = elapsed_ms_of(run_ready_second_try);

	ASSERT(wait_result);
	ASSERT_EQ(1, wait_probe_calls);
	ASSERT(real < 100);

	/* And one that needs a second look still returns as soon as it is
	 * ready, having slept exactly one step in between. */
	wait_probe_reset(0, 2);
	real = elapsed_ms_of(run_ready_second_try);
	ASSERT(wait_result);
	ASSERT_EQ(2, wait_probe_calls);
	ASSERT(real >= 150 && real < 500);
	PASS();
}

/*
 * A budget of zero still probes once. Waiting is an optimisation over asking;
 * a caller that budgeted nothing still wants the answer, and a loop that
 * checked the clock first would return false without ever looking.
 */
TEST a_spent_budget_still_asks_once(void)
{
	wait_probe_reset(0, 1);
	ASSERT(rcd_wait_until(slow_probe, "rvd", 0, NULL));
	ASSERT_EQ(1, wait_probe_calls);

	wait_probe_reset(0, 1000000);
	ASSERT_FALSE(rcd_wait_until(slow_probe, "rvd", 0, NULL));
	ASSERT_EQ(1, wait_probe_calls);
	PASS();
}

SUITE(rcd_cmd_suite)
{
	RUN_TEST(refuses_the_named_hazards);
	RUN_TEST(keeps_credential_sections_unreadable);
	RUN_TEST(no_credential_is_ever_readable);
	RUN_TEST(refuses_unlisted_actions);
	RUN_TEST(refuses_near_misses);
	RUN_TEST(refuses_malformed_payloads);
	RUN_TEST(drops_fields_the_table_does_not_name);
	RUN_TEST(rewrites_the_action_name);
	RUN_TEST(enforces_enum_choices);
	RUN_TEST(enforces_action_ranges_and_types);
	RUN_TEST(rejects_values_that_would_wrap_an_int);
	RUN_TEST(requires_required_fields_and_allows_optional_ones);

	RUN_TEST(refuses_writes_outside_the_key_table);
	RUN_TEST(accepts_a_single_edit_in_either_shape);
	RUN_TEST(applies_a_batch_all_or_nothing);
	RUN_TEST(enforces_types_and_ranges_on_edits);
	RUN_TEST(renders_every_value_as_the_file_spells_it);
	RUN_TEST(names_edits_from_the_table_not_the_payload);

	RUN_TEST(accepts_credentials_within_their_grammar);
	RUN_TEST(refuses_credentials_that_could_mean_something_else);
	RUN_TEST(never_quotes_a_rejected_credential_back);

	RUN_TEST(the_table_decides_the_tier);
	RUN_TEST(channelled_keys_carry_their_own_channel);
	RUN_TEST(every_writable_key_has_an_owner);
	RUN_TEST(impact_separates_the_pipeline_from_the_stream);
	RUN_TEST(the_zone_table_is_two_arrays_of_one_length);
	RUN_TEST(the_timezone_is_an_enum_over_the_zone_table);
	RUN_TEST(a_host_is_a_hostname_or_an_address_and_nothing_else);
	RUN_TEST(a_provider_key_round_trips_through_its_store);
	RUN_TEST(an_unconfirmed_change_goes_back_when_the_window_ends);
	RUN_TEST(a_confirmed_change_stays_and_leaves_nothing_armed);
	RUN_TEST(cancelling_puts_it_back_without_waiting);
	RUN_TEST(a_reboot_inside_the_window_reverts);
	RUN_TEST(an_rcd_restart_inside_the_window_keeps_it_armed);
	RUN_TEST(a_second_change_inside_the_window_keeps_the_first_snapshot);
	RUN_TEST(cancelling_a_staged_change_leaves_nothing_owed);
	RUN_TEST(a_revert_that_cannot_write_keeps_the_record_and_tries_again);
	RUN_TEST(a_revert_that_never_lands_leaves_the_record_for_a_power_cycle);
	RUN_TEST(a_cancel_that_could_not_write_says_so);
	RUN_TEST(an_enact_that_did_not_take_stays_owed);
	RUN_TEST(state_writes_out_the_drift_it_clears);
	RUN_TEST(a_dns_snapshot_does_not_pin_what_dhcp_supplied);
	RUN_TEST(the_guard_reports_itself_only_while_it_is_armed);
	RUN_TEST(an_address_is_four_octets_and_nothing_else);
	RUN_TEST(the_interface_stanza_keeps_what_it_did_not_write);
	RUN_TEST(clearing_an_address_removes_its_line);
	RUN_TEST(every_network_key_is_guarded_and_waits_for_apply);
	RUN_TEST(a_revert_leaves_the_stores_it_did_not_change_alone);
	RUN_TEST(a_revert_can_unset_a_value_that_was_never_there);
	RUN_TEST(a_null_value_asks_for_the_default);
	RUN_TEST(a_key_with_no_default_refuses_the_reset);
	RUN_TEST(one_key_with_no_default_refuses_the_whole_batch);
	RUN_TEST(the_schema_names_the_keys_that_cannot_be_reset);
	RUN_TEST(a_reset_removes_the_line_and_only_charges_for_what_moved);
	RUN_TEST(a_reset_is_never_live);
	RUN_TEST(resetting_the_network_puts_the_shipped_stanza_back);
	RUN_TEST(the_schema_says_where_a_system_key_takes_effect);
	RUN_TEST(nothing_is_unavailable_until_the_camera_says_so);
	RUN_TEST(availability_matches_whole_key_names);
	RUN_TEST(schema_carries_the_labels);
	RUN_TEST(labelled_integers_are_written_as_numbers);
	RUN_TEST(labelled_integers_still_take_the_number);
	RUN_TEST(labelled_integers_refuse_anything_else);
	RUN_TEST(unlabelled_integers_still_refuse_a_string);
	RUN_TEST(every_label_array_spans_its_range);
	RUN_TEST(an_isp_knob_takes_the_word_auto);
	RUN_TEST(only_the_isp_knobs_take_it);
	RUN_TEST(the_schema_says_which_keys_take_auto);
	RUN_TEST(exposure_compensation_goes_both_ways);
	RUN_TEST(a_knob_left_on_auto_reads_back_as_auto);
	RUN_TEST(refuses_more_edits_than_a_request_may_carry);

	RUN_TEST(the_wait_budget_is_wall_time_not_a_count_of_sleeps);
	RUN_TEST(the_wait_reports_the_time_it_measured);
	RUN_TEST(a_daemon_that_answers_is_not_waited_out);
	RUN_TEST(a_spent_budget_still_asks_once);

	RUN_TEST(every_refusal_explains_itself);
}
