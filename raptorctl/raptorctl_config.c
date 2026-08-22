/*
 * raptorctl_config.c -- Config get/set/save subcommand
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>
#include <rss_ipc.h>
#include <rss_common.h>

#include "raptorctl.h"

static const char *find_daemon_for_section(const char *section)
{
	static const struct {
		const char *section;
		const char *daemon;
	} map[] = {
		{"sensor", "rvd"},     {"stream0", "rvd"},    {"stream1", "rvd"},
		{"jpeg", "rvd"},       {"ring", "rvd"},	      {"audio", "rad"},
		{"rtsp", "rsd"},       {"http", "rhd"},	      {"osd", "rod"},
		{"ircut", "ric"},      {"recording", "rmr"},  {"motion", "rmd"},
		{"webrtc", "rwd"},     {"webtorrent", "rwd"}, {"webcam", "rwc"},
		{"filesource", "rfs"}, {"log", "rvd"},	      {NULL, NULL},
	};
	for (int i = 0; map[i].section; i++) {
		if (strcmp(section, map[i].section) == 0)
			return map[i].daemon;
	}
	return NULL;
}

static void print_config_entry(const char *key, const char *value, void *userdata)
{
	int *count = userdata;
	printf("%s = %s\n", key, value);
	(*count)++;
}

static void print_section_json(const char *section, const char *json_str)
{
	printf("[%s]\n", section);
	cJSON *root = cJSON_Parse(json_str);
	if (!root) {
		fprintf(stderr, "(unparseable response for [%s], section omitted)\n", section);
		return;
	}
	cJSON *keys_obj = cJSON_GetObjectItemCaseSensitive(root, "keys");
	if (!keys_obj) {
		cJSON_Delete(root);
		return;
	}
	cJSON *item = NULL;
	cJSON_ArrayForEach(item, keys_obj)
	{
		const char *v = cJSON_GetStringValue(item);
		if (v)
			printf("%s = %s\n", item->string, v);
	}
	cJSON_Delete(root);
}

/*
 * A value typed the way rcd's schema expects it.
 *
 * The shell hands everything over as a string, and rcd wants a number for a
 * number and a boolean for a boolean -- so the shape is recovered here rather
 * than each caller being asked to quote correctly. An enum and a credential
 * are strings either way, and a string that happens to look like a number is
 * refused by the table rather than silently written as one.
 */
static void add_typed(cJSON *j, const char *key, const char *val)
{
	if (strcmp(val, "true") == 0 || strcmp(val, "false") == 0) {
		cJSON_AddBoolToObject(j, key, strcmp(val, "true") == 0);
		return;
	}
	char *end;
	long lv = strtol(val, &end, 10);
	if (*end == '\0' && end != val)
		cJSON_AddNumberToObject(j, key, (double)lv);
	else
		cJSON_AddStringToObject(j, key, val);
}

/*
 * Writes go through rcd rather than to the file.
 *
 * Editing raptor.conf from here would skip the validation, skip applying the
 * keys that can be applied without a restart, and leave nothing to record that
 * a daemon is now running behind -- so a write that rcd cannot carry is
 * refused rather than done a worse way. Which is why an absent rcd is an
 * error naming the fix, and not a quiet fallback.
 */
