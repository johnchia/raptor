/*
 * raptorctl.c -- Raptor control CLI
 *
 * Usage:
 *   raptorctl status                       Check which daemons are running
 *   raptorctl config get <section> [key]   Read config value(s)
 *   raptorctl config set <section> <key> <value>  Set a config value
 *   raptorctl config save                  Save running config to disk
 *   raptorctl <daemon> <command> [args]    Send command to daemon
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cJSON.h>
#include <rss_ipc.h>
#include <rss_common.h>

#include "raptorctl.h"

const char *daemons[] = {"rvd", "rsd", "rad", "rod", "rhd", "ric", "rmr",     "rmd", "rwd",
			 "rwc", "rfs", "rsp", "rsr", "rmq", "rcd", "rsd-555", NULL};

/*
 * `rvd restart`, routed through rcd instead of sent.
 *
 * rvd owns the SoC's media state: on HiMPP its teardown calls
 * HI_MPI_SYS_Exit, which runs every MPP module's exit including the audio
 * ones rad is holding, and a module torn down with its buffers still
 * referenced leaves CMA pages the driver cannot reclaim -- after which no
 * rvd can configure VB again and the board needs its kernel modules
 * reloaded. So the restart has to be bracketed: release the other holders,
 * restart, put them back. rcd does exactly that (rcd_apply.c, restart_rvd)
 * and has done since the console gained restart-tier settings; sending the
 * verb straight to rvd's own socket was the one path that skipped it.
 *
 * No fallback when rcd is missing. Falling back would be sending the
 * unbracketed restart -- the thing this exists to stop -- and the operator
 * has a correct alternative to be told about instead.
 */
