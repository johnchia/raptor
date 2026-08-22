/*
 * rcd_wifi.c -- Reading and writing the wifi credentials
 *
 * See rcd_wifi.h for why the store is the U-Boot environment.
 */

#include "rcd_wifi.h"

#include <rss_common.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef RCD_FW_PRINTENV
#define RCD_FW_PRINTENV "/usr/sbin/fw_printenv"
#endif
#ifndef RCD_FW_SETENV
#define RCD_FW_SETENV "/usr/sbin/fw_setenv"
#endif

#define WIFI_IFACE    "wlan0"
#define WIFI_CTRL_DIR "/run/wpa_supplicant"
#define WIFI_CONF     "/tmp/wpa_supplicant.conf"
#define WIFI_CONF_GEN "/etc/wireless/wpa-conf"
#define WPA_CLI	      "/usr/sbin/wpa_cli"

/*
 * The image's setup mode, which owns the radio whenever there is no network
 * to join: an access point, a DHCP server, a DNS server and the flag that
 * tells rhd to serve the setup page. rcd asks for it by name at the two
 * moments the store crosses that boundary and does none of it itself -- the
 * pieces are the image's, and a camera whose image has no setup mode simply
 * has no such script and the calls fail harmlessly.
 */
#ifndef RCD_SETUP_MODE
#define RCD_SETUP_MODE "/etc/wireless/setup-mode"
#endif

/*
 * How long to wait for the radio to associate before asking for a lease.
 *
 * Short, because rcd serves on one loop and nothing else is answered while
 * this runs -- a longer wait made `raptorctl config apply` report "Failed to
 * send to rcd" after an enact that had in fact worked. It only ever runs to
 * the end when association is failing, which is the case the guard is already
 * watching; a success returns in well under a second. Past this the
 * supplicant keeps trying on its own and so does udhcpc, so the wait makes
 * the common case immediate rather than deciding anything.
 */
#define WIFI_ASSOC_WAIT_MS 5000
#define WIFI_ASSOC_POLL_MS 500
#define WIFI_SSID_VAR	   "wlanssid"
#define WIFI_PSK_VAR	   "wlanpass"

/*
 * Run one of the two environment tools and collect what it printed.
 *
 * fork and execv, never system(). Everywhere else in rcd an external command
 * is a fixed path and a fixed verb, so a shell costs nothing; here the last
 * argument is a value that came off the network. V_TEXT admits every
 * printable character except a double quote and a backslash -- which leaves
 * ';', '$', '|' and a backtick, all of them ordinary in a wifi passphrase and
 * all of them a second command to a shell. Handing the value to execv as one
 * element of argv means no parser ever sees it.
 *
 * Returns the exit status, or -1 if the tool could not be run at all.
 */