static int config_set_via_rcd(const char *section, const char *key, const char *value)
{
	cJSON *j = jcmd("set");
	if (!j)
		return 1;
	jadd_s(j, "section", section);
	jadd_s(j, "key", key);
	add_typed(j, "value", value);

	char req[512];
	jstr(j, req, sizeof(req));
	if (!req[0]) {
		fprintf(stderr, "config set: request too large\n");
		return 1;
	}

	char sock[64];
	snprintf(sock, sizeof(sock), RSS_SOCK_FMT, "rcd");

	char *resp = NULL;
	if (rss_ctrl_send_command_alloc(sock, req, &resp, 30000) < 0) {
		fprintf(stderr, "config set: rcd is not running, and writes go "
				"through it.\nStart it (/etc/init.d/S31raptor start) "
				"and try again.\n");
		return 1;
	}

	cJSON *root = cJSON_Parse(resp);
	free(resp);
	if (!root) {
		fprintf(stderr, "config set: rcd answered unusably\n");
		return 1;
	}

	const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
	int rc = 0;
	if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0) {
		const cJSON *why = cJSON_GetObjectItemCaseSensitive(root, "reason");
		fprintf(stderr, "config set: %s\n",
			cJSON_IsString(why) ? why->valuestring : "refused");
		rc = 1;
	} else {
		/*
		 * Said out loud, because it is the whole difference between the
		 * two tiers and it is otherwise invisible: the value is in the
		 * file and the running daemon has not read it.
		 */
		const cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "results");
		const cJSON *first = cJSON_GetArrayItem(results, 0);
		const cJSON *applied = cJSON_GetObjectItemCaseSensitive(first, "applied");
		if (cJSON_IsString(applied) && strcmp(applied->valuestring, "saved") == 0)
			printf("saved; run 'raptorctl config apply' to restart what needs it\n");
	}

	cJSON_Delete(root);
	return rc;
}

/*
 * A verb that is not a value.
 *
 * Deliberately a thin way to name one. rcd's table decides which actions
 * exist, what arguments each takes and what performing one costs, and a field
 * that table does not name is dropped before it reaches anything -- so there
 * is nothing here to keep in agreement with it, and no list to fall behind.
 * `raptorctl rcd schema` prints what may be asked for.
 *
 * Arguments are key/value pairs, typed the way a `set` value is.
 */
static int config_action(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "Usage: raptorctl config action <name> [key value ...]\n");
		return 1;
	}
	if ((argc - 4) % 2 != 0) {
		fprintf(stderr, "config action: arguments come in key/value pairs\n");
		return 1;
	}

	cJSON *j = jcmd("action");
	if (!j)
		return 1;
	jadd_s(j, "action", argv[3]);
	for (int i = 4; i + 1 < argc; i += 2)
		add_typed(j, argv[i], argv[i + 1]);

	char req[512];
	jstr(j, req, sizeof(req));
	if (!req[0]) {
		fprintf(stderr, "config action: request too large\n");
		return 1;
	}

	char sock[64];
	snprintf(sock, sizeof(sock), RSS_SOCK_FMT, "rcd");

	char *resp = NULL;
	if (rss_ctrl_send_command_alloc(sock, req, &resp, 30000) < 0) {
		fprintf(stderr, "config action: rcd is not running, and actions go "
				"through it.\n");
		return 1;
	}

	cJSON *root = cJSON_Parse(resp);
	free(resp);
	if (!root) {
		fprintf(stderr, "config action: rcd answered unusably\n");
		return 1;
	}

	const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
	int rc = 0;
	if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0) {
		const cJSON *why = cJSON_GetObjectItemCaseSensitive(root, "reason");
		fprintf(stderr, "config action: %s\n",
			cJSON_IsString(why) ? why->valuestring : "refused");
		rc = 1;
	} else {
		/* The note is the half that is not obvious from having asked:
		 * an action that takes effect at the next boot looks exactly
		 * like one that has already happened. */
		const cJSON *note = cJSON_GetObjectItemCaseSensitive(root, "note");
		if (cJSON_IsString(note))
			printf("%s: %s\n", argv[3], note->valuestring);
		else
			printf("%s: done\n", argv[3]);
	}

	cJSON_Delete(root);
	return rc;
}