static int restart_rvd_via_rcd(const char *daemon)
{
	char json[96];
	cJSON *j, *arr;
	int ok;

	if (rss_daemon_check("rcd") <= 0) {
		fprintf(stderr,
			"rcd is not running, and restarting %s without it would leave the "
			"media drivers holding buffers nothing can free.\n"
			"Restart the stack instead: /etc/init.d/S95raptor restart\n",
			daemon);
		return 1;
	}

	j = cJSON_CreateObject();
	if (!j)
		return 1;
	cJSON_AddStringToObject(j, "cmd", "restart");
	arr = cJSON_AddArrayToObject(j, "daemons");
	if (!arr) {
		cJSON_Delete(j);
		return 1;
	}
	cJSON_AddItemToArray(arr, cJSON_CreateString(daemon));
	ok = cJSON_PrintPreallocated(j, json, sizeof(json), 0);
	cJSON_Delete(j);
	if (!ok)
		return 1;

	return send_cmd("rcd", json);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage(stderr);
		return 1;
	}

	if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
		usage(stdout);
		return 0;
	}

	if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0) {
		fprintf(stderr, "Raptor Streaming System — raptorctl [%s] built %s\n",
			rss_build_hash, rss_build_time);
		return 0;
	}

	if (strcmp(argv[1], "-j") == 0) {
		if (argc < 3) {
			fprintf(stderr, "Usage: raptorctl -j '<json>'\n");
			return 1;
		}
		return handle_json_mode(argv[2]);
	}

	if (strcmp(argv[1], "status") == 0) {
		cmd_status();
		return 0;
	}

	if (strcmp(argv[1], "memory") == 0) {
		cmd_memory();
		return 0;
	}

	if (strcmp(argv[1], "cpu") == 0) {
		cmd_cpu();
		return 0;
	}

	if (strcmp(argv[1], "config") == 0)
		return handle_config(argc, argv);

	/* raptorctl test-motion [duration_sec] — trigger RMR clip recording */
	if (strcmp(argv[1], "test-motion") == 0) {
		int dur = 10;
		if (argc >= 3)
			dur = (int)strtol(argv[2], NULL, 10);
		if (dur < 1)
			dur = 1;
		if (dur > 300)
			dur = 300;

		printf("triggering motion clip for %d seconds...\n", dur);
		int ret = send_cmd_verb("rmr", "start");
		if (ret != 0)
			return ret;
		sleep(dur);
		printf("stopping motion clip...\n");
		return send_cmd_verb("rmr", "stop");
	}

	if (!is_daemon(argv[1])) {
		fprintf(stderr, "Unknown daemon: %s\n", argv[1]);
		usage(stderr);
		return 1;
	}

	if (argc < 3) {
		/* raptorctl <daemon> — show status + available commands */
		int pid = rss_daemon_check(argv[1]);
		if (pid > 0)
			printf("%s: running (pid %d)\n", argv[1], pid);
		else
			printf("%s: stopped\n", argv[1]);

		daemon_help(argv[1]);
		return 0;
	}

	if (strcmp(argv[2], "start") == 0) {
		int pid = rss_daemon_check(argv[1]);
		if (pid > 0) {
			printf("%s: already running (pid %d)\n", argv[1], pid);
			return 0;
		}
		pid = fork();
		if (pid < 0) {
			perror("fork");
			return 1;
		}
		if (pid == 0) {
			/* Pass any extra args (e.g. -c /path/to/config) */
			char *child_argv[16];
			child_argv[0] = argv[1];
			int ci = 1;
			for (int i = 3; i < argc && ci < 15; i++)
				child_argv[ci++] = argv[i];
			child_argv[ci] = NULL;
			execvp(argv[1], child_argv);
			fprintf(stderr, "%s: exec failed: %s\n", argv[1], strerror(errno));
			_exit(127);
		}
		int status;
		waitpid(pid, &status, 0);
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
			usleep(100000);
			int new_pid = rss_daemon_check(argv[1]);
			if (new_pid > 0)
				printf("%s: started (pid %d)\n", argv[1], new_pid);
			else
				printf("%s: started\n", argv[1]);
			return 0;
		}
		fprintf(stderr, "%s: failed to start (exit %d)\n", argv[1],
			WIFEXITED(status) ? WEXITSTATUS(status) : -1);
		return 1;
	}

	if (strcmp(argv[2], "stop") == 0) {
		int pid = rss_daemon_check(argv[1]);
		if (pid <= 0) {
			fprintf(stderr, "%s: not running\n", argv[1]);
			return 1;
		}
		send_cmd_verb(argv[1], "shutdown");
		for (int i = 0; i < 20; i++) {
			usleep(100000);
			if (kill(pid, 0) != 0)
				return 0;
		}
		kill(pid, SIGTERM);
		usleep(200000);
		if (kill(pid, 0) != 0)
			return 0;
		fprintf(stderr, "%s: pid %d did not exit\n", argv[1], pid);
		return 1;
	}

	const char *daemon = argv[1];
	const char *cmd = argv[2];
	char json[512];

	/* The one verb that is ordered rather than sent; see above. */
	if (strcmp(daemon, "rvd") == 0 && strcmp(cmd, "restart") == 0)
		return restart_rvd_via_rcd(daemon);

	/* Privacy: send to both RVD (cover) and ROD (text element) */
	bool is_privacy = strcmp(daemon, "rod") == 0 && strcmp(cmd, "privacy") == 0;
	if (is_privacy)
		daemon = "rvd";

	/* Dispatch: try table, then generic fallback */
	int drc = dispatch_daemon_cmd(daemon, cmd, argc, argv, json, sizeof(json));
	if (drc == 2)
		return 0;
	if (drc < 0)
		return 1;
	if (drc == 0) {
		if (strncmp(cmd, "set-", 4) == 0 && argc >= 4) {
			if (build_generic_set(cmd, argc, argv, json, sizeof(json)) < 0)
				return 1;
		} else {
			cJSON *j = jcmd(cmd);
			if (!j)
				return 1;
			jstr(j, json, sizeof(json));
		}
	}

	int ret = send_cmd(daemon, json);

	/* Toggle ROD privacy text element visibility */
	if (is_privacy && ret == 0) {
		bool on = argc > 3 && strcmp(argv[3], "off") != 0;
		cJSON *j = cJSON_CreateObject();
		if (j) {
			cJSON_AddStringToObject(j, "cmd", on ? "show-element" : "hide-element");
			cJSON_AddStringToObject(j, "name", "privacy");
			char rod_json[128];
			cJSON_PrintPreallocated(j, rod_json, sizeof(rod_json), 0);
			cJSON_Delete(j);
			char rod_resp[256];
			rss_ctrl_send_command(RSS_RUN_DIR "/rod.sock", rod_json, rod_resp,
					      sizeof(rod_resp), 1000);
		}
	}

	return ret;
}
