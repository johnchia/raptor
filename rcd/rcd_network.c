/*
 * rcd_network.c -- see rcd_network.h
 */

#include "rcd_network.h"
#include "rcd_system.h"

#include <rss_common.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PATH_IFACE_DIR RCD_SYSCONF_DIR "/network/interfaces.d"
#define PATH_RESOLV    RCD_SYSCONF_DIR "/resolv.conf"

/* Longest stanza this rewrites. The shipped ones are two lines; wlan0, the
 * biggest in the image, is five. */
#define STANZA_MAX 4096

/*
 * The directives this owns. Everything else in the stanza belongs to whoever
 * put it there and is copied through in the order it was found.
 *
 * `dns-nameserver` is not busybox's -- it is the resolvconf spelling, stored
 * here because /etc/resolv.conf is not a store: udhcpc rewrites it on every
 * lease, so a name server kept only there is forgotten the first time DHCP
 * answers. Kept in the stanza it survives, and the interface coming up is
 * what puts it in force.
 */
static const char *const owned[] = {"address", "netmask", "gateway", "dns-nameserver", NULL};

/* ------------------------------------------------------------------ */
/* Which interface                                                     */
/* ------------------------------------------------------------------ */

static char iface[32];

/*
 * S40network reads `wlandev` and, when it is set, brings up wlan0 and leaves
 * eth0 on a fallback address. So the presence of that variable is what makes
 * a camera wireless, and this asks the same question rather than a similar
 * one -- a camera whose primary interface disagreed with its boot script
 * would be configured on the interface it does not use.
 */
const char *rcd_net_iface(void)
{
	if (iface[0])
		return iface;

	rss_strlcpy(iface, "eth0", sizeof(iface));

	FILE *f = popen("fw_printenv -n wlandev 2>/dev/null", "r");
	if (f) {
		char dev[32] = "";
		if (fgets(dev, sizeof(dev), f)) {
			dev[strcspn(dev, " \t\r\n")] = '\0';
			/* The variable names the chipset driver, not the
			 * interface: S40network brings up wlan0 whichever it
			 * is. Only its presence is the answer. */
			if (dev[0])
				rss_strlcpy(iface, "wlan0", sizeof(iface));
		}
		pclose(f);
	}

	RSS_INFO("network: the camera's interface is %s", iface);
	return iface;
}

static void stanza_path(char *out, size_t outsz)
{
	snprintf(out, outsz, "%s/%s", PATH_IFACE_DIR, rcd_net_iface());
}

/* ------------------------------------------------------------------ */
/* Reading the stanza                                                  */
/* ------------------------------------------------------------------ */

/* First word of a line, lowercased into `out`. Leading blanks skipped. */
static const char *first_word(const char *line, char *out, size_t outsz)
{
	while (*line == ' ' || *line == '\t')
		line++;
	size_t n = 0;
	while (line[n] && line[n] != ' ' && line[n] != '\t' && line[n] != '\n' && n + 1 < outsz) {
		out[n] = line[n];
		n++;
	}
	out[n] = '\0';
	return line + n;
}

static bool is_owned(const char *word)
{
	for (int i = 0; owned[i]; i++) {
		if (strcmp(word, owned[i]) == 0)
			return true;
	}
	return false;
}

/*
 * The value of `want` in the stanza, or -1 when it has no such line.
 *
 * Bounded to the first stanza in the file. interfaces.d holds one per file,
 * but a `gateway` belonging to somebody else's stanza is not this camera's
 * gateway, and reading it as one would report a setting nothing applies.
 */
static int stanza_get(const char *want, char *out, size_t outsz)
{
	char path[192];
	stanza_path(path, sizeof(path));

	out[0] = '\0';

	FILE *f = fopen(path, "r");
	if (!f)
		return -1;

	int ret = -1;
	bool in_stanza = false;
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		char word[32];
		const char *rest = first_word(line, word, sizeof(word));

		if (strcmp(word, "iface") == 0) {
			if (in_stanza)
				break; /* the next stanza is not ours */
			in_stanza = true;

			/* iface <name> inet <method> -- the method is the one
			 * setting that is a word of this line rather than a
			 * directive under it. */
			char name[32], family[16], method[32];
			if (strcmp(want, "method") == 0 &&
			    sscanf(rest, "%31s %15s %31s", name, family, method) == 3) {
				rss_strlcpy(out, method, outsz);
				ret = 0;
			}
			continue;
		}

		if (!in_stanza || strcmp(word, want) != 0)
			continue;

		while (*rest == ' ' || *rest == '\t')
			rest++;
		size_t n = strcspn(rest, "\r\n");
		if (n >= outsz)
			n = outsz - 1;
		memcpy(out, rest, n);
		out[n] = '\0';
		ret = 0;
	}
	fclose(f);
	return ret;
}

