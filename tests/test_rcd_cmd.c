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
#include "../rcd/rcd_config.h"
#include "../rcd/rcd_guard.h"
#include "../rcd/rcd_network.h"
#include "../rcd/rcd_proto.h"
#include "../rcd/rcd_schema.h"
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

	ASSERT_EQ(0, validate_set("{\"section\":\"system\",\"key\":\"timezone\","
				  "\"value\":\"America/Los_Angeles\"}",
				  e, &n));
	ASSERT_STR_EQ("America/Los_Angeles", e[0].rendered);

	/* A zone this build does not carry is refused by name rather than
	 * written and discovered at the next boot. */
	ASSERT_EQ(-1, validate_set("{\"section\":\"system\",\"key\":\"timezone\","
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
			 "{\"section\":\"system\",\"key\":\"ntp_server\",\"value\":\"%s\"}",
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
			 "{\"section\":\"system\",\"key\":\"ntp_server\",\"value\":\"%s\"}",
			 bad[i]);
		ASSERT_EQm(bad[i], -1, validate_set(req, e, &n));
	}

	/* And it is a string: a number is not a lenient spelling of one. */
	ASSERT_EQ(-1, validate_set("{\"section\":\"system\",\"key\":\"ntp_server\","
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
	rcd_stale_add(&st, "system", "hostname", RCD_D_COUNT);
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
	rcd_stale_add(&st, "system", "hostname", RCD_D_COUNT);
	cJSON *same = rcd_cmd_cancel(&st, NULL);
	cJSON_Delete(same);
	ASSERT_EQ(0, st.stale_count);

	/* And the stores that cannot be emptied say so rather than writing a
	 * blank. A hostname is the case: every camera has one. */
	ASSERT_EQ(-1, rcd_provider_hostname.set(""));
	ASSERT_EQ(-1, rcd_provider_timezone.set(""));
	ASSERT_EQ(-1, rcd_provider_ntp_server.set(""));
	ASSERT_EQ(-1, rcd_provider_net_dhcp.set(""));
	PASS();
}

TEST the_schema_says_where_a_system_key_takes_effect(void)
{
	cJSON *out = cJSON_CreateObject();
	rcd_schema_emit(out, "system");
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
	RUN_TEST(the_guard_reports_itself_only_while_it_is_armed);
	RUN_TEST(an_address_is_four_octets_and_nothing_else);
	RUN_TEST(the_interface_stanza_keeps_what_it_did_not_write);
	RUN_TEST(clearing_an_address_removes_its_line);
	RUN_TEST(every_network_key_is_guarded_and_waits_for_apply);
	RUN_TEST(a_revert_leaves_the_stores_it_did_not_change_alone);
	RUN_TEST(a_revert_can_unset_a_value_that_was_never_there);
	RUN_TEST(the_schema_says_where_a_system_key_takes_effect);
	RUN_TEST(nothing_is_unavailable_until_the_camera_says_so);
	RUN_TEST(availability_matches_whole_key_names);
	RUN_TEST(schema_carries_the_labels);
	RUN_TEST(labelled_integers_are_written_as_numbers);
	RUN_TEST(labelled_integers_still_take_the_number);
	RUN_TEST(labelled_integers_refuse_anything_else);
	RUN_TEST(unlabelled_integers_still_refuse_a_string);
	RUN_TEST(every_label_array_spans_its_range);
	RUN_TEST(refuses_more_edits_than_a_request_may_carry);

	RUN_TEST(every_refusal_explains_itself);
}
