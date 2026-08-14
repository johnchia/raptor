/*
 * rmq_system.c -- see rmq_system.h
 */

#include "rmq_system.h"
#include "rmq.h"

#include <rss_common.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PATH_TZ	      "/etc/TZ"
#define PATH_TIMEZONE "/etc/timezone"
#define PATH_NTP      "/etc/ntp.conf"

/*
 * Names on the left are what an operator picks; the POSIX string on the right
 * is what musl parses. The DST rules are part of the string because there is
 * no zoneinfo database on the camera to look them up in — which also means
 * this table ages: a country that moves its DST changeover needs the string
 * updated here, the same way tzdata would need updating anywhere else.
 *
 * Deliberately short. It covers the zones a fleet is plausibly installed in
 * rather than all ~350, because every entry is a row in a dropdown someone has
 * to scroll past.
 */
static const struct {
	const char *name;
	const char *posix;
} zones[] = {
	{"UTC", "GMT0"},
	{"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},
	{"Europe/Dublin", "GMT0IST,M3.5.0/1,M10.5.0"},
	{"Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0"},
	{"Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3"},
	{"Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
	{"Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3"},
	{"Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3"},
	{"Europe/Amsterdam", "CET-1CEST,M3.5.0,M10.5.0/3"},
	{"Europe/Warsaw", "CET-1CEST,M3.5.0,M10.5.0/3"},
	{"Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
	{"Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
	{"Europe/Kyiv", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
	{"Europe/Moscow", "MSK-3"},
	{"Africa/Lagos", "WAT-1"},
	{"Africa/Cairo", "EET-2EEST,M4.5.5/0,M10.5.4/24"},
	{"Africa/Johannesburg", "SAST-2"},
	{"Africa/Nairobi", "EAT-3"},
	{"Asia/Jerusalem", "IST-2IDT,M3.4.4/26,M10.5.0"},
	{"Asia/Dubai", "<+04>-4"},
	{"Asia/Karachi", "PKT-5"},
	{"Asia/Kolkata", "IST-5:30"},
	{"Asia/Bangkok", "<+07>-7"},
	{"Asia/Jakarta", "WIB-7"},
	{"Asia/Shanghai", "CST-8"},
	{"Asia/Hong_Kong", "HKT-8"},
	{"Asia/Singapore", "<+08>-8"},
	{"Asia/Manila", "PST-8"},
	{"Asia/Seoul", "KST-9"},
	{"Asia/Tokyo", "JST-9"},
	{"Australia/Perth", "AWST-8"},
	{"Australia/Brisbane", "AEST-10"},
	{"Australia/Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
	{"Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
	{"Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
	{"Pacific/Honolulu", "HST10"},
	{"America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0"},
	{"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
	{"America/Phoenix", "MST7"},
	{"America/Denver", "MST7MDT,M3.2.0,M11.1.0"},
	{"America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
	{"America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
	{"America/Halifax", "AST4ADT,M3.2.0,M11.1.0"},
	{"America/Sao_Paulo", "<-03>3"},
	{"America/Argentina/Buenos_Aires", "<-03>3"},
	{"America/Bogota", "<-05>5"},
	{"America/Mexico_City", "CST6"},
};

#define ZONE_COUNT ((int)(sizeof(zones) / sizeof(zones[0])))

/* Built once, NULL-terminated, because the enum validator wants that shape. */
static const char *zone_names[ZONE_COUNT + 1];

const char *const *rmq_system_zone_names(void)
{
	if (!zone_names[0]) {
		for (int i = 0; i < ZONE_COUNT; i++)
			zone_names[i] = zones[i].name;
		zone_names[ZONE_COUNT] = NULL;
	}
	return zone_names;
}

const char *rmq_system_zone_posix(const char *name)
{
	for (int i = 0; i < ZONE_COUNT; i++) {
		if (strcmp(name, zones[i].name) == 0)
			return zones[i].posix;
	}
	return NULL;
}

/* The reverse, for reporting what the camera is actually set to. Several names
 * share a POSIX string — every Central European zone is the same string — so
 * this returns the first, which is why the reported zone can differ from the
 * one that was chosen while describing the same clock. */
static const char *zone_name_for_posix(const char *posix)
{
	for (int i = 0; i < ZONE_COUNT; i++) {
		if (strcmp(posix, zones[i].posix) == 0)
			return zones[i].name;
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Files                                                               */
/* ------------------------------------------------------------------ */

/* First line of a file, newline stripped. Empty string if unreadable. */
static void read_line(const char *path, char *out, size_t outsz)
{
	out[0] = '\0';

	FILE *f = fopen(path, "r");
	if (!f)
		return;
	if (fgets(out, (int)outsz, f)) {
		size_t n = strlen(out);
		while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
			out[--n] = '\0';
	}
	fclose(f);
}

/*
 * Write via a temporary and rename, so a power cut during the write leaves the
 * old file rather than a truncated one. /etc is an overlay over NOR here, and
 * a half-written /etc/TZ is a camera that will not boot with a clock.
 */
static int write_file(const char *path, const char *content)
{
	char tmp[128];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);

	FILE *f = fopen(tmp, "w");
	if (!f) {
		RSS_WARN("system: cannot write %s: %s", tmp, strerror(errno));
		return -1;
	}
	bool ok = fputs(content, f) >= 0;
	if (fclose(f) != 0)
		ok = false;
	if (!ok) {
		unlink(tmp);
		return -1;
	}

	if (rename(tmp, path) != 0) {
		RSS_WARN("system: cannot replace %s: %s", path, strerror(errno));
		unlink(tmp);
		return -1;
	}
	return 0;
}

int rmq_system_set_timezone(const char *zone_name)
{
	const char *posix = rmq_system_zone_posix(zone_name);
	if (!posix)
		return -1;

	char buf[128];
	snprintf(buf, sizeof(buf), "%s\n", posix);
	if (write_file(PATH_TZ, buf) != 0)
		return -1;

	/* Carried alongside for anything that reads the name rather than the
	 * rule; nothing on this image does, but leaving the two disagreeing
	 * would be a trap for whatever eventually looks. */
	snprintf(buf, sizeof(buf), "%s\n", zone_name);
	write_file(PATH_TIMEZONE, buf);

	RSS_INFO("system: timezone %s (%s) — takes effect on reboot", zone_name, posix);
	return 0;
}

bool rmq_system_valid_host(const char *host)
{
	if (!host)
		return false;

	size_t n = strlen(host);
	if (n < 1 || n > 63)
		return false;

	/*
	 * A hostname or an IPv4 address and nothing else. This is the narrow
	 * type the writable-key table otherwise refuses to have: no slash, no
	 * space, no quote, no shell metacharacter can appear, so the value
	 * cannot become a path or a second config directive when it is written
	 * into ntp.conf on a line of its own.
	 */
	for (size_t i = 0; i < n; i++) {
		char c = host[i];
		if (!isalnum((unsigned char)c) && c != '.' && c != '-')
			return false;
	}

	/* Leading or trailing punctuation is not a hostname anyone meant. */
	if (host[0] == '.' || host[0] == '-' || host[n - 1] == '.' || host[n - 1] == '-')
		return false;

	return true;
}

int rmq_system_set_ntp_server(const char *host)
{
	if (!rmq_system_valid_host(host))
		return -1;

	/*
	 * One server, replacing the pool the image ships with. `iburst` is
	 * kept because the first sync is the one that matters on a camera
	 * whose clock starts at the image build date.
	 */
	char buf[128];
	snprintf(buf, sizeof(buf), "server %s iburst\n", host);
	if (write_file(PATH_NTP, buf) != 0)
		return -1;

	/*
	 * Unlike the timezone this can be applied now: ntpd holds no state
	 * worth preserving and nothing else depends on it being up.
	 */
	int rc = system("/etc/init.d/S49ntpd restart >/dev/null 2>&1");
	RSS_INFO("system: ntp server %s (ntpd restart %s)", host,
		 rc == 0 ? "ok" : "failed — will apply at reboot");
	return 0;
}

void rmq_system_report(const struct rmq_state *st, cJSON *state)
{
	(void)st;

	cJSON *o = cJSON_AddObjectToObject(state, "system");
	if (!o)
		return;

	char tz[128];
	read_line(PATH_TZ, tz, sizeof(tz));
	if (tz[0]) {
		cJSON_AddStringToObject(o, "tz", tz);
		const char *name = zone_name_for_posix(tz);
		/* Only a name from the list, so the select has something to
		 * match. A hand-edited /etc/TZ leaves this absent rather than
		 * inventing a name for a rule nobody here wrote. */
		if (name)
			cJSON_AddStringToObject(o, "timezone", name);
	}

	/* First `server` line; the file is ours to write but not ours to
	 * assume — an image or an operator may have left several. */
	FILE *f = fopen(PATH_NTP, "r");
	if (f) {
		char line[256];
		while (fgets(line, sizeof(line), f)) {
			char host[128];
			if (sscanf(line, " server %127s", host) == 1) {
				cJSON_AddStringToObject(o, "ntp_server", host);
				break;
			}
		}
		fclose(f);
	}
}
