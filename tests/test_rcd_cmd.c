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

#include <raptor_hal.h>

#include "greatest.h"
#include "../rcd/rcd.h"
#include "../rcd/rcd_apply.h"
#include "../rcd/rcd_config.h"
#include "../rcd/rcd_guard.h"
#include "../rcd/rcd_network.h"
#include "../rcd/rcd_osd.h"
#include "../rcd/rcd_proto.h"
#include "../rcd/rcd_schema.h"
#include "../rcd/rcd_state.h"
#include "../rcd/rcd_passwd.h"
#include "../rcd/rcd_system.h"
#include "../rcd/rcd_wifi.h"

#include <poll.h>
#include <pthread.h>
#include <sys/un.h>
#include <sys/socket.h>
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

/*
 * A repeat row is served apart from the keys, and every key served is a
 * section a client may address.
 *
 * `keys` is what a client renders into forms and caches against `rev`, so it
 * has to be the same table on every camera with this build -- which is why a
 * row standing for sections the config named goes in a field of its own
 * instead. A client that has never heard of the field sees exactly the page
 * it saw before, and one that has knows it must ask the file which sections
 * exist before it can draw them.
 */
TEST a_repeat_row_is_served_apart_from_the_keys(void)
{
	cJSON *out = cJSON_CreateObject();
	rcd_schema_emit(out, NULL);

	const cJSON *keys = cJSON_GetObjectItemCaseSensitive(out, "keys");
	const cJSON *reps = cJSON_GetObjectItemCaseSensitive(out, "repeat_keys");
	const cJSON *k = NULL;
	int n = 0;

	ASSERT(cJSON_IsArray(keys));
	ASSERTm("the schema did not say what a repeat row is", cJSON_IsArray(reps));

	cJSON_ArrayForEach(k, keys)
	{
		const cJSON *sec = cJSON_GetObjectItemCaseSensitive(k, "section");

		ASSERT(cJSON_IsString(sec));
		ASSERT_EQm("a row standing for many sections was served as one", false,
			   rcd_row_repeats(sec->valuestring));
	}

	cJSON_ArrayForEach(k, reps)
	{
		const cJSON *sec = cJSON_GetObjectItemCaseSensitive(k, "section");

		ASSERT(cJSON_IsString(sec));
		ASSERT_STR_EQ("osd.*", sec->valuestring);
		/* rod re-reads them, and says so, the same as any other key. */
		ASSERT_STR_EQ("rod",
			      cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(k, "owner")));
		n++;
	}
	ASSERT_EQm("an element is four keys", 4, n);

	cJSON_Delete(out);
	PASS();
}

/*
 * What an element may be called.
 *
 * The name is the one argument in the table whose bytes are the caller's own
 * -- everything else is a number or one of the table's own strings -- because
 * a name is the one thing a table fixed at compile time cannot hold a list
 * of. So the grammar is what stands between a JSON string and a section
 * header in the camera's config file, and it is tight enough to be a config
 * file's section name, a JSON string and a shell word at once.
 */
TEST an_element_name_is_a_name(void)
{
	char wire[RCD_REQ_MAX];
	const char *owner = NULL;

	/* A name rod can hold, in the spellings a person would write. */
	ASSERT_EQm(reason, 0,
		   validate_action("{\"action\":\"osd-add\",\"name\":\"timestamp\"}", wire,
				   sizeof(wire), &owner));
	ASSERT_STR_EQ("rod", owner);
	ASSERTm("the name did not reach the daemon's request", strstr(wire, "timestamp") != NULL);
	ASSERTm("the action did not become rod's own verb", strstr(wire, "add-element") != NULL);

	ASSERT_EQ(0, validate_action("{\"action\":\"osd-add\",\"name\":\"cam-2_front\"}", wire,
				     sizeof(wire), &owner));
	ASSERT_EQ(0, validate_action("{\"action\":\"osd-remove\",\"name\":\"uptime\"}", wire,
				     sizeof(wire), &owner));

	/* Not a name: nothing, a section header, a path, a shell word, a
	 * value with a comment in it, a name rod would not hold. */
	ASSERT_ACTION_REFUSED("{\"action\":\"osd-add\"}");
	ASSERT_ACTION_REFUSED("{\"action\":\"osd-add\",\"name\":\"\"}");
	ASSERT_ACTION_REFUSED("{\"action\":\"osd-add\",\"name\":\"one two\"}");
	ASSERT_ACTION_REFUSED("{\"action\":\"osd-add\",\"name\":\"a]\\n[osd.b\"}");
	ASSERT_ACTION_REFUSED("{\"action\":\"osd-add\",\"name\":\"../etc/passwd\"}");
	ASSERT_ACTION_REFUSED("{\"action\":\"osd-add\",\"name\":\"a;reboot\"}");
	ASSERT_ACTION_REFUSED("{\"action\":\"osd-add\",\"name\":\"a = b # c\"}");
	ASSERT_ACTION_REFUSED("{\"action\":\"osd-add\",\"name\":\"osd.nested\"}");
	ASSERT_ACTION_REFUSED("{\"action\":\"osd-add\",\"name\":"
			      "\"thisnameislongerthanrodwillholdinanelement\"}");
	ASSERT_ACTION_REFUSED("{\"action\":\"osd-add\",\"name\":42}");

	/*
	 * And a new one is not a number, which is what the ordinals spelled
	 * their sections and would go on reading as one of them -- [osd.4] is
	 * in a camera's config right now, made by a page that had no other
	 * name to give it.
	 */
	ASSERT_ACTION_REFUSED("{\"action\":\"osd-add\",\"name\":\"4\"}");
	ASSERT_ACTION_REFUSED("{\"action\":\"osd-add\",\"name\":\"007\"}");

	/*
	 * Which is exactly the name that has to be removable. A camera set up
	 * by hand, or by a page that had no better name to give it, holds
	 * names this would not hand out -- and those elements are drawn.
	 * Being unable to take one away because of how it is spelled would be
	 * the worse rule, and the camera this was written on has an [osd.4].
	 */
	ASSERT_EQm(reason, 0,
		   validate_action("{\"action\":\"osd-remove\",\"name\":\"4\"}", wire, sizeof(wire),
				   &owner));
	ASSERTm("the name did not reach the daemon's request", strstr(wire, "\"4\"") != NULL);

	PASS();
}

/* Both persist. What is drawn is a setting and not an override, and an
 * element that vanished at the next reboot would be a worse surprise than
 * one that stayed. */
TEST making_an_element_is_remembered(void)
{
	const rcd_action_t *add = rcd_action_find("osd-add");
	const rcd_action_t *rm = rcd_action_find("osd-remove");

	ASSERT(add && rm);
	ASSERT(add->persists);
	ASSERT(rm->persists);
	ASSERT_STR_EQ("rod", add->daemon);
	ASSERT_STR_EQ("rod", rm->daemon);
	PASS();
}

TEST refuses_unlisted_actions(void)
{
	ASSERT_ACTION_REFUSED("{\"action\":\"set-brightness\",\"value\":10}");
	ASSERT_ACTION_REFUSED("{\"action\":\"poweroff\"}");
	ASSERT_ACTION_REFUSED("{\"action\":\"\"}");
	ASSERT_ACTION_REFUSED("{}");
	PASS();
}

/*
 * Restarting is rcd's own, and priced as what it is.
 *
 * The one action in the table that interrupts. It reaches no daemon -- there
 * is nothing to route a reboot to -- so it must carry a handler, and it must
 * carry an impact, because the tier every daemon action sits in by
 * construction is exactly the wrong answer for this one. A reboot reported as
 * costing nothing is a button a client would offer without a second thought.
 */
TEST restarting_is_rcds_own_and_says_what_it_costs(void)
{
	const rcd_action_t *a = rcd_action_find("reboot");
	ASSERT(a);
	ASSERT(a->local);
	ASSERT_EQ(NULL, a->daemon);
	ASSERT_EQ(RCD_IMPACT_REBOOT, a->impact);
	ASSERT_EQ(A_END, a->args[0].type); /* nothing to get wrong */
	ASSERT(a->note && a->note[0]);	   /* and it says so */

	/* Powering off is not the same favour: a camera that goes down on a
	 * command has no command that brings it back. */
	ASSERT_EQ(NULL, rcd_action_find("poweroff"));
	ASSERT_EQ(NULL, rcd_action_find("shutdown"));
	PASS();
}

TEST refuses_near_misses(void)
{
	ASSERT_ACTION_REFUSED("{\"action\":\"ircut-mode \"}");
	ASSERT_ACTION_REFUSED("{\"action\":\"IRCUT-MODE\",\"value\":\"day\"}");
	ASSERT_ACTION_REFUSED("{\"action\":\"osd-enable2\"}");
	PASS();
}

/* ------------------------------------------------------------------ */
/* Provisioning                                                        */
/* ------------------------------------------------------------------ */

/*
 * The one action rcd carries out itself, and the only one that is routed
 * nowhere: no daemon owns the boot environment, so there is no socket to
 * forward to and the validator has to say so rather than fall through to the
 * section-routing arm and refuse a perfectly good request as unroutable.
 */
TEST forgetting_the_network_is_rcds_own_action(void)
{
	char wire[RCD_REQ_MAX];
	const char *owner = "not a daemon";

	ASSERT_EQ(0,
		  validate_action("{\"action\":\"provision-reset\"}", wire, sizeof(wire), &owner));
	ASSERT(owner == NULL);
	PASS();
}

/* Deny by default did not stop applying to it. */
TEST forgetting_the_network_has_no_near_misses(void)
{
	ASSERT_ACTION_REFUSED("{\"action\":\"provision-reset \"}");
	ASSERT_ACTION_REFUSED("{\"action\":\"provision_reset\"}");
	ASSERT_ACTION_REFUSED("{\"action\":\"provision\"}");
	ASSERT_ACTION_REFUSED("{\"action\":\"PROVISION-RESET\"}");
	PASS();
}

/*
 * What a client is told before it offers the button. All three parts matter
 * and none of them can be inferred: the owner, because this one is not a
 * daemon; the impact, because every other action here is the live tier and
 * this one is not; and the note, because "it takes effect at the next boot"
 * is the whole difference between a command and a surprise.
 */
TEST the_schema_says_what_forgetting_the_network_costs(void)
{
	cJSON *out = cJSON_CreateObject();
	rcd_schema_emit(out, NULL);

	const cJSON *a = NULL, *found = NULL;
	cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(out, "actions"))
	{
		const cJSON *n = cJSON_GetObjectItemCaseSensitive(a, "name");
		if (cJSON_IsString(n) && strcmp(n->valuestring, "provision-reset") == 0)
			found = a;
	}
	ASSERT(found);

	const cJSON *owner = cJSON_GetObjectItemCaseSensitive(found, "owner");
	ASSERT(cJSON_IsString(owner));
	ASSERT_STR_EQ("rcd", owner->valuestring);

	const cJSON *impact = cJSON_GetObjectItemCaseSensitive(found, "impact");
	ASSERT(cJSON_IsString(impact));
	ASSERT_STR_EQ("reboot", impact->valuestring);

	ASSERT(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(found, "note")));

	/*
	 * And it is offered only where there is a radio, marked the way an
	 * unusable key is. Stated against the same question the emit asks so
	 * the test holds on a host with no camera under it and on a camera.
	 */
	const cJSON *av = cJSON_GetObjectItemCaseSensitive(found, "available");
	ASSERT_EQ(rcd_wifi_present(), !cJSON_IsFalse(av));

	cJSON_Delete(out);
	PASS();
}

