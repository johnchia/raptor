/*
 * rmq_main.c -- Raptor MQTT bridge
 *
 * Holds one broker connection on behalf of the whole camera: a retained status
 * topic backed by a Last Will so the broker knows whether it is alive, a
 * periodic state document assembled from the per-daemon control sockets, and
 * one command topic that fans back out to those same sockets.
 *
 * One process rather than an MQTT client inside each daemon: the payloads are
 * untrusted network input, and raptor keeps that out of the address spaces
 * that own the hardware. What may cross from that input to a daemon is the
 * allowlist in rmq_cmd.c, which is the whole of the trust boundary.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>

#include "rmq.h"
#include "rmq_cmd.h"
#include "rmq_ha.h"
#include "rmq_mdns.h"
#include "rmq_restart.h"

#include <cJSON.h>

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

	/*
	 * Both left empty when unset rather than defaulted here, because an
	 * unset broker is what turns discovery on -- see resolve_broker(). Port
	 * 0 is the sentinel for "not configured"; no valid port is 0.
	 */
	rss_strlcpy(st->host, rss_config_get_str(c, "mqtt", "host", ""), sizeof(st->host));
	st->port = rss_config_get_int(c, "mqtt", "port", 0);
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
	snprintf(st->topic_cmd, sizeof(st->topic_cmd), "%s/cmd", st->topic_prefix);
	snprintf(st->topic_result, sizeof(st->topic_result), "%s/result", st->topic_prefix);
	snprintf(st->topic_snapshot, sizeof(st->topic_snapshot), "%s/snapshot", st->topic_prefix);

	/* Commands default on: this bridge is the camera's only management
	 * surface, so a read-only bridge is the unusual case rather than the
	 * safe default. Turning it off leaves the state and discovery topics
	 * exactly as they were. */
	st->commands_enabled = rss_config_get_bool(c, "mqtt", "commands", true);

	st->save_debounce_ms = rss_config_get_int(c, "mqtt", "save_debounce_ms", 3000);
	if (st->save_debounce_ms < 0)
		st->save_debounce_ms = 0;
	if (st->save_debounce_ms > RMQ_SAVE_MAX_DELAY_MS)
		st->save_debounce_ms = RMQ_SAVE_MAX_DELAY_MS;

	/* Longer than the save window: what it coalesces is a daemon restart,
	 * and commissioning a section key by key should cost one outage. */
	st->restart_debounce_ms = rss_config_get_int(c, "mqtt", "restart_debounce_ms", 5000);
	if (st->restart_debounce_ms < 0)
		st->restart_debounce_ms = 0;
	if (st->restart_debounce_ms > RMQ_RESTART_MAX_DELAY_MS)
		st->restart_debounce_ms = RMQ_RESTART_MAX_DELAY_MS;

	/*
	 * Stills. The sub stream by default because the size difference is not
	 * marginal — measured on a 2560x1920 sensor, the main JPEG is ~600 KB
	 * against the sub's ~6 KB, and a dashboard tile is displayed at a few
	 * hundred pixels either way.
	 */
	st->snapshot_enabled = rss_config_get_bool(c, "mqtt", "snapshot", false);
	st->snapshot_stream = rss_config_get_int(c, "mqtt", "snapshot_stream", 1);
	if (st->snapshot_stream < 0 || st->snapshot_stream >= RMQ_STREAM_COUNT)
		st->snapshot_stream = 1;
	st->snapshot_interval_sec = rss_config_get_int(c, "mqtt", "snapshot_interval", 0);
	if (st->snapshot_interval_sec < 0)
		st->snapshot_interval_sec = 0;

	/* rhd's credential, so the picture URL carries one when rhd demands
	 * one. Kept in step with the file by rmq_restart.c, which is the only
	 * thing that changes it after this point. */
	rmq_http_creds(st, c);

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

	/*
	 * Model reported to HA. The hostname, because on these images it is
	 * already the answer: S00hostname builds it as vendor-soc-unit, so
	 * openipc-ssc377qe-4825 names the SoC part more precisely than anything
	 * the running system can be asked for, and distinguishes two cameras of
	 * the same build where the platform alone cannot.
	 *
	 * The build platform remains the fallback for a host with no name set.
	 */
	char host[64] = "";
	if (gethostname(host, sizeof(host) - 1) == 0 && host[0])
		rss_strlcpy(st->model, host, sizeof(st->model));
	else
		rss_strlcpy(st->model, &rss_build_platform ? rss_build_platform : "raptor",
			    sizeof(st->model));

	st->poll_interval_sec = rss_config_get_int(c, "mqtt", "poll_interval", 10);
	if (st->poll_interval_sec < 1)
		st->poll_interval_sec = 1;
}

