/*
 * rmq_cmd.h -- The command topic
 *
 * A transport, not a policy. What may be commanded and what a value may be is
 * rcd's table; this file size-checks the payload, keeps the bookkeeping MQTT
 * needs that no other client has -- the nonce, the result topic, the fact that
 * a reply has nowhere to go once the camera has rebooted -- and hands the rest
 * over.
 *
 * Three commands stay here because rcd does not own them: the picture, which
 * is a URL this bridge publishes; the reboot, which has to take the retained
 * status offline before it goes; and the /etc settings, which are not in
 * raptor.conf.
 */

#ifndef RMQ_CMD_H
#define RMQ_CMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct rmq_state;

/* A command is a few dozen bytes; anything approaching this is not one. */
#define RMQ_PAYLOAD_MAX 4096

/* Echoed verbatim into the result, so it is bounded like any other input. */
#define RMQ_NONCE_MAX 64

/* Subscribe to the command topic. Safe to call again after a reconnect. */
int rmq_cmd_subscribe(struct rmq_state *st);

/* Handle one message from the command topic. */
void rmq_cmd_handle(struct rmq_state *st, const char *topic, const uint8_t *payload, size_t len);

#endif /* RMQ_CMD_H */
