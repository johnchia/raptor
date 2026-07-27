/*
 * rod_template.c -- Template variable expansion and IP resolution
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "rod.h"

static void resolve_default_iface(char *out, int out_size)
{
	FILE *f = fopen("/proc/net/route", "r");
	if (!f) {
		out[0] = '\0';
		return;
	}
	char line[256];
	out[0] = '\0';
	while (fgets(line, sizeof(line), f)) {
		char iface[16];
		unsigned dest;
		if (sscanf(line, "%15s %x", iface, &dest) == 2 && dest == 0) {
			rss_strlcpy(out, iface, out_size);
			break;
		}
	}
	fclose(f);
}

static void refresh_ip_addrs(rod_state_t *st)
{
	int64_t now = rss_timestamp_us();
	if (st->ip_refresh_ts && now - st->ip_refresh_ts < 60000000)
		return;
	st->ip_refresh_ts = now;

	char iface[16] = "";
	resolve_default_iface(iface, sizeof(iface));

	st->ip[0] = '\0';
	st->ip6[0] = '\0';

	struct ifaddrs *ifa_list, *ifa;
	if (getifaddrs(&ifa_list) < 0)
		return;

	for (ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
		if (!ifa->ifa_addr || (ifa->ifa_flags & IFF_LOOPBACK))
			continue;

		bool match = iface[0] ? strcmp(ifa->ifa_name, iface) == 0 : true;
		if (!match)
			continue;

		if (ifa->ifa_addr->sa_family == AF_INET && !st->ip[0]) {
			struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
			inet_ntop(AF_INET, &sa->sin_addr, st->ip, sizeof(st->ip));
		} else if (ifa->ifa_addr->sa_family == AF_INET6 && !st->ip6[0]) {
			struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)ifa->ifa_addr;
			if (!IN6_IS_ADDR_LINKLOCAL(&sa6->sin6_addr)) {
				inet_ntop(AF_INET6, &sa6->sin6_addr, st->ip6, sizeof(st->ip6));
			}
		}

		if (st->ip[0] && st->ip6[0])
			break;
	}
	freeifaddrs(ifa_list);
}

static void format_uptime(char *buf, size_t bufsz)
{
	FILE *f = fopen("/proc/uptime", "r");
	if (!f) {
		snprintf(buf, bufsz, "Up: ?");
		return;
	}
	double up = 0;
	if (fscanf(f, "%lf", &up) != 1)
		up = 0;
	fclose(f);

	int sec = (int)up;
	int days = sec / 86400;
	int hours = (sec % 86400) / 3600;
	int mins = (sec % 3600) / 60;
	int secs = sec % 60;

	if (days > 0)
		snprintf(buf, bufsz, "%dd %dh %dm %ds", days, hours, mins, secs);
	else if (hours > 0)
		snprintf(buf, bufsz, "%dh %dm %ds", hours, mins, secs);
	else
		snprintf(buf, bufsz, "%dm %ds", mins, secs);
}

/* ── monitor variables: exposure from rvd, temperature from the kernel ── */

/*
 * Well under one tick, so a template using both exposure variables costs
 * one round trip per render rather than two, and the numbers still move
 * every second. The backoff is for the case that actually hurts: rvd
 * alive but wedged, where every expansion would otherwise pay the full
 * timeout. A dead rvd fails the connect immediately and costs nothing.
 */
#define ROD_EXPOSURE_TTL_US	900000
#define ROD_EXPOSURE_BACKOFF_US 5000000
#define ROD_EXPOSURE_TIMEOUT_MS 200

static void refresh_exposure(rod_state_t *st)
{
	int64_t now = rss_timestamp_us();
	int64_t ttl = st->exp_reachable ? ROD_EXPOSURE_TTL_US : ROD_EXPOSURE_BACKOFF_US;

	if (st->exp_refresh_ts && now - st->exp_refresh_ts < ttl)
		return;
	st->exp_refresh_ts = now;

	char resp[512];
	if (rss_ctrl_send_command(RSS_RUN_DIR "/rvd.sock", "{\"cmd\":\"get-exposure\"}", resp,
				  sizeof(resp), ROD_EXPOSURE_TIMEOUT_MS) < 0) {
		st->exp_reachable = false;
		st->exp_ae_luma = 0;
		st->exp_total_gain = 0;
		return;
	}
	st->exp_reachable = true;

	int luma = 0, gain = 0;
	rss_json_get_int(resp, "ae_luma", &luma);
	rss_json_get_int(resp, "total_gain", &gain);

	/* Negative would be a malformed response; clamp both to the "no
	 * reading" value rather than rendering nonsense. */
	st->exp_ae_luma = luma > 0 ? (uint32_t)luma : 0;
	st->exp_total_gain = gain > 0 ? (uint32_t)gain : 0;
}

/*
 * Chip temperature.
 *
 * SigmaStar exports it from the msys driver as TEMP_R, whose contents are
 * the literal text "Temperature 42\n" -- hence scanning for the first
 * number instead of atoi() on the buffer. That attribute sits inside
 * ms_msys.c's CONFIG_MS_CPU_FREQ ifdef, so a kernel built without cpufreq
 * simply has no node and the variable renders as "--".
 *
 * The standard thermal zone is tried as well, so this is not
 * SigmaStar-only. It reports millidegrees where msys reports degrees;
 * nothing this code runs on sits at 1000 C, so the magnitude is a safe
 * way to tell them apart without compiling in a per-platform unit.
 */