/*
 * Settle on a broker address. A configured host always wins and is never
 * second-guessed, so a camera that names its broker behaves exactly as it did
 * before discovery existed -- including costing no startup delay, because the
 * network is not asked at all.
 *
 * Only an unset host consults mDNS, and only once. Failing to find anything is
 * not an error: it falls through to the loopback default, which is what an
 * unconfigured camera used to get unconditionally.
 *
 * A configured port still wins over a discovered one. The pair is not taken
 * atomically because they are set for different reasons -- a port is pinned
 * when the broker listens somewhere unusual, and that stays true of whichever
 * host answers.
 */
static void resolve_broker(rmq_state_t *st)
{
	char addr[sizeof(st->host)];
	int port = 0;

	if (st->host[0]) {
		if (st->port == 0)
			st->port = RMQ_BROKER_PORT;
		return;
	}

	RSS_INFO("rmq: no broker configured, asking mDNS for %s", RMQ_MDNS_MQTT_TYPE);

	if (rmq_mdns_find_broker(addr, sizeof(addr), &port, RMQ_MDNS_DISCOVER_MS) == 0) {
		rss_strlcpy(st->host, addr, sizeof(st->host));
		st->host_discovered = true;
		if (st->port == 0)
			st->port = port;
		return;
	}

	RSS_INFO("rmq: nothing announced a broker, falling back to %s", RMQ_BROKER_FALLBACK);
	rss_strlcpy(st->host, RMQ_BROKER_FALLBACK, sizeof(st->host));
	if (st->port == 0)
		st->port = RMQ_BROKER_PORT;
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
				     "\"host_discovered\":%s,"
				     "\"port\":%d,\"tls\":%s,\"client_id\":\"%s\","
				     "\"topic_prefix\":\"%s\",\"commands\":%s,"
				     "\"save_pending\":%s,\"restart_pending\":%s,"
				     "\"staged_edits\":%d}",
				     rmq_mqtt_connected(st->mqtt) ? "true" : "false", st->host,
				     st->host_discovered ? "true" : "false", st->port,
				     st->use_tls ? "true" : "false", st->client_id,
				     st->topic_prefix, st->commands_enabled ? "true" : "false",
				     st->save_due_ms ? "true" : "false",
				     st->restart_due_ms ? "true" : "false", st->cfg_write_count);
	}

	/*
	 * Withdraw every discovery document and publish it again, which makes
	 * Home Assistant destroy the device and build it back from the current
	 * definitions. The reason to want that: `enabled_by_default` and the
	 * entity's original name are applied only when an entity is first
	 * created, so a camera discovered by an older build keeps whatever it
	 * was given then, however the table has changed since.
	 *
	 * Deliberately a command and not something a version change triggers.
	 * Recreating an entity discards what the user did to it — the rename,
	 * the area, the dashboard place — and that is a cost to accept
	 * knowingly, not to be handed by an upgrade.
	 */
	if (strcmp(cmd, "rediscover") == 0) {
		if (!st->ha_discovery)
			return rss_ctrl_resp_error(resp_buf, resp_buf_size,
						   "ha_discovery is disabled");
		if (!rmq_mqtt_connected(st->mqtt))
			return rss_ctrl_resp_error(resp_buf, resp_buf_size, "not connected");

		rmq_ha_clear_discovery(st);

		/* Republished by the next poll rather than here, so the entities
		 * come back alongside a state document rather than ahead of one
		 * and briefly unavailable. */
		st->discovery_published = false;
		return rss_ctrl_resp(resp_buf, resp_buf_size,
				     "{\"status\":\"ok\",\"cleared\":true,"
				     "\"republish_within_sec\":%d}",
				     st->poll_interval_sec);
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

	/* Before announcing availability, so the camera is never advertised
	 * as online during the window where a command would be missed. */
	if (rmq_cmd_subscribe(st) < 0)
		RSS_WARN("mqtt: subscribe to %s failed, commands will not arrive", st->topic_cmd);

	/* Retained so a subscriber connecting later still learns the state. */
	rmq_mqtt_publish(st->mqtt, st->topic_status, RMQ_STATUS_ONLINE, strlen(RMQ_STATUS_ONLINE),
			 1, true);

	/* Discovery is retained on the broker, but a reconnect may mean the
	 * broker itself restarted and lost it, so republish rather than
	 * assume. Forcing it also re-establishes entities after an HA restart
	 * that cleared them. */
	st->discovery_published = false;

	/* The picture URL names this connection's local address, which a
	 * reconnect may have changed — a lease renewal onto a different
	 * address, or a second interface taking over the route to the broker.
	 * Republished rather than assumed for the same reason discovery is. */
	st->snapshot_next_ms = 0;
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
	cJSON *state = rmq_poll_state(st, &daemons);
	if (!state)
		return;

	/* Before discovery, not after: the entity definitions are built from
	 * this, and on the first cycle the camera's answers and the discovery
	 * document arrive together. Read afterwards, the camera would advertise
	 * an empty resolution list and every image control until something
	 * unrelated forced a republish. */
	bool camera_changed = rmq_ha_note_camera(st, state);

	if (st->ha_discovery) {
		bool changed = !st->discovery_published || camera_changed ||
			       rmq_daemons_differ(&daemons, &st->last_daemons);
		if (changed) {
			const rmq_daemons_t *prev =
				st->discovery_published ? &st->last_daemons : NULL;
			if (rmq_ha_publish_discovery(st, &daemons, prev) == 0) {
				st->last_daemons = daemons;
				st->discovery_published = true;
			}
		}
	}

	/* The bridge's own state, added to the daemons': a pending restart is
	 * the reason a control can read back the value it had a moment ago. */
	rmq_restart_report(st, state);
	rmq_system_report(st, state);

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

		if (rmq_snapshot_due(st, now))
			rmq_snapshot_publish(st);

		if (st->save_due_ms && now >= st->save_due_ms)
			rmq_cmd_flush_saves(st);

		if (rmq_restart_due(st, now)) {
			/*
			 * This blocks for as long as the slowest daemon takes
			 * to come back — tens of seconds for rvd. That is
			 * within the keepalive, and serialising it against the
			 * poll is deliberate: state collected mid-restart
			 * describes a camera that does not exist yet.
			 */
			rmq_restart_apply(st);
			next_poll_ms = 0; /* publish the settled state at once */
		}
	}
}