/*
 * Every action reaches something. An entry that names no daemon, carries no
 * handler and takes no section argument is one the validator can only refuse
 * -- which is a table entry that exists solely to produce an error, and the
 * kind of thing a new action gets wrong once.
 */
TEST every_action_can_be_routed(void)
{
	cJSON *out = cJSON_CreateObject();
	rcd_schema_emit(out, NULL);

	const cJSON *a = NULL;
	int seen = 0;
	cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(out, "actions"))
	{
		const cJSON *name = cJSON_GetObjectItemCaseSensitive(a, "name");
		bool owned = cJSON_IsString(cJSON_GetObjectItemCaseSensitive(a, "owner"));
		bool by_section = false;

		const cJSON *arg = NULL;
		cJSON_ArrayForEach(arg, cJSON_GetObjectItemCaseSensitive(a, "args"))
		{
			const cJSON *t = cJSON_GetObjectItemCaseSensitive(arg, "type");
			if (cJSON_IsString(t) && strcmp(t->valuestring, "section") == 0)
				by_section = true;
		}

		ASSERTm(cJSON_IsString(name) ? name->valuestring : "?", owned || by_section);
		seen++;
	}
	ASSERT(seen > 0);
	cJSON_Delete(out);
	PASS();
}

/*
 * A scan answers with something, which is what the local handler's payload
 * argument exists for -- and it costs nothing, which is what keeps it
 * reachable while a credential is still waiting to be confirmed. That second
 * property is the one the portal depends on: the page that most wants to
 * rescan is the page whose last attempt is mid-guard.
 */
TEST a_scan_is_a_read_and_is_priced_as_one(void)
{
	cJSON *out = cJSON_CreateObject();
	rcd_schema_emit(out, NULL);

	const cJSON *a = NULL, *found = NULL;
	cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(out, "actions"))
	{
		const cJSON *n = cJSON_GetObjectItemCaseSensitive(a, "name");
		if (cJSON_IsString(n) && strcmp(n->valuestring, "wifi-scan") == 0)
			found = a;
	}
	ASSERT(found);

	const cJSON *owner = cJSON_GetObjectItemCaseSensitive(found, "owner");
	ASSERT(cJSON_IsString(owner));
	ASSERT_STR_EQ("rcd", owner->valuestring);

	const cJSON *impact = cJSON_GetObjectItemCaseSensitive(found, "impact");
	ASSERT(cJSON_IsString(impact));
	ASSERT_STR_EQ("none", impact->valuestring);

	/* Nothing takes effect later, so there is nothing to warn about. */
	ASSERT(!cJSON_GetObjectItemCaseSensitive(found, "note"));

	const cJSON *av = cJSON_GetObjectItemCaseSensitive(found, "available");
	ASSERT_EQ(rcd_wifi_present(), !cJSON_IsFalse(av));

	cJSON_Delete(out);
	PASS();
}

/* Routed nowhere, like the other one rcd performs itself. */
TEST a_scan_is_rcds_own_action(void)
{
	char wire[RCD_REQ_MAX];
	const char *owner = "not a daemon";

	ASSERT_EQ(0, validate_action("{\"action\":\"wifi-scan\"}", wire, sizeof(wire), &owner));
	ASSERT(owner == NULL);

	ASSERT_ACTION_REFUSED("{\"action\":\"wifi_scan\"}");
	ASSERT_ACTION_REFUSED("{\"action\":\"scan\"}");
	PASS();
}

/*
 * The fact behind setup mode, and it is always there. Every other object in
 * `state` describes a daemon that may not be running; this one describes the
 * camera, so a client that finds it missing has learned something about the
 * firmware rather than about the network.
 */
TEST state_always_carries_the_provisioning_fact(void)
{
	rcd_state_t st = {0};

	cJSON *resp = rcd_cmd_state(&st, NULL);
	ASSERT(resp);

	const cJSON *sys = cJSON_GetObjectItemCaseSensitive(resp, "system");
	ASSERT(cJSON_IsObject(sys));

	const cJSON *p = cJSON_GetObjectItemCaseSensitive(sys, "provisioned");
	ASSERT(cJSON_IsBool(p));

	/* A camera with no radio has nothing to provision, and saying so is
	 * what keeps a caller from looking for an access point to raise. */
	if (!rcd_wifi_present())
		ASSERT(cJSON_IsTrue(p));

	cJSON_Delete(resp);
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
 * The field a live command expects is the daemon's, not this table's. Almost
 * every one calls it `value` and rc_mode does not: rvd's set-rc-mode carries a
 * bitrate as well, so its mode argument is named. Sending `value` there is
 * refused by rvd with "need channel and mode" -- an error on a key the schema
 * still advertises as live, which is the failure this pins down.
 */
TEST a_live_command_is_sent_the_field_it_asks_for(void)
{
	const rcd_key_t *k = rcd_key_find("stream0", "rc_mode");
	ASSERT(k);
	ASSERT_STR_EQ("set-rc-mode", k->live_cmd);
	ASSERT_STR_EQ("mode", k->live_arg);
	ASSERT_EQ(0, k->live_chn);
	ASSERT_EQ(1, rcd_key_find("stream1", "rc_mode")->live_chn);

	/* And the ordinary case still spells it `value`. */
	ASSERT_STR_EQ("value", rcd_key_find("stream0", "bitrate")->live_arg);
	PASS();
}

/*
 * A key whose live command answers for a family of settings names which one it
 * is, and the name is the key's own. Nothing downstream can catch a selector
 * that says something else: the command is well formed, ric accepts it, and
 * the value lands on a different threshold than the one that was moved.
 *
 * Walked over the whole table rather than asserted on the three that have one
 * today, because the trap is a copy-paste in a fourth.
 */
TEST a_selector_names_the_key_that_carries_it(void)
{
	int seen = 0;
	for (int i = 0;; i++) {
		const rcd_key_t *k = rcd_key_at(i);
		if (!k)
			break;
		if (!k->live_sel)
			continue;
		ASSERT(k->live_cmd);
		ASSERT_STR_EQ(k->key, k->live_sel);
		seen++;
	}
	ASSERT(seen > 0);

	/* And ric is the daemon that takes them, by the section they sit in. */
	const rcd_key_t *nl = rcd_key_find("ircut", "night_luma");
	ASSERT(nl);
	ASSERT_STR_EQ("set-threshold", nl->live_cmd);
	ASSERT_STR_EQ("value", nl->live_arg);
	ASSERT_EQ(-1, nl->live_chn);
	ASSERT_EQ(RCD_D_RIC, rcd_section_owner(nl->section));

	/* A key without one sends no selector at all. */
	ASSERT_EQ(NULL, rcd_key_find("image", "brightness")->live_sel);
	PASS();
}

/*
 * Every threshold `ircut-threshold` offers is also a key, and a live one --
 * the action and the key reach the same handler, so a threshold reachable
 * through one and not the other is a control that silently needs an apply.
 *
 * Driven off the action's own choice list rather than a second copy of it, so
 * a threshold added to ric and wired to the action cannot arrive without one.
 */
TEST every_daynight_threshold_is_a_live_key(void)
{
	const rcd_action_t *a = rcd_action_find("ircut-threshold");
	ASSERT(a);

	const char *const *names = NULL;
	for (int i = 0; a->args[i].type != A_END; i++) {
		if (strcmp(a->args[i].key, "key") == 0)
			names = a->args[i].choices;
	}
	ASSERT(names);

	for (int i = 0; names[i]; i++) {
		const rcd_key_t *k = rcd_key_find("ircut", names[i]);
		ASSERT(k);
		ASSERT_EQ(V_INT, k->type);
		ASSERT_STR_EQ("set-threshold", k->live_cmd);
		ASSERT_STR_EQ(names[i], k->live_sel);
	}

	/* The ranges are ric's, and a value outside one never reaches it. */
	ASSERT_EQ(255, rcd_key_find("ircut", "night_luma")->max);
	ASSERT_EQ(1, rcd_key_find("ircut", "day_gain_pct")->min);
	ASSERT_EQ(100, rcd_key_find("ircut", "day_gain_pct")->max);
	ASSERT_EQ(300, rcd_key_find("ircut", "hysteresis_sec")->max);
	ASSERT_EQ(50, rcd_key_find("ircut", "poll_interval_ms")->min);
	PASS();
}

/*
 * An element has the same four keys whatever this camera called it.
 *
 * The table cannot name a section a person has not written yet, so it names
 * the shape instead -- one `osd.*` row set, reached through every name an
 * element might have. These four are made up on the spot for that reason: if
 * the pattern only answered for names somebody had thought of, it would be
 * the ordinals again under another spelling.
 */
TEST an_element_carries_the_same_keys_whatever_it_is_called(void)
{
	static const char *const slots[] = {"osd.timestamp", "osd.a-name_2", "osd.4", "osd.x",
					    NULL};
	static const char *const expect[] = {"template", "position", "align", "visible", NULL};

	for (int p = 0; slots[p]; p++) {
		int n = 0;
		for (int i = 0;; i++) {
			const rcd_key_t *k = rcd_key_at(i);
			if (!k)
				break;
			if (!rcd_section_is(k->section, slots[p]))
				continue;
			n++;

			const rcd_key_t *first = rcd_key_find(slots[0], k->key);
			ASSERT(first);
			ASSERT_EQ(first->type, k->type);
			ASSERT_EQ(first->min, k->min);
			ASSERT_EQ(first->max, k->max);
		}
		ASSERT_EQ(4, n);

		for (int e = 0; expect[e]; e++)
			ASSERT(rcd_key_find(slots[p], expect[e]));

		/*
		 * `position` is a value now rather than the section name. The
		 * places it may take are rod's own seven and not a coordinate:
		 * rod reads `x,y` too, and it is not on offer here.
		 */
		const rcd_key_t *pos = rcd_key_find(slots[p], "position");
		int places = 0;

		ASSERT_EQ(V_ENUM, pos->type);
		for (int c = 0; pos->choices[c]; c++)
			places++;
		ASSERT_EQ(7, places);
		ASSERT_STR_EQ("top_left", pos->choices[0]);
		ASSERT_STR_EQ("center", pos->choices[6]);

		/* rod owns them, and answers for them when asked. */
		ASSERT_EQ(RCD_D_ROD, rcd_section_owner(slots[p]));
		ASSERT_STR_EQ("rod", rcd_section_reader(slots[p]));
	}

	/* And the overlay's own section is not one of them: [osd] holds the
	 * type every element is drawn in, and matching it here would put a
	 * template and a place on the whole overlay. */
	ASSERT_FALSE(rcd_section_is("osd.*", "osd"));
	ASSERT_EQ(NULL, rcd_key_find("osd", "template"));
	ASSERT(rcd_key_find("osd", "font_size"));
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
	/*
	 * The parent first, because mkdir(2) makes one level and the path has
	 * two -- and the suite mounts a fresh tmpfs on /run, so the parent is
	 * never there. Without this every test that writes a file skipped, the
	 * run reported itself green, and the skip count was the only sign: 33
	 * tests, including the whole of the shadow suite, silently not run.
	 */
	char parent[256];

	snprintf(parent, sizeof(parent), "%s", RCD_SYSCONF_DIR);

	char *slash = strrchr(parent, '/');

	if (slash && slash != parent) {
		*slash = '\0';
		mkdir(parent, 0755);
	}

	if (mkdir(RCD_SYSCONF_DIR, 0755) == 0)
		return 1;
	return access(RCD_SYSCONF_DIR, W_OK) == 0;
}

/*
 * A config file with the given [osd.*] sections, in the given order, loaded
 * the way rcd loads one. Written to the scratch directory the suite already
 * mounts, so the ordering under test is a file's own and not a fixture's.
 */
static rss_config_t *osd_config(const char *const *sections)
{
	char path[300];
	FILE *f;

	snprintf(path, sizeof(path), "%s/osd-order.conf", RCD_SYSCONF_DIR);
	f = fopen(path, "w");
	if (!f)
		return NULL;
	fprintf(f, "[osd]\nfont_size = 24\n\n");
	for (int i = 0; sections[i]; i++)
		fprintf(f, "[%s]\ntemplate = e%d\n\n", sections[i], i);
	fclose(f);

	return rss_config_load(path);
}

/* The elements a config has, as one string, so a test reads as the question. */
static const char *listed(rss_config_t *cfg, char *buf, size_t bufsz)
{
	char named[RCD_OSD_MAX][RCD_SECT_MAX];
	int n = rcd_osd_elements(cfg, named, RCD_OSD_MAX);

	buf[0] = '\0';
	for (int i = 0; i < n; i++)
		snprintf(buf + strlen(buf), bufsz - strlen(buf), "%s%s", i ? " " : "", named[i]);
	return buf;
}

/*
 * The list is the file's, in the file's own order.
 *
 * rss_config prepends each section as it parses, so a walk of the store hands
 * them back backwards. A list that trusted the walk would show every camera's
 * elements in reverse -- harmless-looking until two of them want one place,
 * which rod settles by the order the file lists them in.
 */
TEST the_elements_are_listed_the_way_the_file_reads(void)
{
	if (!sysconf_dir_ready())
		SKIP();

	static const char *const five[] = {"osd.first",	 "osd.second", "osd.third",
					   "osd.fourth", "osd.fifth",  NULL};
	rss_config_t *cfg = osd_config(five);
	char buf[256];

	ASSERT(cfg);
	ASSERT_STR_EQ("osd.first osd.second osd.third osd.fourth osd.fifth",
		      listed(cfg, buf, sizeof(buf)));

	rss_config_free(cfg);
	PASS();
}

/*
 * A section rod would not draw is not an element. rod ignores one with an
 * empty name and one whose name it cannot hold, so listing them would offer
 * an element that nothing draws and no edit reaches -- and [osd] itself is
 * the overlay's own settings rather than an element at all.
 */
TEST a_section_rod_would_not_draw_is_not_an_element(void)
{
	if (!sysconf_dir_ready())
		SKIP();

	static const char *const mixed[] = {
		"osd.", "osd.thisnameislongerthanrodwillholdinanelement", "osd.real", NULL};
	rss_config_t *cfg = osd_config(mixed);
	char buf[256];

	ASSERT(cfg);
	ASSERT_STR_EQ("osd.real", listed(cfg, buf, sizeof(buf)));

	rss_config_free(cfg);
	PASS();
}

/* An overlay with nothing in it is a camera, not an error. */
TEST a_config_with_no_elements_lists_none(void)
{
	if (!sysconf_dir_ready())
		SKIP();

	static const char *const none[] = {NULL};
	rss_config_t *cfg = osd_config(none);
	char buf[256];

	ASSERT(cfg);
	ASSERT_STR_EQ("", listed(cfg, buf, sizeof(buf)));

	rss_config_free(cfg);
	PASS();
}

/*
 * The pattern is how a client asks which elements a camera has: one reply,
 * every element, each value under the name its own section carries. The
 * shape of an element is the schema and does not vary; which of them exist
 * is this, and it is the config file like any other question about it.
 */
TEST the_pattern_answers_with_every_element_by_name(void)
{
	rcd_state_t st;
	char path[256];
	FILE *f;

	if (!sysconf_dir_ready())
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	snprintf(path, sizeof(path), "%s/raptor.conf", RCD_SYSCONF_DIR);
	f = fopen(path, "w");
	ASSERT(f);
	fputs("[osd.timestamp]\ntemplate = %time%\nposition = top_left\n\n"
	      "[osd.uptime]\ntemplate = %uptime%\nposition = top_right\n",
	      f);
	fclose(f);

	memset(&st, 0, sizeof(st));
	st.config_path = path;

	cJSON *req = cJSON_Parse("{\"section\":\"osd.*\"}");
	ASSERT(req);
	cJSON *r = rcd_cmd_get(&st, req);
	cJSON_Delete(req);
	ASSERT(r);

	const cJSON *vals = cJSON_GetObjectItemCaseSensitive(r, "values");
	int stamp = 0, up = 0, pattern = 0;
	const cJSON *v = NULL;

	cJSON_ArrayForEach(v, vals)
	{
		const char *sect =
			cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(v, "section"));

		if (!sect)
			continue;
		if (strcmp(sect, "osd.timestamp") == 0)
			stamp++;
		else if (strcmp(sect, "osd.uptime") == 0)
			up++;
		else if (strcmp(sect, "osd.*") == 0)
			pattern++;
	}

	ASSERT_EQm("an element was not answered for with all four of its keys", 4, stamp);
	ASSERT_EQ(4, up);
	ASSERTm("the pattern answered under its own name instead of the elements'", pattern == 0);

	cJSON_Delete(r);
	unlink(path);
	PASS();
}