static const char *const soc_temp_paths[] = {
	"/sys/class/mstar/msys/TEMP_R",
	"/sys/devices/virtual/mstar/msys/TEMP_R",
	"/sys/class/thermal/thermal_zone0/temp",
};

#define ROD_SOC_TEMP_TTL_US 2000000

/*
 * Pull degrees C out of whatever the node contained. Split from the I/O so
 * the two format quirks above -- the "Temperature " label and the
 * millidegree scale -- are checkable without a sysfs tree.
 */
static bool parse_soc_temp(const char *buf, int *out)
{
	const char *p = buf;

	while (*p && *p != '-' && (*p < '0' || *p > '9'))
		p++;
	if (!*p)
		return false;

	/* A lone '-' with no digits after it is not a reading. */
	if (*p == '-' && (p[1] < '0' || p[1] > '9'))
		return false;

	long v = strtol(p, NULL, 10);
	if (v > 1000 || v < -1000)
		v /= 1000;

	*out = (int)v;
	return true;
}

static bool refresh_soc_temp(rod_state_t *st, int *out)
{
	int64_t now = rss_timestamp_us();

	if (st->soc_temp_refresh_ts && now - st->soc_temp_refresh_ts < ROD_SOC_TEMP_TTL_US) {
		*out = st->soc_temp;
		return st->soc_temp_valid;
	}
	st->soc_temp_refresh_ts = now;
	st->soc_temp_valid = false;

	for (size_t i = 0; i < sizeof(soc_temp_paths) / sizeof(soc_temp_paths[0]); i++) {
		FILE *f = fopen(soc_temp_paths[i], "r");
		if (!f)
			continue;

		char buf[64];
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		if (n == 0)
			continue;
		buf[n] = '\0';

		int t;
		if (!parse_soc_temp(buf, &t))
			continue;

		st->soc_temp = t;
		st->soc_temp_valid = true;
		break;
	}

	*out = st->soc_temp;
	return st->soc_temp_valid;
}

int rod_expand_template(rod_state_t *st, const char *tmpl, char *out, int out_size)
{
	int pos = 0;
	const char *p = tmpl;

	while (*p && pos < out_size - 1) {
		if (*p != '%') {
			out[pos++] = *p++;
			continue;
		}

		const char *end = strchr(p + 1, '%');
		if (!end) {
			out[pos++] = *p++;
			continue;
		}

		int vlen = (int)(end - p - 1);
		if (vlen <= 0 || vlen > 31) {
			out[pos++] = *p++;
			continue;
		}

		char varname[32];
		memcpy(varname, p + 1, vlen);
		varname[vlen] = '\0';

		char val[128] = "";
		if (strcmp(varname, "time") == 0) {
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			struct tm tm;
			localtime_r(&ts.tv_sec, &tm);

			char fmt[64];
			rss_strlcpy(fmt, st->settings.time_format, sizeof(fmt));
			char *fp = strstr(fmt, "%f");
			if (fp) {
				int fps = st->settings.frame_rate;
				int frame = (int)((int64_t)ts.tv_nsec * fps / 1000000000L);
				if (frame >= fps)
					frame = fps - 1;
				char fn[12];
				snprintf(fn, sizeof(fn), "%02d", frame);
				memcpy(fp, fn, 2);
			}
			strftime(val, sizeof(val), fmt, &tm);
		} else if (strcmp(varname, "uptime") == 0) {
			format_uptime(val, sizeof(val));
		} else if (strcmp(varname, "hostname") == 0) {
			rss_strlcpy(val, st->hostname, sizeof(val));
		} else if (strcmp(varname, "ip") == 0) {
			refresh_ip_addrs(st);
			rss_strlcpy(val, st->ip, sizeof(val));
		} else if (strcmp(varname, "ip6") == 0) {
			refresh_ip_addrs(st);
			rss_strlcpy(val, st->ip6, sizeof(val));
		} else if (strcmp(varname, "ae_luma") == 0) {
			/* "--" rather than 0 when there is no reading: 0 is a
			 * plausible-looking luma and would read as a pitch
			 * black scene instead of as a missing signal. */
			refresh_exposure(st);
			if (st->exp_ae_luma)
				snprintf(val, sizeof(val), "%u", st->exp_ae_luma);
			else
				rss_strlcpy(val, "--", sizeof(val));
		} else if (strcmp(varname, "total_gain") == 0) {
			/* Raw x1024 fixed point, deliberately unscaled: this is
			 * the number [ircut] night_gain is compared against, so
			 * a prettier 1.0x would not be the one to calibrate
			 * with. 1024 is unity. */
			refresh_exposure(st);
			if (st->exp_total_gain)
				snprintf(val, sizeof(val), "%u", st->exp_total_gain);
			else
				rss_strlcpy(val, "--", sizeof(val));
		} else if (strcmp(varname, "soc_temp") == 0) {
			int t = 0;
			if (refresh_soc_temp(st, &t))
				snprintf(val, sizeof(val), "%d", t);
			else
				rss_strlcpy(val, "--", sizeof(val));
		} else {
			for (int i = 0; i < st->var_count; i++) {
				if (strcmp(st->vars[i].name, varname) == 0) {
					rss_strlcpy(val, st->vars[i].value, sizeof(val));
					break;
				}
			}
		}

		int vallen = (int)strlen(val);
		if (pos + vallen < out_size) {
			memcpy(out + pos, val, vallen);
			pos += vallen;
		}
		p = end + 1;
	}

	out[pos] = '\0';
	return pos;
}
