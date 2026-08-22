/*
 * rcd_wifi.h -- The credentials that put this camera on a network
 *
 * The store is the U-Boot environment, because that is what reads them: this
 * image's S40network asks `fw_printenv` for wlandev, and /etc/wireless/wpa-conf
 * builds wpa_supplicant.conf from wlanssid and wlanpass at every boot. Keeping
 * them anywhere else would mean two places holding the same fact, and the boot
 * path would still believe the environment.
 *
 * Individual variables, never a wholesale rewrite. The same sector holds
 * ethaddr and the whole boot command, and a camera that loses those is a
 * camera that needs a serial console.
 */

#ifndef RCD_WIFI_H
#define RCD_WIFI_H

#include <stdbool.h>
#include <stddef.h>

#include "rcd_schema.h"

/*
 * How long a client has to be shown to have survived a credential change.
 *
 * Longer than the wired window: a wired address is in force as soon as the
 * interface is up, while a radio has to scan, associate, complete the
 * four-way handshake and only then ask for a lease. On a cold ATBM6031 that
 * is comfortably past a minute when the access point is busy.
 */
#define RCD_GUARD_WIFI_SEC 180

/* Whether this camera has a radio at all. False makes every key in the
 * section unavailable, which is what drops the tab from the console rather
 * than offering settings nothing can act on. */
bool rcd_wifi_present(void);

/*
 * Whether this camera has been told which network to join.
 *
 * The fact behind setup mode, and it is a fact rather than an inference:
 * wlanssid is a variable whose only job is to hold it. thingino asks the same
 * question by grepping /etc/wpa_supplicant.conf for an ssid line, which is
 * why a fresh unit there has to ship a file that is deliberately not a
 * working configuration -- the state and the runtime config are the same
 * bytes, so one cannot be empty without breaking the other. Here the runtime
 * config is generated from this variable at every boot and holds no state of
 * its own.
 *
 * It is deliberately not a flag of rcd's own. A flag would be a second record
 * of the same thing, and the one that could disagree: `set` writes the store,
 * a failed change is put back by the guard from its snapshot of the store, and
 * a flag beside it would have to be reverted too or it would claim a network
 * the camera no longer has credentials for.
 *
 * A camera with no radio is provisioned. Setup mode exists only where a wifi
 * device does, so there is nothing there to provision and nothing that would
 * come of saying so -- and false is the answer that would send a caller
 * looking for an access point to raise.
 *
 * rcd answers this for a running camera. The boot path decides the same
 * question before rcd exists, and reads the same variable to do it: that is
 * the two of them agreeing by construction, which is what S40network and
 * rcd_wifi_present already do for the radio itself.
 */
bool rcd_wifi_provisioned(void);

/*
 * Forget the network and come back in setup mode.
 *
 * Clears both credentials from the boot environment. Nothing else: this is
 * the network the portal sets, not a factory reset, and the camera's name,
 * its addressing and its streaming configuration are untouched.
 *
 * The running radio is left alone, which is the whole of why this is safe to
 * offer. The camera stays on the network it is on until it next boots, so the
 * operator who has just made a mistake still has the connection they made it
 * over, and putting the credentials back undoes it. The alternative -- taking
 * the radio down here -- is a camera with no network and no portal until
 * somebody power-cycles it, which is the state this whole design exists to
 * keep it out of.
 *
 * Returns 0, or -1 with `err` holding the sentence to show.
 */
int rcd_wifi_provision_reset(cJSON *resp, char *err, size_t errsz);

/*
 * What the radio can hear.
 *
 * Asks the supplicant to scan and answers with the results it already has,
 * rather than waiting for the one just requested. rcd serves on a single
 * loop, and a scan takes seconds -- long enough that waiting for it here
 * would stall every other client, which is the same reason the association
 * wait above is as short as it is. A caller that wants fresh results asks
 * again; a page showing a list that fills in over a few seconds is what a
 * scan looks like anyway.
 *
 * Adds `networks` to `resp`: an array of {ssid, signal, secure}, strongest
 * first, one entry per name, hidden networks left out because there is
 * nothing to show and nothing to pick.
 */
int rcd_wifi_scan(cJSON *resp, char *err, size_t errsz);

/*
 * Whether the credentials now in the store have been shown to work.
 *
 * True only when the radio is associated with the network the store names
 * *and* holds an address on it -- which together are a complete proof that
 * the passphrase was right, because a wrong one never completes the
 * handshake. That is what lets a wifi change confirm itself; see
 * rcd_guard.h.
 *
 * Forks, so it is not something to ask on every pass of a loop.
 */
bool rcd_wifi_settled(void);

extern const rcd_provider_t rcd_provider_wifi_ssid;
extern const rcd_provider_t rcd_provider_wifi_psk;

#endif /* RCD_WIFI_H */