int handle_config(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr,
			"Usage: raptorctl config <get|set|save|apply|pending|action> ...\n");
		return 1;
	}

	/* Enacting saved edits, and asking what is owed. Both belong to rcd
	 * because both are about the difference between the file and what is
	 * running, which is the one thing only rcd tracks. */
	if (strcmp(argv[2], "apply") == 0)
		return send_cmd_verb("rcd", "apply");
	if (strcmp(argv[2], "pending") == 0)
		return send_cmd_verb("rcd", "pending");

	if (strcmp(argv[2], "action") == 0)
		return config_action(argc, argv);

	/* config save — tell all daemons to save */
	if (strcmp(argv[2], "save") == 0) {
		int saved = 0;
		for (int i = 0; daemons[i]; i++) {
			char sock_path[64];
			char resp[2048];
			snprintf(sock_path, sizeof(sock_path), RSS_SOCK_FMT, daemons[i]);
			int ret = rss_ctrl_cmd(sock_path, "config-save", resp, sizeof(resp), 2000);
			if (ret >= 0) {
				printf("%s: %s\n", daemons[i], resp);
				saved++;
			}
		}
		if (saved == 0) {
			fprintf(stderr, "No daemons responded\n");
			return 1;
		}
		return 0;
	}

	/* config get <section> [key] */
	if (strcmp(argv[2], "get") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl config get <section> [key]\n");
			return 1;
		}
		const char *section = argv[3];
		const char *key = argc >= 5 ? argv[4] : NULL;

		/* Try daemon first for single-key get */
		if (key) {
			const char *target = find_daemon_for_section(section);
			if (target) {
				char sock_path[64];
				char resp[2048];
				char cmd_json[256];
				snprintf(sock_path, sizeof(sock_path), RSS_SOCK_FMT, target);
				cJSON *j = jcmd("config-get");
				if (!j)
					return 1;
				jadd_s(j, "section", section);
				jadd_s(j, "key", key);
				jstr(j, cmd_json, sizeof(cmd_json));
				int ret = rss_ctrl_send_command(sock_path, cmd_json, resp,
								sizeof(resp), 2000);
				if (ret >= 0) {
					printf("%s\n", resp);
					return 0;
				}
			}
		}

		if (!key) {
			/* Section dump: try daemon first */
			const char *target = find_daemon_for_section(section);
			if (target) {
				char sock_path[64];
				char resp[2048];
				char cmd_json[256];
				snprintf(sock_path, sizeof(sock_path), RSS_SOCK_FMT, target);
				cJSON *j = jcmd("config-get-section");
				if (!j)
					return 1;
				jadd_s(j, "section", section);
				jstr(j, cmd_json, sizeof(cmd_json));
				int ret = rss_ctrl_send_command(sock_path, cmd_json, resp,
								sizeof(resp), 2000);
				if (ret >= 0) {
					print_section_json(section, resp);
					return 0;
				}
			}
		}

		/* Fallback: read from config file */
		const char *cfgpath = RSS_CONFIG_PATH;
		rss_config_t *cfg = rss_config_load(cfgpath);
		if (!cfg) {
			fprintf(stderr, "Config not found\n");
			return 1;
		}

		if (key) {
			const char *val = rss_config_get_str(cfg, section, key, NULL);
			if (val)
				printf("%s\n", val);
			else
				fprintf(stderr, "Key not found: [%s] %s\n", section, key);
			rss_config_free(cfg);
			return val ? 0 : 1;
		}

		/* Section dump from file */
		printf("[%s]\n", section);
		int count = 0;
		rss_config_foreach(cfg, section, print_config_entry, &count);
		if (count == 0)
			fprintf(stderr, "Section not found: [%s]\n", section);
		rss_config_free(cfg);
		return count > 0 ? 0 : 1;
	}

	/* config set <section> <key> <value> */
	if (strcmp(argv[2], "set") == 0) {
		if (argc < 6) {
			fprintf(stderr, "Usage: raptorctl config set <section> <key> <value>\n");
			return 1;
		}
		return config_set_via_rcd(argv[3], argv[4], argv[5]);
	}

	fprintf(stderr, "Usage: raptorctl config <get|set|save|apply|pending> ...\n");
	return 1;
}
