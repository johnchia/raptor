/*
 * mdnsprobe.c -- command-line front end to rmq's DNS-SD browse
 *
 * The browse runs inside rmq, where nothing prints and a wrong answer shows up
 * later as a broker that will not connect. This exposes the same code path so
 * it can be pointed at a network and read directly, which is what makes the
 * two-camera and empty-network cases checkable at all.
 *
 * Deliberately the same translation unit rmq links, not a reimplementation:
 * a probe that agrees with a separate copy of the logic proves nothing about
 * the daemon.
 */

#include "rmq_mdns.h"

#include <rss_common.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PROBE_MAX 16

static int usage(int code)
{
	fprintf(stderr, "usage: mdnsprobe [-b] [-t SECONDS] [-v] [TYPE]\n"
			"\n"
			"  -b          resolve one broker the way rmq would, and print it\n"
			"  -t SECONDS  how long to listen (default 3)\n"
			"  -v          log at debug level\n"
			"\n"
			"TYPE is a fully-qualified DNS-SD type including the trailing dot.\n"
			"Default: " RMQ_MDNS_MQTT_TYPE "\n"
			"\n"
			"Examples:\n"
			"  mdnsprobe                       # who offers MQTT here?\n"
			"  mdnsprobe _rtsp._tcp.local.     # every camera on the LAN\n"
			"  mdnsprobe -b                    # just the broker rmq would pick\n");
	return code;
}

int main(int argc, char *argv[])
{
	rmq_mdns_service_t svc[PROBE_MAX];
	const char *type = RMQ_MDNS_MQTT_TYPE;
	rss_log_level_t level = RSS_LOG_INFO;
	int broker_only = 0;
	int seconds = 3;
	int n, i, c, resolved;

	while ((c = getopt(argc, argv, "bht:v?")) != -1) {
		switch (c) {
		case 'b':
			broker_only = 1;
			break;
		case 't':
			seconds = atoi(optarg);
			if (seconds <= 0 || seconds > 60) {
				fprintf(stderr, "mdnsprobe: -t must be 1..60 seconds\n");
				return 1;
			}
			break;
		case 'v':
			level = RSS_LOG_DEBUG;
			break;
		default:
			return usage(c == 'h' || c == '?' ? 0 : 1);
		}
	}

	if (optind < argc)
		type = argv[optind];

	rss_log_init("mdnsprobe", level, RSS_LOG_TARGET_STDERR, NULL);

	if (broker_only) {
		char host[64] = "";
		int port = 0;

		if (rmq_mdns_find_broker(host, sizeof(host), &port, (unsigned)seconds * 1000) !=
		    0) {
			printf("no broker found\n");
			return 1;
		}
		printf("%s %d\n", host, port);
		return 0;
	}

	printf("browsing %s for %ds ...\n", type, seconds);

	n = rmq_mdns_browse(type, (unsigned)seconds * 1000, svc, PROBE_MAX);
	if (n < 0) {
		fprintf(stderr, "mdnsprobe: browse failed\n");
		return 1;
	}
	if (n == 0) {
		printf("nothing announced %s\n", type);
		return 0;
	}

	resolved = 0;
	for (i = 0; i < n; i++) {
		const rmq_mdns_service_t *s = &svc[i];

		printf("\n%s\n", s->instance);

		/* The meta-query returns bare type names with nothing behind
		 * them, so printing empty host and address fields would read
		 * as a failed lookup rather than a complete answer. */
		if (!s->host[0] && !s->port && !s->txt[0])
			continue;

		printf("  host  %s\n", s->host[0] ? s->host : "(no SRV)");
		if (s->resolved)
			printf("  addr  %s:%u\n", s->addr, (unsigned)s->port);
		else
			printf("  addr  (unresolved)\n");
		if (s->txt[0])
			printf("  txt   %s\n", s->txt);
		resolved += s->resolved;
	}

	printf("\n%d found, %d resolved\n", n, resolved);

	return 0;
}
