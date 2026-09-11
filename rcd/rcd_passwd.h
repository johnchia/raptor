/*
 * rcd_passwd.h -- The root password, and what claiming a camera means
 *
 * A camera ships with an empty root password field. rhd's configuration route
 * authenticates against that account and refuses an empty field on purpose, so
 * a fresh camera answers 401 to everybody -- including the person holding it.
 * Claiming is what ends that, and it is one write: the moment root has a
 * password, the configuration route has a credential.
 *
 * "Claimed" is therefore a fact about /etc/shadow, derived and never stored.
 * Not a flag of rcd's own, for the reason rcd_wifi.h gives about provisioning:
 * a flag beside the store is a second record of the same thing, and the one
 * that can disagree with it.
 *
 * Two callers ask, and the split between them is the whole of the safety here.
 * rhd decides whether a request may reach rcd unauthenticated; rcd decides
 * whether the write it carries is allowed. Neither trusts the other's answer,
 * because this is the only unauthenticated write on the device and one check
 * is one mistake.
 */

#ifndef RCD_PASSWD_H
#define RCD_PASSWD_H

#include <cJSON.h>

#include "rcd_schema.h"

struct rcd_state;

/* The account. Not configurable: rhd authenticates against this one name, and
 * a camera where the two disagreed would be one nobody could configure. */
#define RCD_PASSWD_USER "root"

/*
 * Whether this camera's root account has a password.
 *
 * The claim refuses when it does, `state` reports it, and both read the same
 * file at the moment they are asked -- rcd stays up across a claim, and a
 * cached answer would be a camera claiming to be unclaimed.
 */
bool rcd_passwd_claimed(void);

/*
 * Whether a claim may be accepted at all: true only for a genuinely empty
 * password field. A locked account and an unreadable file are both false --
 * see rss_shadow.h for why those are not the same as unclaimed.
 */
bool rcd_passwd_claimable(void);

/* The root password. Live tier: /etc/shadow is read at each authentication,
 * so there is nothing left for `apply` to do and nothing to enact. */
extern const rcd_provider_t rcd_provider_root_password;

/*
 * Claim this camera: set the root password, once, on a camera that has none.
 *
 * Composed as a `set` on device.root_password rather than writing the store
 * directly, so the claim cannot accept a value the console would be refused:
 * every write goes through the one table.
 *
 * Refuses a camera that is already claimed. That refusal is the reason this
 * is a command and not just a key: a key can be set by anyone rcd lets
 * through, and the claim has to be the one write that stops working after it
 * has been used.
 */
cJSON *rcd_cmd_claim(struct rcd_state *st, const cJSON *root);

#endif /* RCD_PASSWD_H */