/* And with none of them, which is a camera with no overlay rather than a
 * client asking about a section that does not exist. */
TEST the_pattern_with_no_elements_is_not_an_error(void)
{
	rcd_state_t st;
	char path[256];
	FILE *f;

	if (!sysconf_dir_ready())
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	snprintf(path, sizeof(path), "%s/raptor.conf", RCD_SYSCONF_DIR);
	f = fopen(path, "w");
	ASSERT(f);
	fputs("[osd]\nfont_size = 24\n", f);
	fclose(f);

	memset(&st, 0, sizeof(st));
	st.config_path = path;

	cJSON *req = cJSON_Parse("{\"section\":\"osd.*\"}");
	ASSERT(req);
	cJSON *r = rcd_cmd_get(&st, req);
	cJSON_Delete(req);
	ASSERT(r);

	ASSERT_STR_EQ("ok", cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r, "status")));
	ASSERT_EQ(0, cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(r, "values")));

	cJSON_Delete(r);
	unlink(path);
	PASS();
}

/*
 * A key is written into an element that exists, and nothing else makes one.
 *
 * A section is created by being written to, so before this a form's untouched
 * fields were enough: the overlay page had a value in every field of every
 * element whether or not anybody typed one, and applying it wrote a
 * `visible = false` into a section that did not exist. That created an
 * element -- one with nothing in it, at the default place, taking that place
 * from the element already drawn there. The camera it was found on lost its
 * clock.
 */
TEST a_key_for_an_element_that_is_not_there_makes_no_element(void)
{
	rcd_state_t st;
	char path[256], text[1024] = "";
	FILE *f;

	if (!sysconf_dir_ready())
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	snprintf(path, sizeof(path), "%s/raptor.conf", RCD_SYSCONF_DIR);
	f = fopen(path, "w");
	ASSERT(f);
	fputs("[osd.timestamp]\ntype = text\ntemplate = %time%\nposition = top_left\n", f);
	fclose(f);

	memset(&st, 0, sizeof(st));
	st.config_path = path;

	cJSON *req =
		cJSON_Parse("{\"edits\":["
			    "{\"section\":\"osd.spare\",\"key\":\"visible\",\"value\":false},"
			    "{\"section\":\"osd.spare\",\"key\":\"template\",\"value\":\"x\"}]}");
	ASSERT(req);
	cJSON *resp = rcd_cmd_set(&st, req);
	cJSON_Delete(req);
	ASSERT(resp);

	const cJSON *one = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(resp, "results"), 0);
	const cJSON *said = cJSON_GetObjectItemCaseSensitive(one, "note");
	bool said_nothing_landed = cJSON_IsString(said) && strstr(said->valuestring, "no element");

	cJSON_Delete(resp);

	f = fopen(path, "r");
	ASSERT(f);
	fread(text, 1, sizeof(text) - 1, f);
	fclose(f);

	/* Neither key made it, the template included: an element is made by
	 * asking for one, and text is not the asking. */
	ASSERTm("a key created the element it was addressed to",
		strstr(text, "[osd.spare]") == NULL);
	ASSERT(strstr(text, "[osd.timestamp]") != NULL);
	ASSERTm("the reply did not say the edit had gone nowhere", said_nothing_landed);

	unlink(path);
	PASS();
}

/* The pattern is not a section either: a client that sends it where a name
 * belongs writes nothing, rather than a [osd.*] nothing would ever draw. */
TEST the_pattern_is_not_a_section_to_write_to(void)
{
	rcd_state_t st;
	char path[256], text[1024] = "";
	FILE *f;

	if (!sysconf_dir_ready())
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	snprintf(path, sizeof(path), "%s/raptor.conf", RCD_SYSCONF_DIR);
	f = fopen(path, "w");
	ASSERT(f);
	fputs("[osd.timestamp]\ntemplate = %time%\n", f);
	fclose(f);

	memset(&st, 0, sizeof(st));
	st.config_path = path;

	cJSON *req = cJSON_Parse(
		"{\"edits\":[{\"section\":\"osd.*\",\"key\":\"template\",\"value\":\"x\"}]}");
	ASSERT(req);
	cJSON *resp = rcd_cmd_set(&st, req);
	cJSON_Delete(req);
	ASSERT(resp);
	cJSON_Delete(resp);

	f = fopen(path, "r");
	ASSERT(f);
	fread(text, 1, sizeof(text) - 1, f);
	fclose(f);

	ASSERTm("the pattern was written to the file as a section",
		strstr(text, "[osd.*]") == NULL);

	unlink(path);
	PASS();
}

/*
 * Editing an element the table never heard of. This is the whole of what the
 * ordinals were working around: a camera set up by hand with [osd.timestamp]
 * and [osd.uptime] had two elements no client could name.
 */
TEST an_element_the_camera_named_is_edited_under_that_name(void)
{
	rcd_state_t st;
	char path[256], text[1024] = "";
	FILE *f;

	if (!sysconf_dir_ready())
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	snprintf(path, sizeof(path), "%s/raptor.conf", RCD_SYSCONF_DIR);
	f = fopen(path, "w");
	ASSERT(f);
	fputs("[osd.timestamp]\ntemplate = %time%\nposition = top_left\n", f);
	fclose(f);

	memset(&st, 0, sizeof(st));
	st.config_path = path;

	cJSON *req = cJSON_Parse("{\"edits\":[{\"section\":\"osd.timestamp\","
				 "\"key\":\"position\",\"value\":\"bottom_right\"}]}");
	ASSERT(req);
	cJSON *resp = rcd_cmd_set(&st, req);
	cJSON_Delete(req);
	ASSERT(resp);

	const cJSON *one = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(resp, "results"), 0);
	const char *said = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(one, "section"));

	ASSERTm("the reply answered about a section other than the one asked about",
		said && strcmp(said, "osd.timestamp") == 0);
	cJSON_Delete(resp);

	f = fopen(path, "r");
	ASSERT(f);
	fread(text, 1, sizeof(text) - 1, f);
	fclose(f);

	ASSERT(strstr(text, "bottom_right") != NULL);
	ASSERT(strstr(text, "top_left") == NULL);

	unlink(path);
	PASS();
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

