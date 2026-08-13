/*
 * rmq_main.c -- Raptor MQTT bridge
 *
 * Holds one broker connection on behalf of the whole camera and, in later
 * phases, fans commands out to the per-daemon control sockets. This first
 * phase establishes only presence: a retained status topic plus a Last Will,
 * so the broker knows whether the camera is alive. It subscribes to nothing
 * and accepts no commands, so there is no path from the network to any daemon.
 *
 * One process rather than an MQTT client inside each daemon: the payloads are
 * untrusted network input, and raptor keeps that out of the address space that
 * owns the hardware.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>

#include "rmq.h"
#include "rmq_ha.h"

#include <cJSON.h>

#define RMQ_STATUS_ONLINE  "online"
#define RMQ_STATUS_OFFLINE "offline"

/* ------------------------------------------------------------------ */
/* Identity                                                            */
/* ------------------------------------------------------------------ */

/*
 * MACs that are placeholders rather than per-unit addresses. OpenIPC's
 * interfaces.d/eth0 falls back to a literal when the U-Boot environment has
 * no ethaddr, and images have been seen shipping that literal *in* the
 * environment — so every camera from such an image shares one MAC.
 *
 * That matters more here than it looks: MQTT client ids must be unique, and a
 * broker evicts the existing session when a second client connects with the
 * same id (§3.1.4). Two such cameras take turns kicking each other off, with
 * their retained status flapping, which is a miserable thing to diagnose from
 * the symptom. Warn at startup instead.
 */
static bool mac_is_placeholder(const char *hex12)
{
	static const char *known[] = {
		"000023344566", /* OpenIPC interfaces.d/eth0 fallback */
		"000000000000",
		"ffffffffffff",
		NULL,
	};

	for (int i = 0; known[i]; i++) {
		if (strcmp(hex12, known[i]) == 0)
			return true;
	}
	return false;
}

/*
 * Derive a stable client id from the primary MAC. Falls back to the hostname
 * so a board with an unexpected interface name still gets a usable identity
 * rather than refusing to start.
 */
static void derive_client_id(char *out, size_t outsz)
{
	static const char *ifaces[] = {"eth0", "eth1", "wlan0", NULL};

	for (int i = 0; ifaces[i]; i++) {
		char path[128];
		snprintf(path, sizeof(path), "/sys/class/net/%s/address", ifaces[i]);

		int len = 0;
		char *mac = rss_read_file(path, &len);
		if (!mac)
			continue;

		char hex[16];
		int n = 0;
		for (int j = 0; j < len && n < 12; j++) {
			char c = mac[j];
			if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
				hex[n++] = c;
			else if (c >= 'A' && c <= 'F')
				hex[n++] = (char)(c - 'A' + 'a');
		}
		free(mac);

		if (n == 12) {
			hex[n] = '\0';
			if (mac_is_placeholder(hex))
				RSS_WARN("mqtt: %s carries placeholder MAC %s, so this client id "
					 "is not unique — a second camera from the same image "
					 "will evict this one from the broker. Provision a real "
					 "MAC (fw_setenv ethaddr) or set [mqtt] client_id",
					 ifaces[i], hex);
			snprintf(out, outsz, "raptor-%s", hex);
			return;
		}
	}

	char host[64] = "";
	if (gethostname(host, sizeof(host) - 1) == 0 && host[0])
		snprintf(out, outsz, "raptor-%s", host);
	else
		snprintf(out, outsz, "raptor-unknown");
}

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */

