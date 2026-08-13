/*
 * rmq_mqtt.h -- MQTT 3.1.1 client
 *
 * The subset needed to bridge a camera to a broker: CONNECT, PUBLISH,
 * SUBSCRIBE, DISCONNECT, over plain TCP or TLS.
 *
 * This header is the seam. Nothing above it knows how packets are framed, so
 * the client underneath can be replaced without disturbing callers — as it
 * once was, from a hand-rolled codec to libmosquitto.
 */

#ifndef RMQ_MQTT_H
#define RMQ_MQTT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct rmq_mqtt rmq_mqtt_t;

/* CONNACK return codes (MQTT 3.1.1 §3.2.2.3). */
typedef enum {
	RMQ_CONNACK_ACCEPTED = 0,
	RMQ_CONNACK_BAD_PROTOCOL = 1,
	RMQ_CONNACK_ID_REJECTED = 2,
	RMQ_CONNACK_UNAVAILABLE = 3,
	RMQ_CONNACK_BAD_CREDENTIALS = 4,
	RMQ_CONNACK_NOT_AUTHORIZED = 5,
} rmq_connack_t;

typedef struct {
	const char *host;
	int port;
	const char *client_id;
	const char *username; /* NULL or "" for anonymous */
	const char *password;
	bool use_tls;
	int keepalive_sec; /* 0 disables keepalive entirely */
	int connect_timeout_ms;

	/* Last Will and Testament. Published by the broker if this client
	 * drops without a DISCONNECT, which is the only way an ungraceful
	 * death (power loss, SIGKILL) is ever reported. NULL topic = no will. */
	const char *will_topic;
	const char *will_payload;
	uint8_t will_qos;
	bool will_retain;
} rmq_mqtt_opts_t;

/* Invoked from rmq_mqtt_loop() for each inbound PUBLISH. The payload is not
 * NUL-terminated and is only valid for the duration of the call. */
typedef void (*rmq_mqtt_on_message_fn)(const char *topic, const uint8_t *payload, size_t len,
				       void *user);

rmq_mqtt_t *rmq_mqtt_new(void);
void rmq_mqtt_free(rmq_mqtt_t *m);

void rmq_mqtt_set_message_cb(rmq_mqtt_t *m, rmq_mqtt_on_message_fn cb, void *user);

/*
 * Connect and wait for CONNACK. Returns 0 on success, or -1 with the broker's
 * CONNACK code available via rmq_mqtt_last_connack() when the failure was a
 * rejection rather than a transport error.
 */
int rmq_mqtt_connect(rmq_mqtt_t *m, const rmq_mqtt_opts_t *opts);
void rmq_mqtt_disconnect(rmq_mqtt_t *m);
bool rmq_mqtt_connected(const rmq_mqtt_t *m);
int rmq_mqtt_last_connack(const rmq_mqtt_t *m);

int rmq_mqtt_publish(rmq_mqtt_t *m, const char *topic, const void *payload, size_t len, uint8_t qos,
		     bool retain);
int rmq_mqtt_subscribe(rmq_mqtt_t *m, const char *filter, uint8_t qos);

/*
 * Pump I/O: wait up to timeout_ms for readable data, dispatch whatever
 * arrives, and send a PINGREQ when keepalive requires one. Returns 0 on
 * success, -1 if the connection was lost (caller should reconnect).
 */
int rmq_mqtt_loop(rmq_mqtt_t *m, int timeout_ms);

int rmq_mqtt_get_fd(const rmq_mqtt_t *m);

#endif /* RMQ_MQTT_H */