/* ------------------------------------------------------------------ */
/* get: one round trip per section                                     */
/* ------------------------------------------------------------------ */

/*
 * A stand-in for a daemon, which is the only way to count what `get` asks it.
 *
 * The property under test is how many times rcd opens rvd's control socket to
 * answer one request, and nothing in the reply decides that -- so this accepts,
 * counts, answers the shortest valid thing, and closes.
 */
typedef struct {
	int fd;
	int conns;
	pthread_t tid;
	bool stop;
	const char *reply;  /* NULL for the shortest valid thing */
	char last_req[512]; /* what the caller sent, for asserting on */
} fake_daemon_t;

static void *fake_daemon_run(void *arg)
{
	fake_daemon_t *d = arg;
	const char *reply = d->reply ? d->reply : "{\"status\":\"ok\",\"keys\":{}}";

	while (!d->stop) {
		struct pollfd pfd = {.fd = d->fd, .events = POLLIN};

		if (poll(&pfd, 1, 50) <= 0)
			continue;

		int c = accept(d->fd, NULL, NULL);

		if (c < 0)
			continue;
		d->conns++;

		/* Read the request off the wire and answer it: two bytes of
		 * length, then the body, which is what rss_ctrl speaks. */
		uint8_t hdr[2];

		if (read(c, hdr, 2) == 2) {
			size_t want = ((size_t)hdr[0] << 8) | hdr[1];
			char sink[1024];
			size_t kept = 0;

			d->last_req[0] = '\0';
			while (want > 0) {
				ssize_t n =
					read(c, sink, want > sizeof(sink) ? sizeof(sink) : want);

				if (n <= 0)
					break;
				if (kept + (size_t)n < sizeof(d->last_req)) {
					memcpy(d->last_req + kept, sink, (size_t)n);
					kept += (size_t)n;
					d->last_req[kept] = '\0';
				}
				want -= (size_t)n;
			}
		}

		size_t rlen = strlen(reply);
		uint8_t out[2] = {(uint8_t)(rlen >> 8), (uint8_t)(rlen & 0xff)};

		if (write(c, out, 2) == 2)
			(void)!write(c, reply, rlen);
		close(c);
	}
	return NULL;
}

static bool fake_daemon_start_reply(fake_daemon_t *d, const char *name, const char *reply)
{
	char path[128];

	memset(d, 0, sizeof(*d));
	d->reply = reply;
	mkdir(RSS_RUN_DIR, 0755);
	snprintf(path, sizeof(path), RSS_SOCK_FMT, name);
	unlink(path);

	d->fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (d->fd < 0)
		return false;

	struct sockaddr_un a;

	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	rss_strlcpy(a.sun_path, path, sizeof(a.sun_path));

	if (bind(d->fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(d->fd, 5) < 0) {
		close(d->fd);
		return false;
	}
	return pthread_create(&d->tid, NULL, fake_daemon_run, d) == 0;
}

static bool fake_daemon_start(fake_daemon_t *d, const char *name)
{
	return fake_daemon_start_reply(d, name, NULL);
}

static void fake_daemon_stop(fake_daemon_t *d, const char *name)
{
	char path[128];

	d->stop = true;
	pthread_join(d->tid, NULL);
	close(d->fd);
	snprintf(path, sizeof(path), RSS_SOCK_FMT, name);
	unlink(path);
}

/* A `get` naming `n` copies of one key. */
static cJSON *get_naming(const char *section, const char *key, int n)
{
	cJSON *root = cJSON_CreateObject();
	cJSON *arr = cJSON_AddArrayToObject(root, "keys");

	for (int i = 0; i < n; i++) {
		cJSON *e = cJSON_CreateObject();

		cJSON_AddStringToObject(e, "section", section);
		cJSON_AddStringToObject(e, "key", key);
		cJSON_AddItemToArray(arr, e);
	}
	return root;
}

/*
 * An ISP knob rvd has no reading for stays absent from `state`.
 *
 * rvd leaves the value out when its getter declines -- Ingenic has no getter
 * for the denoise strengths, and answers nothing for a knob the running
 * process has not written. rcd has no number that can stand in for that: 0 is
 * a position on all thirteen scales, and on Ingenic brightness 0 is the
 * position that blows the picture white. A subscriber that reads a fabricated
 * 0 as a reading and echoes it back is what makes that reachable, so the hole
 * has to survive the hop.
 */
TEST state_leaves_out_an_isp_knob_rvd_could_not_read(void)
{
	fake_daemon_t d;
	rcd_state_t st;
	static const char isp[] = "{\"status\":\"ok\",\"brightness\":96,"
				  "\"caps\":{\"brightness\":{\"min\":1,\"max\":255,"
				  "\"neutral\":128},\"temper\":{\"min\":0,\"max\":255,"
				  "\"neutral\":128}},\"settable\":\",brightness,temper,\"}";

	memset(&st, 0, sizeof(st));
	if (!fake_daemon_start_reply(&d, "rvd", isp))
		SKIPm("cannot listen on " RSS_RUN_DIR " -- run the suite under unshare -rm");

	/* `state` asks the pidfile which sockets are worth a round trip, so the
	 * fake has to look alive as well as answer. Our own pid is a live one. */
	char pidpath[128];
	FILE *pf;

	snprintf(pidpath, sizeof(pidpath), "%s/rvd.pid", RSS_RUN_DIR);
	pf = fopen(pidpath, "w");
	if (pf) {
		fprintf(pf, "%d\n", (int)getpid());
		fclose(pf);
	}

	cJSON *r = rcd_cmd_state(&st, NULL);

	fake_daemon_stop(&d, "rvd");
	unlink(pidpath);
	ASSERT(r != NULL);

	const cJSON *image = cJSON_GetObjectItemCaseSensitive(r, "image");

	ASSERT(image != NULL);

	const cJSON *bright = cJSON_GetObjectItemCaseSensitive(image, "brightness");

	ASSERTm("a knob rvd read is carried across", cJSON_IsNumber(bright));
	ASSERT_EQ(96, bright->valueint);

	/* And the one it did not read is absent rather than zero. Asserted as
	 * absence and not as a value, because any value here is the bug. */
	ASSERTm("a knob rvd could not read must not appear at all",
		cJSON_GetObjectItemCaseSensitive(image, "temper") == NULL);
	ASSERTm("nor may any other unread knob",
		cJSON_GetObjectItemCaseSensitive(image, "contrast") == NULL);

	/* The caps and the settable list are not readings and still travel:
	 * a client has to draw the control whether or not it has a value. */
	ASSERT(cJSON_GetObjectItemCaseSensitive(image, "caps") != NULL);
	ASSERT(cJSON_GetObjectItemCaseSensitive(image, "settable") != NULL);

	cJSON_Delete(r);
	PASS();
}

/*
 * A daemon that parses a key itself resolves it with "" for a default and
 * applies its own afterwards -- rod reads its font size that way, and its
 * config-get-section then carries font_size as the empty string. Read as a
 * number that is 0: below the key's floor, and shown as though somebody chose
 * it. Empty is no value. The key is unset, as the file says it is.
 */
TEST an_empty_value_from_a_daemon_is_no_value(void)
{
	fake_daemon_t d;
	rcd_state_t st;
	char path[256];
	FILE *f;

	if (!sysconf_dir_ready())
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	snprintf(path, sizeof(path), "%s/raptor.conf", RCD_SYSCONF_DIR);
	f = fopen(path, "w");
	ASSERT(f);
	fputs("[osd]\nfont_stroke = 2\n", f);
	fclose(f);

	memset(&st, 0, sizeof(st));
	st.config_path = path;
	if (!fake_daemon_start_reply(&d, "rod",
				     "{\"status\":\"ok\",\"keys\":{\"font_size\":\"\","
				     "\"font_stroke\":\"2\"}}"))
		SKIPm("cannot listen on " RSS_RUN_DIR " -- run the suite under unshare -rm");

	cJSON *req = cJSON_Parse("{\"section\":\"osd\"}");
	ASSERT(req);
	cJSON *r = rcd_cmd_get(&st, req);
	cJSON_Delete(req);
	fake_daemon_stop(&d, "rod");
	ASSERT(r);

	const cJSON *vals = cJSON_GetObjectItemCaseSensitive(r, "values");
	const cJSON *size = NULL, *stroke = NULL, *v = NULL;

	cJSON_ArrayForEach(v, vals)
	{
		const char *key = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(v, "key"));

		if (key && strcmp(key, "font_size") == 0)
			size = v;
		else if (key && strcmp(key, "font_stroke") == 0)
			stroke = v;
	}
	ASSERT(size && stroke);
	ASSERTm("an empty string from the daemon was served as a value",
		cJSON_GetObjectItemCaseSensitive(size, "value") == NULL);
	ASSERTm("a key the daemon resolved with nothing is unset",
		cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(size, "set")));
	/* And one it resolved with something is still the daemon's answer. */
	const cJSON *sv = cJSON_GetObjectItemCaseSensitive(stroke, "value");

	ASSERT(cJSON_IsNumber(sv));
	ASSERT_EQ(2, sv->valueint);

	cJSON_Delete(r);
	PASS();
}

/*
 * The comment over section_from_daemon has always said one round trip per
 * section rather than per key. It was true of a request naming a section and
 * false of one naming keys, which asked the daemon again for every key -- and
 * a healthy daemon answers in a fraction of a millisecond, so nothing showed
 * it. A daemon that has stopped answering is where the difference lands, at a
 * control-socket timeout each.
 */
TEST get_asks_a_daemon_once_however_many_keys_name_its_section(void)
{
	fake_daemon_t d;
	rcd_state_t st;

	memset(&st, 0, sizeof(st));
	if (!fake_daemon_start(&d, "rvd"))
		SKIPm("cannot listen on " RSS_RUN_DIR " -- run the suite under unshare -rm");

	cJSON *req = get_naming("image", "brightness", 20);
	cJSON *r = rcd_cmd_get(&st, req);

	cJSON_Delete(req);
	ASSERT(r != NULL);
	cJSON_Delete(r);

	int conns = d.conns;

	fake_daemon_stop(&d, "rvd");
	ASSERT_EQm("rvd was asked once per key", 1, conns);
	PASS();
}

/*
 * Adding an element reaches the file before the next request is answered.
 *
 * A save is debounced, which is right for an action a slider sends tens of
 * and wrong for one whose whole effect is that something now exists: the very
 * next request is the form filling the new element in, and rcd refuses a key
 * addressed to an element the file does not have. Found on a camera, where
 * adding an element and typing into it in the same breath wrote neither.
 */