static int env_run(char *const argv[], char *out, size_t outsz)
{
	if (out && outsz)
		out[0] = '\0';

	int fd[2] = {-1, -1};
	if (out && pipe(fd) != 0) {
		RSS_WARN("wifi: pipe: %s", strerror(errno));
		return -1;
	}

	pid_t pid = fork();
	if (pid < 0) {
		RSS_WARN("wifi: fork: %s", strerror(errno));
		if (fd[0] >= 0) {
			close(fd[0]);
			close(fd[1]);
		}
		return -1;
	}

	if (pid == 0) {
		if (fd[1] >= 0) {
			dup2(fd[1], STDOUT_FILENO);
			close(fd[0]);
			close(fd[1]);
		}
		int null = open("/dev/null", O_WRONLY);
		if (null >= 0) {
			dup2(null, STDERR_FILENO);
			close(null);
		}
		execv(argv[0], argv);
		_exit(127);
	}

	if (fd[1] >= 0)
		close(fd[1]);

	if (out && outsz) {
		size_t n = 0;
		ssize_t r;
		while (n + 1 < outsz && (r = read(fd[0], out + n, outsz - 1 - n)) > 0)
			n += (size_t)r;
		out[n] = '\0';
		/* fw_printenv -n still terminates its line. */
		while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
			out[--n] = '\0';
	}
	if (fd[0] >= 0)
		close(fd[0]);

	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int env_get(const char *name, char *out, size_t outsz)
{
	char *const argv[] = {(char *)RCD_FW_PRINTENV, (char *)"-n", (char *)name, NULL};

	/* A variable that is not set is not an error here: it is a camera
	 * nobody has given credentials to yet, which is reported as unset
	 * exactly like a key absent from raptor.conf. */
	if (env_run(argv, out, outsz) != 0 || !out[0])
		return -1;
	return 0;
}

static int env_set(const char *name, const char *value)
{
	/*
	 * fw_setenv with no value at all removes the variable, which is the
	 * state a reset asks for -- not an empty string, which wpa-conf would
	 * read as a network named nothing.
	 */
	char *const del[] = {(char *)RCD_FW_SETENV, (char *)name, NULL};
	char *const put[] = {(char *)RCD_FW_SETENV, (char *)name, (char *)value, NULL};

	if (env_run(value && value[0] ? put : del, NULL, 0) != 0) {
		RSS_WARN("wifi: cannot write %s to the boot environment", name);
		return -1;
	}

	/*
	 * Read it back before reporting success.
	 *
	 * This store is not a file of our own: the same sector carries
	 * bootcmd, mtdparts and the memory layout, and fw_setenv rewrites the
	 * whole of it to change one variable. A write that reports success and
	 * did not land is therefore the one failure here that can cost more
	 * than a setting, so it is checked rather than assumed -- cheaply, by
	 * asking for the value back.
	 */
	char back[RCD_VAL_MAX];
	int rc = env_get(name, back, sizeof(back));

	if (!value || !value[0]) {
		if (rc == 0) {
			RSS_WARN("wifi: %s still set after being cleared", name);
			return -1;
		}
		return 0;
	}
	if (rc != 0 || strcmp(back, value) != 0) {
		RSS_WARN("wifi: %s did not read back as written", name);
		return -1;
	}
	return 0;
}

static int ssid_get(char *out, size_t outsz)
{
	return env_get(WIFI_SSID_VAR, out, outsz);
}

static int ssid_set(const char *value)
{
	return env_set(WIFI_SSID_VAR, value);
}

/*
 * The passphrase is read back, and the report layer is what keeps it off the
 * wire: a V_SECRET returns "readable": false before any value is looked at.
 * The read has to work anyway, because the guard snapshots what it is about
 * to change and a snapshot it could not take is a change it could not undo.
 */
static int psk_get(char *out, size_t outsz)
{
	return env_get(WIFI_PSK_VAR, out, outsz);
}

static int psk_set(const char *value)
{
	return env_set(WIFI_PSK_VAR, value);
}

bool rcd_wifi_present(void)
{
	/*
	 * Answered once. This forks fw_printenv, and `schema` asks it for
	 * every key in the section on every request; the answer cannot change
	 * while rcd is running, because neither a boot variable nor a driver
	 * appears underneath it.
	 */
	static int cached = -1;
	if (cached >= 0)
		return cached != 0;

	/*
	 * The same question S40network asks, in the same order: the boot
	 * environment names a radio, and the kernel has one. Either alone is
	 * a camera that cannot use this section -- an unset wlandev means
	 * nothing will bring the interface up whatever the driver found, and
	 * a wlandev naming hardware this image has no driver for leaves
	 * nothing under /sys/class/net to configure.
	 */
	char dev[64];
	if (env_get("wlandev", dev, sizeof(dev)) != 0) {
		cached = 0;
		return false;
	}

	DIR *d = opendir("/sys/class/net");
	if (!d) {
		cached = 0;
		return false;
	}

	bool found = false;
	const struct dirent *e;
	while (!found && (e = readdir(d)))
		found = strncmp(e->d_name, "wlan", 4) == 0;
	closedir(d);

	cached = found ? 1 : 0;
	return found;
}

bool rcd_wifi_provisioned(void)
{
	char ssid[RCD_VAL_MAX];

	/* Nothing to provision, so nothing outstanding. See rcd_wifi.h. */
	if (!rcd_wifi_present())
		return true;

	/*
	 * Asked every time, unlike the presence question above. This is the
	 * one that changes while rcd runs -- a portal writing credentials and
	 * a reset clearing them are both this variable moving underneath a
	 * daemon that stays up across either.
	 */
	return env_get(WIFI_SSID_VAR, ssid, sizeof(ssid)) == 0;
}

int rcd_wifi_provision_reset(cJSON *resp, char *err, size_t errsz)
{
	(void)resp; /* it does something; it does not answer with anything */

	if (!rcd_wifi_present()) {
		snprintf(err, errsz,
			 "this camera has no radio, so there is no setup mode to return to");
		return -1;
	}

	/*
	 * The name first, then the passphrase, and the order is the whole of
	 * what makes a half-completed reset survivable.
	 *
	 * Cleared this way round, a second write that fails leaves a camera
	 * with no network to join and a passphrase for one it has forgotten:
	 * unprovisioned, so it comes up in setup mode, with a stray variable
	 * that the generator never reads because there is no ssid to write
	 * beside it. Cleared the other way round it leaves a camera that still
	 * knows which network to join and no longer knows how -- provisioned,
	 * so no setup mode, associating with nothing forever. That is the
	 * failure this design exists to make impossible, and it is not worth
	 * reintroducing for the sake of a tidier pair of calls.
	 */
	if (env_set(WIFI_SSID_VAR, "") != 0) {
		snprintf(err, errsz, "the network name could not be cleared");
		return -1;
	}
	if (env_set(WIFI_PSK_VAR, "") != 0) {
		snprintf(err, errsz,
			 "the network was forgotten, but its passphrase is still stored");
		return -1;
	}

	RSS_INFO("wifi: provisioning cleared; setup mode from the next boot");
	return 0;
}

/* Ask the running supplicant something. Non-zero when there is none to ask. */
static int wpa_cli(const char *verb, char *out, size_t outsz)
{
	char *const argv[] = {(char *)WPA_CLI,
			      (char *)"-p",
			      (char *)WIFI_CTRL_DIR,
			      (char *)"-i",
			      (char *)WIFI_IFACE,
			      (char *)verb,
			      NULL};
	return env_run(argv, out, outsz);
}

/* Whether the radio has finished associating and is ready to be given an
 * address. Anything else -- scanning, handshaking, or no supplicant at all --
 * is not yet. */
static bool wifi_associated(void)
{
	char st[512];

	if (wpa_cli("status", st, sizeof(st)) != 0)
		return false;
	return strstr(st, "wpa_state=COMPLETED") != NULL;
}

/*
 * Put the stored credentials into force.
 *
 * This does not bring the interface down, and the first version of it did.
 * Copying net_enact from rcd_network.c was the mistake: a wired interface has
 * no daemon living across the cycle, and a wireless one does. wpa_supplicant
 * holds its control socket at /run/wpa_supplicant/wlan0 for as long as it
 * runs, `ifdown` does not reliably reach the stanza's `post-down killall`, and
 * an `ifup` that follows starts a second supplicant which finds that socket
 * still bound:
 *
 *	ctrl_iface exists and seems to be in use - cannot override it
 *	Failed to initialize control interface '/run/wpa_supplicant'.
 *
 * The interface then comes up unassociated and never gets a lease. On a camera
 * whose only interface is the radio, that is the whole camera, and it took a
 * serial console to get back. The apply reported the failure correctly; what
 * it could not do was undo it.
 *
 * So the supplicant is left alone and asked to re-read its configuration
 * instead. Nothing is torn down, so there is no window in which the radio is
 * neither on the old network nor the new one.
 */
static int wifi_enact(void)
{
	/*
	 * Regenerate the configuration with the generator the wlan0 stanza
	 * itself runs at boot, so a camera reconfigured here and a camera
	 * rebooted arrive at byte-identical files. It exits non-zero when
	 * there is no ssid to write, which is a store nobody has configured
	 * rather than a failure to enact.
	 */
	char cmd[512];

	/*
	 * If the camera is in setup mode, the radio is an access point and
	 * the store has just been handed a network to join instead. That
	 * transition is more than a supplicant -- there is a DHCP server and
	 * a DNS server to stop, an address to drop and an rhd to put back on
	 * the console -- so it is asked for by name rather than reproduced
	 * here. The script decides for itself whether there is anything to
	 * do, which is why this is unconditional.
	 */
	(void)system(RCD_SETUP_MODE " stop >/dev/null 2>&1");

	snprintf(cmd, sizeof(cmd), "%s > %s 2>/dev/null", WIFI_CONF_GEN, WIFI_CONF);
	if (system(cmd) != 0) {
		/*
		 * No ssid, so there is nothing to associate with -- which on a
		 * camera that has a setup mode is not a failure to enact but
		 * the other half of it. This is the revert path: a credential
		 * that never worked has just been put back to the nothing that
		 * was there before, and unless the access point comes up again
		 * the camera is on no network at all and there is no way left
		 * to give it another one. That is the failure this whole
		 * design exists to prevent, so it is the store being enacted
		 * rather than an error.
		 */
		if (system(RCD_SETUP_MODE " start >/dev/null 2>&1") == 0) {
			RSS_INFO("wifi: no credentials; the camera is in setup mode");
			return 0;
		}
		RSS_WARN("wifi: no credentials to associate with; %s wrote nothing", WIFI_CONF_GEN);
		return -1;
	}

	RSS_INFO("wifi: re-associating %s", WIFI_IFACE);

	if (wpa_cli("reconfigure", NULL, 0) != 0) {
		/*
		 * No supplicant answered, so this is a radio that was never
		 * brought up -- or one left behind by an earlier failure, with
		 * its socket still on disk and nothing holding it. Clear that
		 * and go through ifup, which is the only path that starts a
		 * supplicant at all.
		 */
		RSS_INFO("wifi: no supplicant on %s; bringing the interface up", WIFI_IFACE);
		/*
		 * `ifdown` first, and not to tear anything down -- everything
		 * it would stop is already stopped, which is how this branch
		 * was reached. It is there for the record ifupdown keeps of
		 * which interfaces it has configured: an `ifup` on one that is
		 * still listed prints "interface wlan0 already configured",
		 * does nothing, and exits zero. The enact then reports success
		 * over a radio that never associated, and the guard reverts a
		 * credential that was correct.
		 *
		 * The killall and the unlink stay after it, and they are what
		 * keeps this from being the version that broke the camera: an
		 * ifdown does not reliably reach the stanza's own
		 * `post-down killall`, and a supplicant still holding its
		 * control socket makes the next one refuse to start.
		 */
		snprintf(cmd, sizeof(cmd),
			 "ifdown -f %s >/dev/null 2>&1; "
			 "killall -q wpa_supplicant 2>/dev/null; "
			 "rm -f %s/%s; "
			 "ifup %s >/dev/null 2>&1",
			 WIFI_IFACE, WIFI_CTRL_DIR, WIFI_IFACE, WIFI_IFACE);
		if (system(cmd) != 0) {
			RSS_WARN("wifi: could not bring %s up", WIFI_IFACE);
			return -1;
		}
	}

	/* Bounded, and not the verdict: a radio still handshaking here is a
	 * radio the guard is watching, and udhcpc asks again by itself. */
	for (int waited = 0; waited < WIFI_ASSOC_WAIT_MS; waited += WIFI_ASSOC_POLL_MS) {
		if (wifi_associated())
			break;
		usleep(WIFI_ASSOC_POLL_MS * 1000);
	}

	/*
	 * Renew, never restart. The lease that was valid on the old network
	 * is not valid on the new one, and udhcpc asks again on SIGUSR1 --
	 * whereas killing it is exactly how the first attempt left a camera
	 * with an associated radio and no address.
	 */
	snprintf(cmd, sizeof(cmd),
		 "kill -USR1 $(cat /var/run/udhcpc.%s.pid 2>/dev/null) 2>/dev/null", WIFI_IFACE);
	if (system(cmd) != 0)
		RSS_INFO("wifi: no dhcp client on %s to renew", WIFI_IFACE);

	return 0;
}

/* ------------------------------------------------------------------ */
/* What the radio can hear, and whether it got on                      */
/* ------------------------------------------------------------------ */

/* Bounds on one scan reply. Well under rss_ctrl's 64 KB, and past what a
 * person picks from: a list this long is already a list nobody reads. */
#define WIFI_SCAN_MAX	  32
#define WIFI_SCAN_OUT_MAX 8192

/*
 * One line of `wpa_cli scan_results`, which is tab-separated and in this
 * order:
 *
 *	bssid	frequency	signal level	flags	ssid
 *
 * The SSID is last for a reason worth keeping: it is the only field that can
 * contain a tab, so it is taken as the whole remainder of the line rather
 * than as the fifth field. Splitting on every tab would truncate a name at
 * its first one and then fail to match the network the operator picked.
 */
static bool scan_line(char *line, const char **ssid, int *signal, bool *secure)
{
	char *f[4];

	for (int i = 0; i < 4; i++) {
		char *tab = strchr(line, '\t');
		if (!tab)
			return false;
		*tab = '\0';
		f[i] = line;
		line = tab + 1;
	}
	if (!line[0])
		return false; /* hidden: nothing to show and nothing to pick */

	*signal = (int)strtol(f[2], NULL, 10);
	*secure = strstr(f[3], "WPA") != NULL || strstr(f[3], "WEP") != NULL;
	*ssid = line;
	return true;
}

int rcd_wifi_scan(cJSON *resp, char *err, size_t errsz)
{
	if (!rcd_wifi_present()) {
		snprintf(err, errsz, "this camera has no radio");
		return -1;
	}

	/*
	 * Ask for a fresh scan and then read what is already there. The
	 * request is deliberately not waited on -- see rcd_wifi.h. It fails
	 * while one is already running, which is not a failure of this call.
	 */
	(void)wpa_cli("scan", NULL, 0);

	char out[WIFI_SCAN_OUT_MAX];
	if (wpa_cli("scan_results", out, sizeof(out)) != 0) {
		snprintf(err, errsz, "the radio is not running a supplicant to scan with");
		return -1;
	}

	cJSON *arr = cJSON_AddArrayToObject(resp, "networks");
	if (!arr) {
		snprintf(err, errsz, "out of memory");
		return -1;
	}

	char *save = NULL;
	int kept = 0;

	/* The first line is the column header, and it has no tabs in the
	 * places scan_line needs, so it fails to parse like any other
	 * malformed line rather than needing to be counted past. */
	for (char *line = strtok_r(out, "\n", &save); line && kept < WIFI_SCAN_MAX;
	     line = strtok_r(NULL, "\n", &save)) {
		const char *ssid = NULL;
		int signal = 0;
		bool secure = false;

		if (!scan_line(line, &ssid, &signal, &secure))
			continue;

		/*
		 * One entry per name. A network with several access points
		 * appears once per radio it is heard on, and the operator is
		 * choosing a network rather than an access point -- so the
		 * first sighting stands and later ones are dropped. Fine as a
		 * linear scan: the list is capped at WIFI_SCAN_MAX.
		 */
		bool seen = false;
		const cJSON *e = NULL;
		cJSON_ArrayForEach(e, arr)
		{
			const cJSON *n = cJSON_GetObjectItemCaseSensitive(e, "ssid");
			if (cJSON_IsString(n) && strcmp(n->valuestring, ssid) == 0) {
				seen = true;
				break;
			}
		}
		if (seen)
			continue;

		cJSON *o = cJSON_CreateObject();
		if (!o)
			break;
		cJSON_AddStringToObject(o, "ssid", ssid);
		cJSON_AddNumberToObject(o, "signal", signal);
		cJSON_AddBoolToObject(o, "secure", secure);
		cJSON_AddItemToArray(arr, o);
		kept++;
	}

	/* wpa_supplicant already returns them strongest first, so there is
	 * nothing to sort -- and re-sorting would only be a second opinion
	 * about numbers this daemon did not measure. */
	return 0;
}

bool rcd_wifi_settled(void)
{
	char want[RCD_VAL_MAX];

	/* Nothing configured cannot have settled onto anything. */
	if (env_get(WIFI_SSID_VAR, want, sizeof(want)) != 0)
		return false;

	char st[1024];
	if (wpa_cli("status", st, sizeof(st)) != 0)
		return false;
	if (!strstr(st, "wpa_state=COMPLETED"))
		return false;

	/*
	 * On the network the store names, not merely on *a* network. Without
	 * this a camera that has not yet let go of the network it was already
	 * on reads as settled onto the one it is being moved to -- which is
	 * the reading that would confirm a credential that has not been tried.
	 *
	 * Anchored on the line start so that `bssid=` and `ssid=` are not the
	 * same match, and terminated so a prefix is not a match either.
	 */
	char needle[RCD_VAL_MAX + 8];
	snprintf(needle, sizeof(needle), "\nssid=%s\n", want);
	if (!strstr(st, needle))
		return false;

	/*
	 * And holding an address -- but this is the weaker half of the two,
	 * and it is worth saying which way round that is. OpenIPC's udhcpc
	 * script puts a hardcoded 192.168.1.10 on the interface when a lease
	 * does not arrive, so a camera that never associated at all still has
	 * an address and would pass this on its own. It is the ssid match
	 * above that carries the proof; this only rules out the moment
	 * between association and a lease.
	 *
	 * Asked of the kernel rather than by forking ifconfig -- this runs on
	 * a timer.
	 */
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return false;

	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	rss_strlcpy(ifr.ifr_name, WIFI_IFACE, sizeof(ifr.ifr_name));

	bool have_addr = ioctl(fd, SIOCGIFADDR, &ifr) == 0 &&
			 ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr != 0;
	close(fd);
	return have_addr;
}

/* One enact behind both keys: they are two variables read by one boot-time
 * generator, and a camera whose SSID and passphrase changed together must not
 * bring the interface up twice. */
const rcd_provider_t rcd_provider_wifi_ssid = {
	.get = ssid_get, .set = ssid_set, .enact = wifi_enact, .resettable = true};

const rcd_provider_t rcd_provider_wifi_psk = {
	.get = psk_get, .set = psk_set, .enact = wifi_enact, .resettable = true};