/*
 * Rewrite one directive, carrying everything else through.
 *
 * An empty value removes the line rather than writing a blank one: "no
 * gateway" is a configuration, and `gateway` with nothing after it is a parse
 * error to ifupdown. A directive that is not in the stanza is appended inside
 * it, which is why the write walks to the end of the stanza rather than
 * stopping at the first match.
 */
static int stanza_set(const char *name, const char *value)
{
	char path[192];
	stanza_path(path, sizeof(path));

	/* The whole of what this file may change, checked here rather than
	 * trusted from the caller: a stanza carries `pre-up` commands and the
	 * shell substitution the MAC comes from, and a rewriter that could be
	 * pointed at one of those is a rewriter that could lose it. */
	if (!is_owned(name)) {
		RSS_ERROR("network: %s is not this daemon's to write", name);
		return -1;
	}

	int len = 0;
	char *text = rss_read_file(path, &len);
	if (!text) {
		RSS_WARN("network: %s is missing; refusing to invent one", path);
		return -1;
	}
	if (len > STANZA_MAX) {
		free(text);
		RSS_WARN("network: %s is larger than this knows how to rewrite", path);
		return -1;
	}

	char *out = calloc(1, (size_t)len + 256);
	if (!out) {
		free(text);
		return -1;
	}

	size_t used = 0;
	bool written = false;
	bool in_stanza = false;
	bool past_stanza = false;
	char *save = NULL;

	for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char word[32];
		first_word(line, word, sizeof(word));

		if (strcmp(word, "iface") == 0) {
			/* A second stanza in the same file ends ours. The
			 * shipped files hold one, but nothing guarantees it,
			 * and a directive appended after somebody else's
			 * `iface` line would belong to them. */
			if (in_stanza) {
				if (!written && value[0])
					used += (size_t)sprintf(out + used, "    %s %s\n", name,
								value);
				written = true;
				in_stanza = false;
				past_stanza = true;
			} else if (!past_stanza) {
				in_stanza = true;
			}
		} else if (in_stanza && strcmp(word, name) == 0) {
			/* Ours, and the one line that changes. A second copy
			 * is dropped rather than kept: ifupdown refuses a
			 * duplicate option outright, so leaving one behind
			 * would be an interface that no longer comes up. */
			if (!written && value[0]) {
				used += (size_t)sprintf(out + used, "    %s %s\n", name, value);
				written = true;
			}
			continue;
		}

		used += (size_t)sprintf(out + used, "%s\n", line);
	}

	if (in_stanza && !written && value[0])
		used += (size_t)sprintf(out + used, "    %s %s\n", name, value);

	free(text);

	char tmp[200];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	int ret = -1;
	if (f) {
		if (fchmod(fileno(f), 0644) != 0)
			RSS_WARN("network: cannot set the mode on %s", tmp);
		bool ok = fputs(out, f) >= 0;
		if (fclose(f) != 0)
			ok = false;
		if (ok && rename(tmp, path) == 0)
			ret = 0;
		else
			unlink(tmp);
	}
	free(out);

	if (ret != 0)
		RSS_WARN("network: cannot replace %s: %s", path, strerror(errno));
	return ret;
}

/* The method word on the `iface` line, which is the one thing in the stanza
 * that is not a directive of its own. */
static int method_set(const char *method)
{
	char path[192];
	stanza_path(path, sizeof(path));

	int len = 0;
	char *text = rss_read_file(path, &len);
	if (!text || len > STANZA_MAX) {
		free(text);
		return -1;
	}

	char *out = calloc(1, (size_t)len + 64);
	if (!out) {
		free(text);
		return -1;
	}

	size_t used = 0;
	bool done = false;
	char *save = NULL;
	for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char word[32], name[32], family[16], old[32];
		const char *rest = first_word(line, word, sizeof(word));
		if (!done && strcmp(word, "iface") == 0 &&
		    sscanf(rest, "%31s %15s %31s", name, family, old) == 3) {
			used += (size_t)sprintf(out + used, "iface %s %s %s\n", name, family,
						method);
			done = true;
			continue;
		}
		used += (size_t)sprintf(out + used, "%s\n", line);
	}
	free(text);

	int ret = -1;
	if (done) {
		char tmp[200];
		snprintf(tmp, sizeof(tmp), "%s.tmp", path);
		FILE *f = fopen(tmp, "w");
		if (f) {
			if (fchmod(fileno(f), 0644) != 0)
				RSS_WARN("network: cannot set the mode on %s", tmp);
			bool ok = fputs(out, f) >= 0;
			if (fclose(f) != 0)
				ok = false;
			if (ok && rename(tmp, path) == 0)
				ret = 0;
			else
				unlink(tmp);
		}
	}
	free(out);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Enacting                                                            */
/* ------------------------------------------------------------------ */

/*
 * Bring the interface down and back up, and this is the call that costs the
 * caller its connection. It is reached only from `apply`, never from `set`.
 *
 * Not `S40network restart`: that script's stop brings down lo and every
 * interface in the image, which is a great deal more than an address change
 * asked for. Not a bare `ifdown; ifup` either -- ifdown reads the file as it
 * is *now*, so switching dhcp to static makes it run the static teardown and
 * leave udhcpc running with the old lease. So the DHCP client is killed by
 * its own pidfile first and the address flushed explicitly, which is correct
 * whichever method the interface came up with.
 */