TEST an_element_reaches_the_file_before_the_next_request(void)
{
	fake_daemon_t d;
	rcd_state_t st;

	memset(&st, 0, sizeof(st));
	if (!fake_daemon_start(&d, "rod"))
		SKIPm("cannot listen on " RSS_RUN_DIR " -- run the suite under unshare -rm");

	cJSON *req = cJSON_Parse("{\"action\":\"osd-add\",\"name\":\"logo\"}");
	cJSON *r = rcd_cmd_action(&st, req);

	cJSON_Delete(req);
	ASSERT(r != NULL);
	ASSERT_STR_EQ("ok", cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r, "status")));
	cJSON_Delete(r);

	int conns = d.conns;
	char last[512];

	rss_strlcpy(last, d.last_req, sizeof(last));
	fake_daemon_stop(&d, "rod");

	ASSERT_EQm("adding an element was one round trip, so nothing wrote the file", 2, conns);
	ASSERTm("rod was not asked to write the file", strstr(last, "config-save") != NULL);
	ASSERT_EQm("a save was still owed when the call returned", 0, (int)st.save_due_ms);
	PASS();
}

TEST get_asks_once_per_distinct_section(void)
{
	fake_daemon_t d;
	rcd_state_t st;

	memset(&st, 0, sizeof(st));
	if (!fake_daemon_start(&d, "rvd"))
		SKIPm("cannot listen on " RSS_RUN_DIR " -- run the suite under unshare -rm");

	/* Two sections, both read by rvd, named five times each. Caching by
	 * section rather than by daemon is deliberate: the request rcd sends
	 * names the section, so two sections are two questions. */
	cJSON *req = cJSON_CreateObject();
	cJSON *arr = cJSON_AddArrayToObject(req, "keys");

	for (int i = 0; i < 5; i++) {
		const char *secs[] = {"image", "jpeg"};
		const char *keys[] = {"brightness", "quality"};

		for (int j = 0; j < 2; j++) {
			cJSON *e = cJSON_CreateObject();

			cJSON_AddStringToObject(e, "section", secs[j]);
			cJSON_AddStringToObject(e, "key", keys[j]);
			cJSON_AddItemToArray(arr, e);
		}
	}

	cJSON *r = rcd_cmd_get(&st, req);

	cJSON_Delete(req);
	ASSERT(r != NULL);
	cJSON_Delete(r);

	int conns = d.conns;

	fake_daemon_stop(&d, "rvd");
	ASSERT_EQm("one round trip per key, not per section", 2, conns);
	PASS();
}

/*
 * And a cap, because the cache bounds the round trips by the number of
 * sections but not the work: what a request asks for is work rcd's single
 * serve loop does before it answers anybody else.
 */
TEST get_refuses_more_keys_than_a_request_may_carry(void)
{
	rcd_state_t st;

	memset(&st, 0, sizeof(st));

	cJSON *req = get_naming("image", "brightness", RCD_GETS_MAX + 1);
	cJSON *r = rcd_cmd_get(&st, req);

	cJSON_Delete(req);
	ASSERT(r != NULL);

	const cJSON *code = cJSON_GetObjectItemCaseSensitive(r, "code");

	ASSERT(cJSON_IsString(code));
	ASSERT_STR_EQ(RCD_E_TOOMANY, code->valuestring);
	cJSON_Delete(r);

	/* And the one below it is answered rather than refused, so the cap is
	 * a limit and not an off-by-one. */
	req = get_naming("image", "brightness", RCD_GETS_MAX);
	r = rcd_cmd_get(&st, req);
	cJSON_Delete(req);
	ASSERT(r != NULL);
	code = cJSON_GetObjectItemCaseSensitive(r, "code");
	ASSERT_FALSEm("the cap refused a request at the limit", cJSON_IsString(code));
	cJSON_Delete(r);
	PASS();
}

/*
 * The cache is sized past the table rather than checked at runtime, so this is
 * what makes that safe. A section added to the table without a thought for
 * this file fails here rather than quietly losing its live values.
 */
TEST the_live_cache_holds_every_section_the_table_has(void)
{
	const char *seen[RCD_LIVE_MAX * 4];
	int n = 0, repeats = 0;

	for (int i = 0; rcd_key_at(i); i++) {
		const rcd_key_t *k = rcd_key_at(i);
		bool have = false;

		for (int j = 0; j < n; j++)
			have = have || strcmp(seen[j], k->section) == 0;
		if (have)
			continue;
		ASSERT(n < (int)(sizeof(seen) / sizeof(seen[0])));
		if (rcd_row_repeats(k->section))
			repeats++;
		seen[n++] = k->section;
	}

	ASSERT(n > 0);
	/* A repeat row is one section here and as many as rod draws on a
	 * camera, so what has to fit is the table's count with that row's
	 * worth of elements in place of the row. */
	int worst = n - repeats + repeats * RCD_OSD_MAX;

	ASSERT_EQm("more sections than the get cache can hold", true, worst <= RCD_LIVE_MAX);
	PASS();
}

/* ------------------------------------------------------------------ */
/* Assumptions about the image underneath                              */
/* ------------------------------------------------------------------ */

/*
 * The codec numbers arrive on rvd's wire and originate in the HAL. rcd used to
 * spell them 0, 1, 2, 3 next to a comment saying they mirrored rss_codec_t --
 * a copy, and a copy can drift. Reordering the HAL's enum would have
 * relabelled every stream on the console with nothing failing to build.
 *
 * They are the enum now, so this test cannot fail by drift. It asserts the
 * values themselves instead, because they are a wire format: rvd on an older
 * build is still sending the old numbers, and renumbering the enum is a
 * protocol change whoever does it should be made to notice.
 */
TEST the_codec_numbers_are_a_wire_format(void)
{
	ASSERT_EQ(0, RSS_CODEC_H264);
	ASSERT_EQ(1, RSS_CODEC_H265);
	ASSERT_EQ(2, RSS_CODEC_JPEG);
	ASSERT_EQ(3, RSS_CODEC_MJPEG);
	PASS();
}

/*
 * An init script is found by name whatever number it carries.
 *
 * The number is boot order and moves when a package is added ahead of one; the
 * name is the convention. Spelling both meant an image that renumbered ntpd
 * wrote the file, reloaded nothing, and reported the change as in force.
 */
static bool initd_ready(void)
{
	char dir[192];

	if (!sysconf_dir_ready())
		return false;
	snprintf(dir, sizeof(dir), "%s/init.d", RCD_SYSCONF_DIR);
	mkdir(dir, 0755);
	return access(dir, W_OK) == 0;
}

#define RAN_MARKER RCD_SYSCONF_DIR "/init.d/ran"

/*
 * A script that records having been run. Which script was chosen is the whole
 * question, and a set() that returns 0 either way cannot answer it.
 */
static bool put_script(const char *basename)
{
	char path[256];
	FILE *f;

	snprintf(path, sizeof(path), "%s/init.d/%s", RCD_SYSCONF_DIR, basename);
	f = fopen(path, "w");
	if (!f)
		return false;
	fprintf(f, "#!/bin/sh\necho \"%s $1\" > %s\nexit 0\n", basename, RAN_MARKER);
	fclose(f);
	return chmod(path, 0755) == 0;
}

static void drop_script(const char *basename)
{
	char path[256];

	snprintf(path, sizeof(path), "%s/init.d/%s", RCD_SYSCONF_DIR, basename);
	unlink(path);
}

/* What the marker says, or "" when nothing ran. */
static const char *what_ran(char *buf, size_t sz)
{
	FILE *f = fopen(RAN_MARKER, "r");

	buf[0] = '\0';
	if (!f)
		return buf;
	if (fgets(buf, (int)sz, f))
		buf[strcspn(buf, "\r\n")] = '\0';
	fclose(f);
	return buf;
}

static const char *const initd_decoys[] = {"S49ntpdx", "K49ntpd", "S49ntpd.bak",
					   "ntpdx",    "S49ntp",  NULL};

static void initd_clean(void)
{
	unlink(RAN_MARKER);
	drop_script("S49ntpd");
	drop_script("S43ntpd");
	drop_script("ntpd");
	for (int i = 0; initd_decoys[i]; i++)
		drop_script(initd_decoys[i]);
}

TEST an_init_script_is_found_whatever_number_it_carries(void)
{
	char ran[128];

	if (!initd_ready())
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	initd_clean();

	/* The number this image happens to ship is not the one another does. */
	if (!put_script("S43ntpd"))
		SKIPm("cannot write an init script");

	ASSERT_EQ(0, rcd_provider_ntp_server.set("10.0.0.1"));
	ASSERT_STR_EQm("a renumbered ntpd was not the one that ran", "S43ntpd restart",
		       what_ran(ran, sizeof(ran)));

	/* A sysvinit /etc spells it without the number at all. */
	initd_clean();
	if (!put_script("ntpd"))
		SKIPm("cannot write an init script");
	ASSERT_EQ(0, rcd_provider_ntp_server.set("10.0.0.2"));
	ASSERT_STR_EQ("ntpd restart", what_ran(ran, sizeof(ran)));

	/* And with no script at all the value is still stored, because an
	 * image that does not run ntpd is not a failed request. */
	initd_clean();
	ASSERT_EQ(0, rcd_provider_ntp_server.set("10.0.0.3"));
	ASSERT_STR_EQm("something ran with no ntpd installed", "", what_ran(ran, sizeof(ran)));

	char got[RCD_VAL_MAX] = "";

	ASSERT_EQ(0, rcd_provider_ntp_server.get(got, sizeof(got)));
	ASSERT_STR_EQ("10.0.0.3", got);
	PASS();
}

/*
 * And the match is anchored at both ends, so it cannot pick up a neighbour: a
 * stop link, a backup somebody left behind, or a different service whose name
 * merely starts the same way. Reloading the wrong service is worse than
 * reloading none.
 */
TEST the_init_script_match_is_anchored(void)
{
	char ran[128];

	if (!initd_ready())
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	initd_clean();

	for (int i = 0; initd_decoys[i]; i++) {
		if (!put_script(initd_decoys[i]))
			SKIPm("cannot write init scripts");
	}

	ASSERT_EQ(0, rcd_provider_ntp_server.set("10.0.0.4"));
	ASSERT_STR_EQm("a script that is not ntpd was run", "", what_ran(ran, sizeof(ran)));

	initd_clean();
	PASS();
}

/*
 * A daemon is installed wherever this image puts it.
 *
 * `hello` looked in /usr/bin and nowhere else, and the console draws a control
 * only for a daemon that is installed -- so on an image that installs to
 * /usr/sbin, every daemon read as absent and the console drew almost nothing.
 * This camera's own PATH puts /usr/sbin first, so that layout is not a
 * hypothetical one.
 */
