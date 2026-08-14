/*
 * rmq_system.h -- the settings that live in /etc rather than in raptor.conf
 *
 * Two of them: the timezone and the NTP server. Everything else the plan
 * listed is out of scope — DNS is written by the DHCP client and would be
 * overwritten at the next lease, the hostname is generated from the MAC
 * elsewhere, and static addressing is a wish-list item. What is left is the
 * pair that a camera actually needs set at install and cannot get from DHCP
 * on this network.
 *
 * Neither can lock anyone out, which is why there is no commit-confirm here.
 * A wrong timezone displays the wrong time and a wrong NTP server leaves the
 * clock where it was; both are reachable and reversible over the same bridge
 * that set them. That stops being true the moment an address or a broker host
 * is exposed, and the confirm timer belongs with those rather than here.
 */

#ifndef RMQ_SYSTEM_H
#define RMQ_SYSTEM_H

#include <stdbool.h>
#include <stddef.h>

#include <cJSON.h>

struct rmq_state;

/*
 * Timezones are a closed list, not free text.
 *
 * There is no /usr/share/zoneinfo in this image, so the kernel of the matter
 * is a POSIX TZ string — `EST5EDT,M3.2.0,M11.1.0` — which carries its own DST
 * rules and is not something anyone should be asked to type. The list maps a
 * name people recognise to the string musl needs, which also keeps the
 * writable surface an enum: no free text reaches /etc/TZ.
 */
const char *const *rmq_system_zone_names(void);

/* The POSIX TZ string for a name from the list above, or NULL if unknown. */
const char *rmq_system_zone_posix(const char *name);

/*
 * Apply. Both write their file and return 0 on success.
 *
 * The timezone takes effect on **reboot**, not now: /etc/init.d/rcS exports TZ
 * once at boot, and a raptor daemon restarts by re-execing itself, which keeps
 * the environment it already had. Writing the file is all that can be done
 * from here, so the entity says so and the state document reports it.
 */
int rmq_system_set_timezone(const char *zone_name);

/* Takes effect immediately — ntpd is restarted, which costs nothing. */
int rmq_system_set_ntp_server(const char *host);

/* True if `host` is a plausible hostname or IPv4 address. */
bool rmq_system_valid_host(const char *host);

/* Current values into the state document, under "system". */
void rmq_system_report(const struct rmq_state *st, cJSON *state);

#endif /* RMQ_SYSTEM_H */
