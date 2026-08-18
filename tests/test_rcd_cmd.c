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

TEST the_schema_says_where_a_system_key_takes_effect(void)
{
	cJSON *out = cJSON_CreateObject();
	rcd_schema_emit(out, "system");
	const cJSON *keys = cJSON_GetObjectItemCaseSensitive(out, "keys");
	ASSERT(cJSON_IsArray(keys));
	ASSERT_EQ(2, cJSON_GetArraySize(keys));

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
			checked++;
		}
	}
	ASSERT_EQ(2, checked);
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
