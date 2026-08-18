/*
 * rcd_system.h -- Keys whose store is a file in /etc rather than raptor.conf
 *
 * The rest of rcd's table describes lines in raptor.conf, read back by the
 * daemon that owns the section. These describe the camera itself, which has no
 * owning daemon and no line in that file: the timezone is /etc/TZ, the time
 * server is /etc/ntp.conf. A key names one of the providers below and rcd
 * reads and writes through it instead of through the config file.
 *
 * Everything else about such a key is unchanged -- it is validated against the
 * same table, batched into the same `set`, and described by the same `schema`,
 * so a client renders it without knowing where it is kept.
 */

#ifndef RCD_SYSTEM_H
#define RCD_SYSTEM_H

#include "rcd_schema.h"

/*
 * Where these files live. Overridable at compile time so the tests can point
 * the providers at a scratch directory: they exercise the real writers, and
 * the real writers replace /etc/TZ.
 */
#ifndef RCD_SYSCONF_DIR
#define RCD_SYSCONF_DIR "/etc"
#endif

/*
 * The timezone names, NULL-terminated, in the order the generator emitted
 * them. This is the key's `choices` array as well as the lookup table, so a
 * name the schema offers and a name `set` accepts cannot drift apart.
 */
extern const char *const rcd_zone_names[];

/* The POSIX TZ rule for a name from that list, or NULL if it is not one. */
const char *rcd_zone_posix(const char *name);

extern const rcd_provider_t rcd_provider_timezone;
extern const rcd_provider_t rcd_provider_ntp_server;

#endif /* RCD_SYSTEM_H */