static void on_message(const char *topic, const uint8_t *payload, size_t len, void *user)
{
	rmq_cmd_handle(user, topic, payload, len);
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

	/* After the enabled check: a disabled bridge must not spend three
	 * seconds on the network before deciding to exit. */
	resolve_broker(&st);

	st.mqtt = rmq_mqtt_new();
	if (!st.mqtt) {
		RSS_FATAL("failed to allocate mqtt client");
		rss_config_free(dctx.cfg);
		rss_daemon_cleanup("rmq");
		return 1;
	}

	rmq_mqtt_set_message_cb(st.mqtt, on_message, &st);

	RSS_INFO("rmq: broker %s:%d%s, client '%s', prefix '%s'", st.host, st.port,
		 st.host_discovered ? " (discovered)" : "", st.client_id, st.topic_prefix);
	if (st.commands_enabled)
		RSS_INFO("rmq: commands accepted on %s, results on %s", st.topic_cmd,
			 st.topic_result);
	else
		RSS_INFO("rmq: commands disabled, read-only bridge");

	rss_mkdir_p(RSS_RUN_DIR);
	st.ctrl = rss_ctrl_listen(RSS_RUN_DIR "/rmq.sock");
	if (!st.ctrl)
		RSS_WARN("control socket failed (non-fatal)");

	serve_loop(&st);

	RSS_INFO("rmq shutting down");

	/* A change made moments before the stop is still owed to flash, and
	 * the daemons that hold it may be going down with us. */
	rmq_cmd_flush_saves(&st);
	rmq_restart_flush_writes(&st);

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