TEST a_daemon_is_found_wherever_the_image_puts_it(void)
{
	char dir[192], bin[256];

	if (!sysconf_dir_ready())
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	snprintf(dir, sizeof(dir), "%s/sbin", RCD_SYSCONF_DIR);
	mkdir(dir, 0755);
	snprintf(bin, sizeof(bin), "%s/rvd", dir);
	unlink(bin);

	char *saved = getenv("PATH");
	char keep[512] = "";

	if (saved)
		rss_strlcpy(keep, saved, sizeof(keep));

	/* Nowhere on PATH: absent, and no daemon is running under the suite. */
	setenv("PATH", dir, 1);

	rcd_state_t st;
	char resp[8192];

	memset(&st, 0, sizeof(st));
	st.config_path = "/dev/null";
	ASSERT(rcd_handle("{\"cmd\":\"hello\"}", resp, sizeof(resp), &st) > 0);

	cJSON *r = cJSON_Parse(resp);

	ASSERT(r != NULL);
	const cJSON *rvd = cJSON_GetObjectItemCaseSensitive(
		cJSON_GetObjectItemCaseSensitive(r, "daemons"), "rvd");
	ASSERT(rvd != NULL);
	ASSERT_FALSEm("rvd was reported installed with nothing on PATH",
		      cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rvd, "installed")));
	cJSON_Delete(r);

	/* Now put it somewhere that is not /usr/bin. */
	FILE *f = fopen(bin, "w");

	if (!f) {
		if (keep[0])
			setenv("PATH", keep, 1);
		SKIPm("cannot write a stand-in daemon");
	}
	fputs("#!/bin/sh\nexit 0\n", f);
	fclose(f);
	chmod(bin, 0755);

	ASSERT(rcd_handle("{\"cmd\":\"hello\"}", resp, sizeof(resp), &st) > 0);
	r = cJSON_Parse(resp);
	ASSERT(r != NULL);
	rvd = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(r, "daemons"),
					       "rvd");
	ASSERT(rvd != NULL);
	ASSERTm("a daemon outside /usr/bin was reported absent",
		cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rvd, "installed")));
	cJSON_Delete(r);

	unlink(bin);
	if (keep[0])
		setenv("PATH", keep, 1);
	else
		unsetenv("PATH");
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

/* ------------------------------------------------------------------ */
/* Claiming: the root password, and the one write an unclaimed camera  */
/* accepts from a stranger                                             */
/* ------------------------------------------------------------------ */

#define TEST_SHADOW RCD_SYSCONF_DIR "/shadow"

/* The line a fresh OpenIPC image ships: an empty password field, which is why
 * a camera arrives configurable by nobody. */
static int shadow_says(const char *field)
{
	if (!sysconf_dir_ready())
		return 0;

	FILE *f = fopen(TEST_SHADOW, "w");

	if (!f)
		return 0;
	fprintf(f, "root:%s:19477::::::\ndaemon:*:::::::\nnobody:*:::::::\n", field);
	fclose(f);
	return 1;
}

static const char *shadow_field(char *buf, size_t sz)
{
	FILE *f = fopen(TEST_SHADOW, "r");

	buf[0] = '\0';
	if (!f)
		return buf;

	char line[512];

	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "root:", 5) != 0)
			continue;
		char *end = strchr(line + 5, ':');

		if (end)
			*end = '\0';
		snprintf(buf, sz, "%s", line + 5);
		break;
	}
	fclose(f);
	return buf;
}

/*
 * The grammar is a fourth one rather than a borrowed one, and these are the
 * cases that say why. It is wider than V_CRED, which has no '$' and so cannot
 * carry a hash at all; it is not bounded by WPA's lengths the way V_SECRET is;
 * and it excludes exactly what the store cannot hold, which is a colon and a
 * control byte, rather than the punctuation a password ought to be allowed.
 */
TEST a_password_may_contain_what_a_password_contains(void)
{
	rcd_edit_t e[RCD_EDITS_MAX];
	int n = 0;

	ASSERT_EQ(0, validate_set("{\"section\":\"device\",\"key\":\"root_password\","
				  "\"value\":\"correct horse battery staple\"}",
				  e, &n));
	ASSERT_EQ(0, validate_set("{\"section\":\"device\",\"key\":\"root_password\","
				  "\"value\":\"p@ssw0rd!#%^&*()_+-=[]{}|;\\u0027\\\",.<>/?\"}",
				  e, &n));

	/* Too short, and short is the one length rule there is. */
	ASSERT_EQ(-1, validate_set("{\"section\":\"device\",\"key\":\"root_password\","
				   "\"value\":\"short\"}",
				   e, &n));
	ASSERT_STR_EQ(RCD_E_RANGE, code);

	/* Empty is not "no password": there is no value in this grammar that
	 * asks for an unclaimed camera. */
	ASSERT_EQ(-1, validate_set("{\"section\":\"device\",\"key\":\"root_password\","
				   "\"value\":\"\"}",
				   e, &n));

	/* The store is colon-delimited and line-oriented, and those are the
	 * only two exclusions. */
	ASSERT_EQ(-1, validate_set("{\"section\":\"device\",\"key\":\"root_password\","
				   "\"value\":\"has:a:colon\"}",
				   e, &n));
	ASSERT_STR_EQ(RCD_E_CHOICE, code);
	ASSERT_EQ(-1, validate_set("{\"section\":\"device\",\"key\":\"root_password\","
				   "\"value\":\"has\\na newline\"}",
				   e, &n));
	ASSERT_STR_EQ(RCD_E_CHOICE, code);

	/* And the value never comes back, whatever happens to it. */
	ASSERT_EQ(NULL, strstr(reason, "correct horse"));
	PASS();
}

/*
 * A client that can hash locally may, and that is what makes the setup access
 * point survivable: it is an open network by construction, so an eavesdropper
 * should get something to crack rather than something to send.
 *
 * The shape is checked rather than trusted. A plaintext password that merely
 * begins with '$' must not be mistaken for a hash and stored as one -- that
 * would write a password nobody could ever authenticate with, silently.
 */
TEST a_pre_derived_hash_is_taken_as_one_and_a_lookalike_is_not(void)
{
	rcd_edit_t e[RCD_EDITS_MAX];
	int n = 0;

	ASSERT_EQ(0, validate_set("{\"section\":\"device\",\"key\":\"root_password\","
				  "\"value\":\"$5$abcdefghijklmnop$0123456789abcdef\"}",
				  e, &n));
	ASSERT_STR_EQ("$5$abcdefghijklmnop$0123456789abcdef", e[0].rendered);

	/* Longer than any passphrase, and accepted: a sha512 hash is 106
	 * bytes, which is the reason the value buffer is the width it is. */
	ASSERT_EQ(0, validate_set("{\"section\":\"device\",\"key\":\"root_password\","
				  "\"value\":\"$6$abcdefghijklmnop$"
				  "0123456789012345678901234567890123456789"
				  "0123456789012345678901234567890123456789012345\"}",
				  e, &n));

	static const char *const not_hashes[] = {
		"$money$$$",   /* dollars, but not a hash */
		"$5$saltonly", /* no digest */
		"$5$salt$",    /* an empty digest */
		"$$$",	       /* three markers and nothing else */
		/*
		 * And the store's own two exclusions, which apply here for
		 * the same reason they apply to a plaintext and matter more:
		 * this value is written to /etc/shadow verbatim, so it is the
		 * only path by which either byte could reach the file. A colon
		 * adds a field, truncating the hash to something no login can
		 * reproduce; a newline adds a record, and the first line
		 * naming an account is the one every shadow reader takes.
		 */
		"$5$salt$dig:est",
		"$5$salt$digest\\noperator::20000:0:99999:7:::",
		NULL,
	};

	for (int i = 0; not_hashes[i]; i++) {
		char json[160];

		snprintf(json, sizeof(json),
			 "{\"section\":\"device\",\"key\":\"root_password\","
			 "\"value\":\"%s\"}",
			 not_hashes[i]);
		ASSERT_EQm(not_hashes[i], -1, validate_set(json, e, &n));
		ASSERT_STR_EQ(RCD_E_CHOICE, code);
	}
	PASS();
}

/*
 * What the provider does to the file, and what it refuses to do to it.
 *
 * The line is edited rather than rewritten: every other account and every
 * ageing field on this one has to survive, for the reason the eth0 stanza's
 * hwaddress line has to.
 */
TEST setting_the_password_claims_the_camera(void)
{
	if (!shadow_says(""))
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	ASSERT(rcd_passwd_claimable());
	ASSERT_FALSE(rcd_passwd_claimed());

	/* Emptying this store is not an operation: unconfigured here is a
	 * camera anyone on the network may take. */
	ASSERT_EQ(-1, rcd_provider_root_password.set(""));
	ASSERT(rcd_passwd_claimable());

	ASSERT_EQ(0, rcd_provider_root_password.set("correct horse battery staple"));
	ASSERT(rcd_passwd_claimed());
	ASSERT_FALSEm("a claimed camera is not claimable a second time", rcd_passwd_claimable());

	char field[256];
	shadow_field(field, sizeof(field));
	ASSERT_EQm("the password itself must never reach the file", NULL,
		   strstr(field, "correct horse"));
	ASSERT_EQm("hashed with the method rcd asked for", 0, strncmp(field, "$5$", 3));

	/* And the accounts this writer does not own are still there. */
	FILE *f = fopen(TEST_SHADOW, "r");
	ASSERT(f != NULL);
	char all[1024] = "";
	size_t got = fread(all, 1, sizeof(all) - 1, f);
	all[got] = '\0';
	fclose(f);
	ASSERT(strstr(all, "daemon:*:") != NULL);
	ASSERT(strstr(all, "nobody:*:") != NULL);
	PASS();
}

/* A pre-derived value is stored as it came. Re-hashing a hash would store the
 * wrong thing, and nothing downstream would notice until a login failed. */
TEST a_pre_derived_hash_is_stored_verbatim(void)
{
	if (!shadow_says(""))
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	const char *given = "$5$abcdefghijklmnop$0123456789abcdef";

	ASSERT_EQ(0, rcd_provider_root_password.set(given));

	char field[256];
	ASSERT_STR_EQ(given, shadow_field(field, sizeof(field)));

	/* And `get` answers with the store, which is what lets `set` tell a
	 * password change from a no-op. It is never reported to a client --
	 * emit_value refuses a secret before the provider is asked. */
	char back[RCD_VAL_MAX];
	ASSERT_EQ(0, rcd_provider_root_password.get(back, sizeof(back)));
	ASSERT_STR_EQ(given, back);
	PASS();
}

/*
 * And the writer refuses the same two bytes on its own account, which is the
 * check that does not depend on the grammar having run. The value that reaches
 * this function has been hashed by one path and passed through untouched by
 * another, and only the file knows what the file cannot hold.
 */
TEST the_writer_refuses_a_field_the_store_cannot_hold(void)
{
	if (!shadow_says(""))
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	static const char *const unstorable[] = {
		"$5$salt$dig:est",				/* adds a field */
		"$5$salt$digest\noperator::20000:0:99999:7:::", /* adds a record */
		NULL,
	};

	for (int i = 0; unstorable[i]; i++) {
		ASSERT_EQm(unstorable[i], -1, rcd_provider_root_password.set(unstorable[i]));
		ASSERTm("and the camera is still there to be claimed", rcd_passwd_claimable());
	}

	/* Nothing was written on the way to refusing: the other accounts are
	 * as they were, and root still has the one line it started with. */
	FILE *f = fopen(TEST_SHADOW, "r");
	ASSERT(f != NULL);
	char all[1024] = "";
	size_t got = fread(all, 1, sizeof(all) - 1, f);

	all[got] = '\0';
	fclose(f);
	ASSERT_STR_EQ("root::19477::::::\ndaemon:*:::::::\nnobody:*:::::::\n", all);
	PASS();
}

