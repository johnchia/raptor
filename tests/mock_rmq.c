/*
 * mock_rmq.c -- Stand-ins for rmq's transport and daemon table
 *
 * test_rmq_cmd.c exercises rmq_cmd_plan(), which is pure: it decides what may
 * cross into a daemon and builds the request, touching neither the broker nor
 * a control socket. These exist only so the object file links. Everything
 * they stand in for is I/O against a live camera and is verified on hardware.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../rmq/rmq_mqtt.h"
#include "../rmq/rmq_poll.h"
#include "../rmq/rmq_restart.h"

int rmq_mqtt_publish(rmq_mqtt_t *m, const char *topic, const void *payload, size_t len, uint8_t qos,
		     bool retain)
{
	(void)m;
	(void)topic;
	(void)payload;
	(void)len;
	(void)qos;
	(void)retain;
	return 0;
}

int rmq_mqtt_subscribe(rmq_mqtt_t *m, const char *filter, uint8_t qos)
{
	(void)m;
	(void)filter;
	(void)qos;
	return 0;
}

const char *rmq_daemon_name(rmq_daemon_t d)
{
	(void)d;
	return "?";
}

rmq_daemon_t rmq_daemon_by_name(const char *name)
{
	(void)name;
	return RMQ_D_COUNT;
}

void rmq_restart_stage(struct rmq_state *st, const rmq_cfg_write_t *w, rmq_daemon_t owner)
{
	(void)st;
	(void)w;
	(void)owner;
}

int rmq_snapshot_capture(struct rmq_state *st)
{
	(void)st;
	return 0;
}