static int net_enact(void)
{
	const char *dev = rcd_net_iface();

	char cmd[512];
	snprintf(cmd, sizeof(cmd),
		 "kill $(cat /var/run/udhcpc.%s.pid 2>/dev/null) 2>/dev/null; "
		 "ifdown -f %s >/dev/null 2>&1; "
		 "ifconfig %s 0.0.0.0 down >/dev/null 2>&1; "
		 "ifup %s >/dev/null 2>&1",
		 dev, dev, dev, dev);

	RSS_INFO("network: reconfiguring %s -- every connection to the camera drops here", dev);
	int rc = system(cmd);

	/*
	 * The name server, last. With DHCP the lease script has just written
	 * /etc/resolv.conf and must win, because the server it names is the
	 * one that can actually be reached; with a static address nothing
	 * writes that file at all, so the stored value is the only way it is
	 * ever set.
	 */
	char method[32] = "";
	stanza_get("method", method, sizeof(method));
	if (strcmp(method, "static") == 0) {
		char dns[64] = "";
		stanza_get("dns-nameserver", dns, sizeof(dns));
		if (dns[0]) {
			char line[128];
			snprintf(line, sizeof(line), "nameserver %s\n", dns);
			FILE *f = fopen(PATH_RESOLV, "w");
			if (f) {
				fputs(line, f);
				fclose(f);
			} else {
				RSS_WARN("network: cannot write %s", PATH_RESOLV);
			}
		}
	}

	if (rc != 0) {
		RSS_ERROR("network: %s did not come back up", dev);
		return -1;
	}
	RSS_INFO("network: %s reconfigured", dev);
	return 0;
}

/* ------------------------------------------------------------------ */
/* The five keys                                                       */
/* ------------------------------------------------------------------ */

static int dhcp_get(char *out, size_t outsz)
{
	char method[32] = "";
	if (stanza_get("method", method, sizeof(method)) != 0 || !method[0])
		return -1;
	rss_strlcpy(out, strcmp(method, "dhcp") == 0 ? "true" : "false", outsz);
	return 0;
}

static int dhcp_set(const char *value)
{
	/*
	 * The stanza has to name a method, so there is no writing nothing
	 * here. What an interface nobody has configured does is DHCP -- it is
	 * what the packaged stanza says and what a camera out of the box has
	 * to do to be reachable at all -- so that is what emptying this means.
	 */
	if (!value[0])
		return method_set("dhcp");
	return method_set(strcmp(value, "true") == 0 ? "dhcp" : "static");
}

static int addr_get(char *out, size_t outsz)
{
	return stanza_get("address", out, outsz) == 0 && out[0] ? 0 : -1;
}
static int addr_set(const char *v)
{
	return stanza_set("address", v);
}

static int mask_get(char *out, size_t outsz)
{
	return stanza_get("netmask", out, outsz) == 0 && out[0] ? 0 : -1;
}
static int mask_set(const char *v)
{
	return stanza_set("netmask", v);
}

static int gw_get(char *out, size_t outsz)
{
	return stanza_get("gateway", out, outsz) == 0 && out[0] ? 0 : -1;
}
static int gw_set(const char *v)
{
	return stanza_set("gateway", v);
}

/*
 * The stored name server, falling back to whatever is resolving right now.
 *
 * On a camera that has never been given one, the stanza is silent and
 * /etc/resolv.conf holds the server DHCP handed out -- which is the true
 * answer to "what is this camera using", and a good deal more useful in the
 * field than an empty box. Once one is set, the stanza is the answer.
 */
static int dns_get(char *out, size_t outsz)
{
	if (stanza_get("dns-nameserver", out, outsz) == 0 && out[0])
		return 0;

	FILE *f = fopen(PATH_RESOLV, "r");
	if (!f)
		return -1;

	int ret = -1;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char host[128];
		if (sscanf(line, " nameserver %127s", host) == 1) {
			rss_strlcpy(out, host, outsz);
			ret = 0;
			break;
		}
	}
	fclose(f);
	return ret;
}

static int dns_set(const char *v)
{
	return stanza_set("dns-nameserver", v);
}

/* One enact behind all five: they write one file, and it is brought into
 * force once however many of them a request carried. */
const rcd_provider_t rcd_provider_net_dhcp = {dhcp_get, dhcp_set, net_enact, true};
const rcd_provider_t rcd_provider_net_address = {addr_get, addr_set, net_enact, true};
const rcd_provider_t rcd_provider_net_netmask = {mask_get, mask_set, net_enact, true};
const rcd_provider_t rcd_provider_net_gateway = {gw_get, gw_set, net_enact, true};
const rcd_provider_t rcd_provider_net_dns = {dns_get, dns_set, net_enact, true};