/*
 * The refusals, which are the half that must never break: this is the only
 * unauthenticated write on the device.
 *
 * rhd has already declined to forward a claim on a camera that is not
 * claimable. These are rcd not taking rhd's word for it.
 */
TEST a_claim_is_refused_on_a_camera_that_is_not_claimable(void)
{
	rcd_state_t st;
	memset(&st, 0, sizeof(st));

	if (!shadow_says("$5$abcdefghijklmnop$0123456789abcdef"))
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	cJSON *req = cJSON_Parse("{\"cmd\":\"claim\",\"password\":\"a good long password\"}");
	cJSON *resp = rcd_cmd_claim(&st, req);

	ASSERT(resp != NULL);
	ASSERT_STR_EQ("error", cJSON_GetObjectItemCaseSensitive(resp, "status")->valuestring);
	ASSERT_STR_EQ(RCD_E_CLAIMED, cJSON_GetObjectItemCaseSensitive(resp, "code")->valuestring);
	cJSON_Delete(resp);

	/* An account locked on purpose is not an unclaimed one, and it is
	 * refused with the same code and a different sentence. */
	ASSERT(shadow_says("!"));
	resp = rcd_cmd_claim(&st, req);
	ASSERT_STR_EQ(RCD_E_CLAIMED, cJSON_GetObjectItemCaseSensitive(resp, "code")->valuestring);
	cJSON_Delete(resp);

	/* A claim carrying nothing to set is a malformed request, not a
	 * claimed camera: the shapes must not be confused. */
	ASSERT(shadow_says(""));
	cJSON *empty = cJSON_Parse("{\"cmd\":\"claim\"}");
	resp = rcd_cmd_claim(&st, empty);
	ASSERT_STR_EQ(RCD_E_MALFORMED, cJSON_GetObjectItemCaseSensitive(resp, "code")->valuestring);
	cJSON_Delete(resp);
	cJSON_Delete(empty);
	cJSON_Delete(req);
	PASS();
}

TEST the_schema_says_where_a_system_key_takes_effect(void)
{
	cJSON *out = cJSON_CreateObject();
	rcd_schema_emit(out, "device");
	const cJSON *keys = cJSON_GetObjectItemCaseSensitive(out, "keys");
	ASSERT(cJSON_IsArray(keys));
	ASSERT_EQ(4, cJSON_GetArraySize(keys));

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
		 * be advertised as write-only the way a credential is -- with
		 * one exception, which is the key that is both. */
		if (strcmp(key->valuestring, "root_password") != 0)
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
		if (strcmp(key->valuestring, "root_password") == 0) {
			/* Settable and never readable, like every other
			 * credential -- and, unlike the others, backed by a
			 * store, so the schema can also say whether one has
			 * been set without saying what it is. That bit is what
			 * lets a form offer "set a password" or "change it"
			 * rather than an input that always looks empty. */
			ASSERT(cJSON_IsFalse(ro));
			ASSERT_STR_EQ("password",
				      cJSON_GetObjectItemCaseSensitive(k, "type")->valuestring);
			ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(k, "accepts_hash")));

			/* Live: /etc/shadow is read at every authentication,
			 * so nothing is owed to `apply` and nothing waits. */
			ASSERT_STR_EQ("live", tier->valuestring);
			ASSERT_STR_EQ("none", imp->valuestring);

			/* And not guarded, deliberately. A revert here would
			 * put an empty hash back and un-claim the camera on a
			 * timer. See the key's entry in rcd_schema.c. */
			ASSERT_EQ(NULL, cJSON_GetObjectItemCaseSensitive(k, "guard_sec"));
			checked++;
		}
	}
	ASSERT_EQ(4, checked);
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
 * The overlay's font size takes a share of the picture as well as a count of
 * pixels, and keeps it as written.
 *
 * A size in pixels is a size on one picture: 24 is a caption on a 1520-line
 * encode and a banner on a 360-line one. "4%" is the second thing to say and
 * is not a point on the pixel scale, so it stays a string all the way to the
 * file, where rod reads it the same way.
 */
TEST a_font_size_takes_a_share_of_the_picture(void)
{
	rcd_edit_t e[RCD_EDITS_MAX];
	int n = 0;

	ASSERT_SET_OK("{\"section\":\"osd\",\"key\":\"font_size\",\"value\":\"4%\"}", e, &n);
	ASSERT_STR_EQ("4%", e[0].rendered);
	ASSERTm("a percentage reached the daemon as a number", !e[0].is_num);

	/* One decimal place, because a whole percent of a 1520-line encode is
	 * a fifteen-pixel step. */
	ASSERT_SET_OK("{\"section\":\"osd\",\"key\":\"font_size\",\"value\":\"4.5%\"}", e, &n);
	ASSERT_STR_EQ("4.5%", e[0].rendered);

	/* And the pixels still are pixels. */
	ASSERT_SET_OK("{\"section\":\"osd\",\"key\":\"font_size\",\"value\":55}", e, &n);
	ASSERT_STR_EQ("55", e[0].rendered);
	ASSERT(e[0].is_num);
	PASS();
}

/*
 * The ends are the percentage's own. A share outside them is a size no
 * picture makes legible or a banner across the frame, and both are likelier
 * to be a misplaced digit than a request.
 */
TEST a_share_of_the_picture_has_its_own_range(void)
{
	ASSERT_SET_REFUSED("{\"section\":\"osd\",\"key\":\"font_size\",\"value\":\"0.1%\"}");
	ASSERT_STR_EQ(RCD_E_RANGE, code);
	ASSERT_SET_REFUSED("{\"section\":\"osd\",\"key\":\"font_size\",\"value\":\"40%\"}");
	ASSERT_STR_EQ(RCD_E_RANGE, code);

	/* And the spelling is the spelling: what rcd writes has to be what rod
	 * reads, so anything it is not certain of is refused here rather than
	 * discovered on the picture. */
	ASSERT_SET_REFUSED("{\"section\":\"osd\",\"key\":\"font_size\",\"value\":\"4 %\"}");
	ASSERT_SET_REFUSED("{\"section\":\"osd\",\"key\":\"font_size\",\"value\":\"4.25%\"}");
	ASSERT_SET_REFUSED("{\"section\":\"osd\",\"key\":\"font_size\",\"value\":\"%\"}");
	ASSERT_SET_REFUSED("{\"section\":\"osd\",\"key\":\"font_size\",\"value\":\"4px\"}");
	ASSERT_SET_REFUSED("{\"section\":\"osd\",\"key\":\"font_size\",\"value\":\"4\"}");

	/* Only this key. Every other integer here is a magnitude in units of
	 * its own, with no picture to be a share of. */
	ASSERT_SET_REFUSED("{\"section\":\"osd\",\"key\":\"font_stroke\",\"value\":\"4%\"}");
	ASSERT_STR_EQ(RCD_E_TYPE, code);
	ASSERT_SET_REFUSED("{\"section\":\"jpeg\",\"key\":\"quality\",\"value\":\"80%\"}");
	ASSERT_STR_EQ(RCD_E_TYPE, code);
	PASS();
}

/*
 * A client has no other way to learn the word exists: it is not a number in
 * the range, and a form drawn from the range alone can never say "leave this
 * to the tuning". Said only where it is true.
 */
/*
 * And which keys their owner can put back without being restarted.
 *
 * `tier` does not answer this: tier is what setting a value costs, and a reset
 * is a different operation with a different cost -- restart-tier for almost
 * every key however live it is to set, because rcd has no default to hand a
 * live command. A client reading only `tier` stages every reset behind an
 * apply and warns about a restart that will not happen, which is what the
 * console did for thirteen knobs whose owner could have undone the write on
 * the spot.
 *
 * hflip is the counter-example and shares the section: live to set, and rvd
 * has no way to un-write it short of reading the file again at its next start.
 */
TEST the_schema_says_which_keys_reset_live(void)
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
		const cJSON *rl = cJSON_GetObjectItemCaseSensitive(k, "resets_live");

		if (!cJSON_IsString(sec) || !cJSON_IsString(key))
			continue;

		/* Published for every key, so a client can tell "no" from "this
		 * camera is too old to say". */
		ASSERTm("every key states whether it resets live", cJSON_IsBool(rl));

		if (strcmp(sec->valuestring, "image") == 0 &&
		    strcmp(key->valuestring, "contrast") == 0) {
			ASSERTm("an ISP knob resets live", cJSON_IsTrue(rl));
			checked++;
		}
		if (strcmp(sec->valuestring, "image") == 0 &&
		    strcmp(key->valuestring, "hflip") == 0) {
			ASSERTm("orientation does not", cJSON_IsFalse(rl));
			checked++;
		}
		if (strcmp(sec->valuestring, "stream0") == 0 &&
		    strcmp(key->valuestring, "width") == 0) {
			ASSERTm("nor does a restart-tier key", cJSON_IsFalse(rl));
			checked++;
		}
	}
	ASSERT_EQ(3, checked);
	cJSON_Delete(out);
	PASS();
}

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

	/*
	 * And the same for the second range a font size can be written on: a
	 * form drawn from the pixel range alone can offer only half of what
	 * the key takes, and has no other way to learn about the other half.
	 */
	const cJSON *fs = NULL, *stroke = NULL;
	cJSON_ArrayForEach(k, keys)
	{
		const cJSON *sec = cJSON_GetObjectItemCaseSensitive(k, "section");
		const cJSON *key = cJSON_GetObjectItemCaseSensitive(k, "key");

		if (!cJSON_IsString(sec) || strcmp(sec->valuestring, "osd") != 0)
			continue;
		if (strcmp(key->valuestring, "font_size") == 0)
			fs = k;
		if (strcmp(key->valuestring, "font_stroke") == 0)
			stroke = k;
	}
	ASSERT(fs && stroke);
	ASSERT_EQ(NULL, cJSON_GetObjectItemCaseSensitive(stroke, "percent"));
	const cJSON *pct = cJSON_GetObjectItemCaseSensitive(fs, "percent");
	ASSERT(cJSON_IsObject(pct));
	ASSERT_EQ(RCD_PCT_MIN / 10.0,
		  cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(pct, "min")));
	ASSERT_EQ(RCD_PCT_MAX / 10.0,
		  cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(pct, "max")));
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
 * Rotation is four angles, not a range, and what lands in the file has to be
 * degrees: rvd reads [image] rotate with rss_config_get_int, so an index would
 * mean 90 where 1 was asked for and would do it silently, on the next start.
 * A numeric enum writes its own spelling, which is the whole reason it is one
 * rather than a labelled integer -- and a labels array could not name 0, 90,
 * 180 and 270 anyway, labelling min + i.
 */