static void load_config(rmq_state_t *st)
{
	rss_config_t *c = st->cfg;

	rss_strlcpy(st->host, rss_config_get_str(c, "mqtt", "host", "127.0.0.1"), sizeof(st->host));
	st->port = rss_config_get_int(c, "mqtt", "port", 1883);
	rss_strlcpy(st->username, rss_config_get_str(c, "mqtt", "username", ""),
		    sizeof(st->username));
	rss_strlcpy(st->password, rss_config_get_str(c, "mqtt", "password", ""),
		    sizeof(st->password));
	st->use_tls = rss_config_get_bool(c, "mqtt", "tls", false);
	st->keepalive_sec = rss_config_get_int(c, "mqtt", "keepalive", 60);
	st->reconnect_delay_ms = rss_config_get_int(c, "mqtt", "reconnect_delay_ms", 5000);

	const char *cid = rss_config_get_str(c, "mqtt", "client_id", "");
	if (cid && cid[0])
		rss_strlcpy(st->client_id, cid, sizeof(st->client_id));
	else
		derive_client_id(st->client_id, sizeof(st->client_id));

	const char *prefix = rss_config_get_str(c, "mqtt", "topic_prefix", "");
	if (prefix && prefix[0])
		rss_strlcpy(st->topic_prefix, prefix, sizeof(st->topic_prefix));
	else
		snprintf(st->topic_prefix, sizeof(st->topic_prefix), "raptor/%s", st->client_id);

	snprintf(st->topic_status, sizeof(st->topic_status), "%s/status", st->topic_prefix);
	snprintf(st->topic_state, sizeof(st->topic_state), "%s/state", st->topic_prefix);

	/* Home Assistant discovery */
	st->ha_discovery = rss_config_get_bool(c, "mqtt", "ha_discovery", true);
	rss_strlcpy(st->discovery_prefix,
		    rss_config_get_str(c, "mqtt", "discovery_prefix", "homeassistant"),
		    sizeof(st->discovery_prefix));
	snprintf(st->topic_discovery, sizeof(st->topic_discovery), "%s/device/%s/config",
		 st->discovery_prefix, st->client_id);

	const char *dname = rss_config_get_str(c, "mqtt", "device_name", "");
	if (dname && dname[0])
		rss_strlcpy(st->device_name, dname, sizeof(st->device_name));
	else
		rss_strlcpy(st->device_name, st->client_id, sizeof(st->device_name));

	/* Model reported to HA. The build platform is the honest answer and
	 * costs nothing, being compiled in already. */
	rss_strlcpy(st->model, &rss_build_platform ? rss_build_platform : "raptor",
		    sizeof(st->model));

	st->poll_interval_sec = rss_config_get_int(c, "mqtt", "poll_interval", 10);
	if (st->poll_interval_sec < 1)
		st->poll_interval_sec = 1;
}

/* ------------------------------------------------------------------ */
/* Control socket                                                      */
/* ------------------------------------------------------------------ */

static int rmq_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	rmq_state_t *st = userdata;

	int common =
		rss_ctrl_handle_common(cmd_json, resp_buf, resp_buf_size, st->cfg, st->config_path);
	if (common >= 0)
		return common;

	char cmd[64];
	if (rss_json_get_str(cmd_json, "cmd", cmd, sizeof(cmd)) != 0)
		return rss_ctrl_resp_error(resp_buf, resp_buf_size, "missing cmd");

	if (strcmp(cmd, "status") == 0 || strcmp(cmd, "config-show") == 0) {
		return rss_ctrl_resp(resp_buf, resp_buf_size,
				     "{\"status\":\"ok\",\"connected\":%s,\"host\":\"%s\","
				     "\"port\":%d,\"tls\":%s,\"client_id\":\"%s\","
				     "\"topic_prefix\":\"%s\"}",
				     rmq_mqtt_connected(st->mqtt) ? "true" : "false", st->host,
				     st->port, st->use_tls ? "true" : "false", st->client_id,
				     st->topic_prefix);
	}

	return rss_ctrl_resp_error(resp_buf, resp_buf_size, "unknown command");
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

static int rmq_connect(rmq_state_t *st)
{
	rmq_mqtt_opts_t opts = {
		.host = st->host,
		.port = st->port,
		.client_id = st->client_id,
		.username = st->username,
		.password = st->password,
		.use_tls = st->use_tls,
		.keepalive_sec = st->keepalive_sec,
		.connect_timeout_ms = 10000,
		/* The will is what makes an ungraceful death visible: power
		 * loss or SIGKILL leaves no chance to publish anything. */
		.will_topic = st->topic_status,
		.will_payload = RMQ_STATUS_OFFLINE,
		.will_qos = 1,
		.will_retain = true,
	};

	if (rmq_mqtt_connect(st->mqtt, &opts) < 0)
		return -1;

	/* Retained so a subscriber connecting later still learns the state. */
	rmq_mqtt_publish(st->mqtt, st->topic_status, RMQ_STATUS_ONLINE, strlen(RMQ_STATUS_ONLINE),
			 1, true);

	/* Discovery is retained on the broker, but a reconnect may mean the
	 * broker itself restarted and lost it, so republish rather than
	 * assume. Forcing it also re-establishes entities after an HA restart
	 * that cleared them. */
	st->discovery_published = false;
	return 0;
}

/*
 * One poll cycle: collect state, publish it, and republish discovery if the
 * set of running daemons changed. Discovery is deliberately driven by the
 * poll rather than by a timer of its own, so entities can never describe a
 * daemon set the state document does not match.
 */
static void rmq_poll_cycle(rmq_state_t *st)
{
	rmq_daemons_t daemons;
	cJSON *state = rmq_poll_state(&daemons);
	if (!state)
		return;

	if (st->ha_discovery) {
		bool changed =
			!st->discovery_published || rmq_daemons_differ(&daemons, &st->last_daemons);
		if (changed) {
			const rmq_daemons_t *prev =
				st->discovery_published ? &st->last_daemons : NULL;
			if (rmq_ha_publish_discovery(st, &daemons, prev) == 0) {
				st->last_daemons = daemons;
				st->discovery_published = true;
			}
		}
	}

	char *payload = cJSON_PrintUnformatted(state);
	cJSON_Delete(state);
	if (!payload)
		return;

	rmq_mqtt_publish(st->mqtt, st->topic_state, payload, strlen(payload), 0, true);
	free(payload);
}

static void serve_loop(rmq_state_t *st)
{
	int ctrl_fd = st->ctrl ? rss_ctrl_get_fd(st->ctrl) : -1;
	int reconnect_wait = 0;
	uint64_t next_poll_ms = 0; /* first cycle runs immediately */

	while (rss_running(st->running)) {
		if (ctrl_fd >= 0) {
			fd_set fds;
			struct timeval tv = {0, 0};
			FD_ZERO(&fds);
			FD_SET(ctrl_fd, &fds);
			if (select(ctrl_fd + 1, &fds, NULL, NULL, &tv) > 0)
				rss_ctrl_accept_and_handle(st->ctrl, rmq_ctrl_handler, st);
		}

		if (!rmq_mqtt_connected(st->mqtt)) {
			if (reconnect_wait > 0) {
				usleep(200000);
				reconnect_wait -= 200;
				continue;
			}
			if (rmq_connect(st) < 0) {
				reconnect_wait = st->reconnect_delay_ms;
				RSS_WARN("mqtt: reconnecting in %d ms", reconnect_wait);
				continue;
			}
			reconnect_wait = 0;
		}

		/* Short timeout so the control socket stays responsive. */
		if (rmq_mqtt_loop(st->mqtt, 200) < 0) {
			RSS_WARN("mqtt: connection lost");
			rmq_mqtt_disconnect(st->mqtt);
			reconnect_wait = st->reconnect_delay_ms;
			continue;
		}

		/* Poll on a wall-clock deadline rather than counting loop
		 * iterations, which would drift with however long the daemon
		 * round trips happen to take. */
		uint64_t now = (uint64_t)(rss_timestamp_us() / 1000);
		if (now >= next_poll_ms) {
			rmq_poll_cycle(st);
			next_poll_ms = now + (uint64_t)st->poll_interval_sec * 1000;
		}
	}
}

int main(int argc, char **argv)
{
	rss_daemon_ctx_t dctx;
	int ret = rss_daemon_init(&dctx, "rmq", argc, argv, NULL);
	if (ret != 0)
		return ret > 0 ? 0 : 1;

	rmq_state_t st;
	memset(&st, 0, sizeof(st));
	st.cfg = dctx.cfg;
	st.config_path = dctx.config_path;
	st.running = dctx.running;

	load_config(&st);

	if (!rss_config_get_bool(st.cfg, "mqtt", "enabled", false)) {
		RSS_INFO("mqtt disabled in config, exiting");
		rss_config_free(dctx.cfg);
		rss_daemon_cleanup("rmq");
		return 0;
	}

	st.mqtt = rmq_mqtt_new();
	if (!st.mqtt) {
		RSS_FATAL("failed to allocate mqtt client");
		rss_config_free(dctx.cfg);
		rss_daemon_cleanup("rmq");
		return 1;
	}

	RSS_INFO("rmq: broker %s:%d, client '%s', prefix '%s'", st.host, st.port, st.client_id,
		 st.topic_prefix);

	rss_mkdir_p(RSS_RUN_DIR);
	st.ctrl = rss_ctrl_listen(RSS_RUN_DIR "/rmq.sock");
	if (!st.ctrl)
		RSS_WARN("control socket failed (non-fatal)");

	serve_loop(&st);

	RSS_INFO("rmq shutting down");

	/* A graceful DISCONNECT tells the broker to discard the will, so the
	 * retained status would otherwise stay "online" forever. Publish the
	 * offline state ourselves before going. */
	if (rmq_mqtt_connected(st.mqtt))
		rmq_mqtt_publish(st.mqtt, st.topic_status, RMQ_STATUS_OFFLINE,
				 strlen(RMQ_STATUS_OFFLINE), 1, true);

	rmq_mqtt_free(st.mqtt);

	if (st.ctrl)
		rss_ctrl_destroy(st.ctrl);

	rss_config_free(dctx.cfg);
	rss_daemon_cleanup("rmq");

	return 0;
}