TEST rotation_is_degrees_and_only_right_angles(void)
{
	rcd_edit_t e[RCD_EDITS_MAX];
	int n = 0;

	ASSERT_SET_OK("{\"section\":\"image\",\"key\":\"rotate\",\"value\":270}", e, &n);
	ASSERT_STR_EQm("a number has to reach the file as the same number", "270", e[0].rendered);
	n = 0;
	ASSERT_SET_OK("{\"section\":\"image\",\"key\":\"rotate\",\"value\":\"90\"}", e, &n);
	ASSERT_STR_EQm("and so does the same value spelled as a string", "90", e[0].rendered);

	/* The VPSS has no setting for an angle off the quarter turns, and a
	 * value it would ignore is worse stored than refused: the file would
	 * say the picture is turned and the picture would not be. */
	n = 0;
	ASSERTm("45 is not an angle this hardware turns to",
		validate_set("{\"section\":\"image\",\"key\":\"rotate\",\"value\":45}", e, &n) !=
			0);
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

/*
 * And a font size written as a share of the picture reads back as one.
 *
 * Read as a number, "4%" is 4 -- a size on the pixel scale, inside the range,
 * and off by a factor of the frame height. A client that showed it and wrote
 * it back would turn a legible overlay into four pixels of nothing.
 */
TEST a_font_size_in_percent_reads_back_in_percent(void)
{
	rcd_state_t st;
	char path[320];

	if (!sysconf_dir_ready())
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	snprintf(path, sizeof(path), "%s/raptor.conf", RCD_SYSCONF_DIR);
	FILE *f = fopen(path, "w");
	ASSERT(f);
	fputs("[osd]\nfont_size = 4%\nfont_stroke = 2\n", f);
	fclose(f);

	memset(&st, 0, sizeof(st));
	st.config_path = path;

	cJSON *req = cJSON_Parse("{\"keys\":[{\"section\":\"osd\",\"key\":\"font_size\"},"
				 "{\"section\":\"osd\",\"key\":\"font_stroke\"}]}");
	cJSON *resp = rcd_cmd_get(&st, req);
	cJSON_Delete(req);
	ASSERT(resp);

	const cJSON *vals = cJSON_GetObjectItemCaseSensitive(resp, "values");
	const cJSON *fs = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(vals, 0), "value");
	const cJSON *sk = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(vals, 1), "value");

	ASSERTm("a size in percent read back as a number", cJSON_IsString(fs));
	ASSERT_STR_EQ("4%", fs->valuestring);
	ASSERTm("a size in pixels read back as a string", cJSON_IsNumber(sk));
	ASSERT_EQ(2, (int)cJSON_GetNumberValue(sk));

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

	/*
	 * And nothing about this list is limited to the ISP setters, which is
	 * what it was quietly taken to be. Rotation is an [image] key whose
	 * setter is on the framesource half of the vtable; it reached the
	 * schema naming no entry in rvd's settable list and was hidden on
	 * every camera as a result -- a control nobody could see and no test
	 * could miss, because nothing tied the two lists together.
	 *
	 * Both directions, because the answer is the camera's: a part that can
	 * turn a picture says so and a part that cannot must not be offered
	 * the control. The i6c list above is the second case already.
	 */
	ASSERT_EQm("a camera that did not publish rotation must not offer it", false,
		   rcd_key_available(&st, rcd_key_find("image", "rotate")));
	snprintf(st.isp_settable, sizeof(st.isp_settable),
		 ",brightness,contrast,saturation,hflip,vflip,ae_comp,drc_strength,rotate,");
	ASSERTm("a camera that publishes rotation must offer it",
		rcd_key_available(&st, rcd_key_find("image", "rotate")));
	ASSERT_EQm("and the same list still hides what it does not name", false,
		   rcd_key_available(&st, rcd_key_find("image", "sharpness")));

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
	ASSERT_EQ(4, seen);
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

/* A reset of a live key that its owner cannot put back is not live: there is
 * no value to hand the running daemon, and the default it will read is in the
 * daemon, not in rcd. So it goes to the file and the owner is recorded behind,
 * whatever the key's tier is when it carries a value.
 *
 * Orientation is the case: live to set, and rvd has no way to undo the write
 * short of being restarted. The ISP knobs are the other case and name a
 * live_reset -- see resetting_an_isp_knob_asks_rvd_to_put_it_back. */
TEST a_reset_is_never_live(void)
{
	rcd_state_t st;
	char path[320];

	if (!sysconf_dir_ready())
		SKIPm("no writable " RCD_SYSCONF_DIR " -- run the suite under unshare -rm");

	snprintf(path, sizeof(path), "%s/raptor.conf", RCD_SYSCONF_DIR);
	FILE *f = fopen(path, "w");
	ASSERT(f);
	fputs("[image]\nhflip = 1\n", f);
	fclose(f);

	memset(&st, 0, sizeof(st));
	st.config_path = path;

	const rcd_key_t *k = rcd_key_find("image", "hflip");
	ASSERT(k);
	ASSERTm("the key under test is not a live one", rcd_key_live(k));
	ASSERTm("and its owner cannot put it back", k->live_reset == NULL);

	cJSON *req = cJSON_Parse("{\"section\":\"image\",\"key\":\"hflip\",\"value\":null}");
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
	ASSERT(strstr(text, "hflip") == NULL);
	ASSERT_EQ(1, st.stale_count);

	unlink(path);
	PASS();
}

/*
 * And a reset of a key its owner *can* put back is sent, not deferred.
 *
 * An ISP knob lives in the driver and outlives rvd, so taking the key out of
 * the file un-writes nothing: the restart rcd used to record re-reads a file
 * that no longer names the knob and leaves the last value applied, on Ingenic
 * until the next power cycle. rvd is asked instead, and it knows what to put
 * back because the HAL publishes it.
 *
 * The request is asserted rather than just the outcome. rvd answers ok to a
 * great many things, so a reply alone would pass against a reset that sent the
 * wrong command, or the right command naming the wrong knob.
 */
TEST resetting_an_isp_knob_asks_rvd_to_put_it_back(void)
{
	fake_daemon_t d;
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
	/* Checked for NULL before it is compared: ASSERT_STR_EQ strcmps its
	 * arguments, so a key that stopped naming a restore would take the
	 * whole suite down with a SEGV instead of failing this line. */
	ASSERTm("the key under test names a restore", k->live_reset != NULL);
	ASSERT_STR_EQ("reset-isp", k->live_reset);

	if (!fake_daemon_start_reply(&d, "rvd", "{\"status\":\"ok\"}")) {
		unlink(path);
		SKIPm("cannot listen on " RSS_RUN_DIR " -- run the suite under unshare -rm");
	}

	cJSON *req = cJSON_Parse("{\"section\":\"image\",\"key\":\"brightness\",\"value\":null}");
	cJSON *resp = rcd_cmd_set(&st, req);

	cJSON_Delete(req);
	fake_daemon_stop(&d, "rvd");
	ASSERT(resp);
	cJSON_Delete(resp);

	ASSERTm("rvd was asked to put the knob back",
		strstr(d.last_req, "\"cmd\":\"reset-isp\"") != NULL);
	ASSERTm("and told which knob", strstr(d.last_req, "\"key\":\"brightness\"") != NULL);
	ASSERTm("with no value of rcd's own", strstr(d.last_req, "value") == NULL);

	/* The key still leaves the file -- that is what a reset is -- but
	 * nothing is owed afterwards, because the picture already moved. */
	char text[256] = "";

	f = fopen(path, "r");
	ASSERT(f);
	fread(text, 1, sizeof(text) - 1, f);
	fclose(f);
	ASSERT(strstr(text, "brightness") == NULL);
	ASSERT_EQm("a restore that worked owes no restart", 0, st.stale_count);

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
	RUN_TEST(a_repeat_row_is_served_apart_from_the_keys);
	RUN_TEST(an_element_name_is_a_name);
	RUN_TEST(making_an_element_is_remembered);
	RUN_TEST(refuses_unlisted_actions);
	RUN_TEST(refuses_near_misses);
	RUN_TEST(restarting_is_rcds_own_and_says_what_it_costs);
	RUN_TEST(forgetting_the_network_is_rcds_own_action);
	RUN_TEST(forgetting_the_network_has_no_near_misses);
	RUN_TEST(the_schema_says_what_forgetting_the_network_costs);
	RUN_TEST(every_action_can_be_routed);
	RUN_TEST(a_scan_is_a_read_and_is_priced_as_one);
	RUN_TEST(a_scan_is_rcds_own_action);
	RUN_TEST(state_always_carries_the_provisioning_fact);
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
	RUN_TEST(a_live_command_is_sent_the_field_it_asks_for);
	RUN_TEST(a_selector_names_the_key_that_carries_it);
	RUN_TEST(every_daynight_threshold_is_a_live_key);
	RUN_TEST(an_element_carries_the_same_keys_whatever_it_is_called);
	RUN_TEST(the_elements_are_listed_the_way_the_file_reads);
	RUN_TEST(a_section_rod_would_not_draw_is_not_an_element);
	RUN_TEST(a_config_with_no_elements_lists_none);
	RUN_TEST(the_pattern_answers_with_every_element_by_name);
	RUN_TEST(an_empty_value_from_a_daemon_is_no_value);
	RUN_TEST(the_pattern_with_no_elements_is_not_an_error);
	RUN_TEST(a_key_for_an_element_that_is_not_there_makes_no_element);
	RUN_TEST(the_pattern_is_not_a_section_to_write_to);
	RUN_TEST(an_element_the_camera_named_is_edited_under_that_name);
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
	RUN_TEST(resetting_an_isp_knob_asks_rvd_to_put_it_back);
	RUN_TEST(resetting_the_network_puts_the_shipped_stanza_back);
	RUN_TEST(a_password_may_contain_what_a_password_contains);
	RUN_TEST(a_pre_derived_hash_is_taken_as_one_and_a_lookalike_is_not);
	RUN_TEST(setting_the_password_claims_the_camera);
	RUN_TEST(a_pre_derived_hash_is_stored_verbatim);
	RUN_TEST(the_writer_refuses_a_field_the_store_cannot_hold);
	RUN_TEST(a_claim_is_refused_on_a_camera_that_is_not_claimable);
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
	RUN_TEST(a_font_size_takes_a_share_of_the_picture);
	RUN_TEST(a_share_of_the_picture_has_its_own_range);
	RUN_TEST(the_schema_says_which_keys_reset_live);
	RUN_TEST(the_schema_says_which_keys_take_auto);
	RUN_TEST(exposure_compensation_goes_both_ways);
	RUN_TEST(rotation_is_degrees_and_only_right_angles);
	RUN_TEST(a_knob_left_on_auto_reads_back_as_auto);
	RUN_TEST(a_font_size_in_percent_reads_back_in_percent);
	RUN_TEST(refuses_more_edits_than_a_request_may_carry);
	RUN_TEST(state_leaves_out_an_isp_knob_rvd_could_not_read);
	RUN_TEST(get_asks_a_daemon_once_however_many_keys_name_its_section);
	RUN_TEST(an_element_reaches_the_file_before_the_next_request);
	RUN_TEST(get_asks_once_per_distinct_section);
	RUN_TEST(get_refuses_more_keys_than_a_request_may_carry);
	RUN_TEST(the_live_cache_holds_every_section_the_table_has);
	RUN_TEST(the_codec_numbers_are_a_wire_format);
	RUN_TEST(an_init_script_is_found_whatever_number_it_carries);
	RUN_TEST(the_init_script_match_is_anchored);
	RUN_TEST(a_daemon_is_found_wherever_the_image_puts_it);

	RUN_TEST(the_wait_budget_is_wall_time_not_a_count_of_sleeps);
	RUN_TEST(the_wait_reports_the_time_it_measured);
	RUN_TEST(a_daemon_that_answers_is_not_waited_out);
	RUN_TEST(a_spent_budget_still_asks_once);

	RUN_TEST(every_refusal_explains_itself);
}
