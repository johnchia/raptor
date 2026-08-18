# System configuration, and the portal it has to be ready for

A design for `[system]` — timezone, NTP, hostname, network — as a section of
`rcd` rather than a daemon of its own, written so that a captive portal is a
client of the same socket instead of a second way to configure the camera.

Status: the provider hook and the two time keys are **built and
board-verified**; everything from [Confirm-or-revert](#confirm-or-revert)
onward is still design. Verified facts cite the file they came from, on a
running SSC377QE (INFINITY6C) board on an OpenIPC base.

## Why this exists

Two things happened. The web console landed and renders every writable key from
`rcd`'s `schema`, so the browser holds no copy of the key table. Then the MQTT
bridge dropped its whole `config` entity category, on the grounds that two
interfaces offering the same key give two answers about what the camera is set
to. That was right, and it left `[system]` with no interface at all — timezone
and NTP were only ever `rmq`'s.

Separately, a captive portal is wanted for openipc-raptor. A portal is, in the
end, a form that writes system settings on a camera that has no network yet. If
`[system]` is an rcd section, the portal inherits validation, batching and apply
for nothing. If it is a set of one-off writers, the portal grows its own — which
is what thingino has, and it costs them a duplicate implementation of every
field (see [Appendix](#appendix-what-thingino-does)).

The root password is deliberately out of scope. It needs its own discussion
about privilege on this device; [Security](#security) records what that
discussion has to settle.

## Decision: extend rcd, do not add a daemon

- **One schema.** The console renders from `schema`. A `[system]` tab appears
  because new keys appear — no page code.
- **One writer.** Typed keys, ranges, choices, batching, tiers, drift tracking
  and `apply` sequencing already exist, already run as root, already write under
  one flock.
- **One client shape.** Console, `raptorctl`, `rmq` and the future portal all
  post the same documents to the same socket.
- **Smallest footprint.** A daemon costs an init script, a socket, a supervision
  entry, RAM, and a second thing that can be down. If `rcd` is down nothing is
  configurable anyway.

A separate daemon would earn its keep if system config had to outlive rcd or
ship independently of it. Neither is true.

The cost is one struct field, one accessor table, four value types and one
impact level. The wire format, the value grammars, `set` batching, the
stale/pending mirror and `apply` are untouched.

## Principles

1. **Every field any interface will ever collect is a key in the table.** The
   moment one becomes a special-cased side door, a second writer follows it.
2. **The portal is a client, not a path.** Its server-side code is AP bring-up,
   DNS hijack, redirect and a page. No config logic.
3. **Unprovisioned is state the daemon reports**, not a grep for a missing line
   in a config file.
4. **Anything that can strand the camera is revert-guarded.** One timer, built
   once, covers a changed IP and a mistyped wifi password.
5. **A secret may arrive pre-derived.** The interface that most needs system
   settings — an open setup AP — is the one that can least afford plaintext.

## The provider hook

Today a key means *a line in `raptor.conf`, re-read by the daemon that owns the
section*. `rcd_key_t` carries section, key, type, range, choices and an optional
`live_cmd`; the owner and the cost of an apply are derived from the section.
System keys are not that. Their stores are `/etc/TZ`, `/etc/ntp.conf`,
`/etc/network/interfaces.d/eth0` and the U-Boot environment.

So a key may name an accessor pair instead of taking the config-file default:

```c
typedef struct rcd_provider {
	/* Current value, rendered as the schema type says. */
	int (*get)(char *out, size_t outsz);

	/* Store it. The value has already been validated against the table
	 * entry, so this never parses and never bounds-checks. */
	int (*set)(const char *value);
} rcd_provider_t;
```

One field on `rcd_key_t`. `rcd_cmd_get` and `rcd_cmd_set` branch on it rather
than opening a daemon socket; `rcd_section_owner` answers `RCD_D_COUNT`, which
already means "nothing owns this".

Most of the implementation exists. `rmq/rmq_system.c` already writes `/etc/TZ`
and `/etc/ntp.conf`, carries the generated timezone table, and validates a
hostname. It moves to `rcd/rcd_system.c`.

`rmq`'s `system-set` command is **deleted** rather than turned into a forwarder.
Those two settings were the only reason system configuration lived in rmq at
all, and a bridge that is deliberately no longer a configuration interface
should not keep one door open into one.

### Four value types

| Type | Grammar | For |
|---|---|---|
| `V_HOST` | alphanumeric, `.`, `-`; 1–63; no leading or trailing punctuation | `ntp_server`, `hostname` |
| `V_IPV4` | dotted quad, each octet 0–255 | `address`, `netmask`, `gateway`, `dns` |
| `V_TEXT` | printable, length-capped, no `"` and no newline; reported back | `ssid` |
| `V_SECRET` | printable, no `:` and no newline; never reported back | wifi psk, later the root password |

`V_HOST`'s validator is already written, as `rmq_system_valid_host()`. No slash,
space, quote or shell metacharacter can appear, so the value cannot become a
second config directive on the line it is written to. `V_IPV4` is stricter than
`V_HOST` on purpose: an address field that accepts a name produces a config file
that fails at `ifup`, after the network is already down.

`V_TEXT` exists because an SSID is neither of those and is not a secret either.
It is up to 32 arbitrary octets, so `V_HOST` would refuse the spaces and
punctuation that real networks use; and it has to be shown back to whoever is
picking a network, so `V_SECRET`'s "never reported" is wrong for it. Two
characters are excluded, each for a reason at the store rather than in the
abstract: a `"` breaks the `ssid="..."` line that `wpa_passphrase` generates,
and a newline breaks the `name=value` record `fw_setenv` writes. The cap is the
store's, not a taste — 32 for an SSID.

**`V_CRED` cannot be reused for `V_SECRET`'s job.** Its grammar is RFC 3986's
unreserved set — alphanumerics and `- . _ ~` — because those four credentials
end up inside RTSP URLs and Digest headers. `$` is not in that set, so `V_CRED`
cannot carry a `$6$` crypt string or a WPA passphrase containing punctuation.
Widening it would weaken the four keys it exists to protect.

### One more impact level

The impact enum stops at `pipeline`. Timezone genuinely only takes effect on
reboot: `rcS` exports `TZ` once at boot, and a raptor daemon restarts by
re-execing itself, which keeps the environment it already had.

```
RCD_IMPACT_NONE      nothing is interrupted; the live tier
RCD_IMPACT_SERVICE   one feature pauses; no client notices
RCD_IMPACT_STREAM    connected viewers are dropped
RCD_IMPACT_PIPELINE  capture stops, everything downstream reconnects
RCD_IMPACT_REBOOT    the camera restarts                        <-- new
```

Impact is currently derived per daemon. A provider-backed key has no daemon, so
it declares its own — a small generalisation of `rcd_daemon_impact()` into
"impact of this key", falling back to the owner's for everything that exists
today.

**Tier and impact answer different questions, and the timezone is what
separates them.** The tier says whether anything is still owed to `apply`; the
impact says what it costs to be in force. A provider owes nothing — its store
*is* the value, and `apply` would find nothing to enact — so every
provider-backed key is `live`, and the timezone carries `impact: reboot` beside
it. A client that reads only the tier applies nothing and is right; one that
reads the impact says why the clock has not moved yet. Reporting it as
`restart` instead would put it in the apply queue, where pressing Apply could
never clear it.

### What does not change

`rhd` stays a dumb proxy. `POST /api/v1/rcd` already forwards an opaque body to
rcd's socket on a worker thread and returns the reply verbatim. The daemon that
parses HTTP off the network never touches `/etc` — it hands JSON to a
root-owned Unix socket and waits. That property is worth more here than anywhere
else in the system, and this design does not spend it.

## The key table

Cost is what an `apply` of that key costs whoever is using the camera.
`revert` means guarded by the timer in [Confirm-or-revert](#confirm-or-revert).

| Key | Type | Store | Cost | Notes |
|---|---|---|---|---|
| `system.timezone` | `V_ENUM` | `/etc/TZ` + `/etc/timezone` | reboot | 309 choices from the generated zone table. `/etc/TZ` takes the POSIX rule, `/etc/timezone` the name, so the control reads back the zone that was picked rather than whichever one shares its rule |
| `system.ntp_server` | `V_HOST` | `/etc/ntp.conf` | service | Written as `server <host> iburst`; `S49ntpd restart` applies it now. Already implemented |
| `system.hostname` | `V_HOST` | `/etc/hostname` (overlay) | reboot | Also advertised by udhcpc, so it is how the camera names itself on the LAN |
| `network.dhcp` | `V_BOOL` | `interfaces.d/eth0` | revert | False switches the stanza to `static` and requires the three below |
| `network.address` | `V_IPV4` | `interfaces.d/eth0` | revert | Kept while `dhcp` is true, so switching back and forth does not lose the static settings |
| `network.netmask` | `V_IPV4` | `interfaces.d/eth0` | revert | |
| `network.gateway` | `V_IPV4` | `interfaces.d/eth0` | revert | |
| `network.dns` | `V_IPV4` | `/etc/resolv.conf` | revert | Overwritten by udhcpc on a lease, so only meaningful with `dhcp = false`. The schema should say so rather than let it look effective |
| `wifi.ssid` | `V_TEXT` | U-Boot env `wlanssid` | reboot | Capped at 32 octets |
| `wifi.psk` | `V_SECRET` | U-Boot env `wlanpass` | revert | Accepts a passphrase or a 64-hex pre-derived PSK |

Three sections rather than one flat `[system]`: rcd already groups by section
and the console already makes a tab per group, so `[network]` and `[wifi]` land
as their own pages for free — and the availability rule that hides absent
hardware can hide `[wifi]` whole on a wired camera.

**`[network]` describes the primary interface, and a camera has one.** If a
wifi device is present it is the primary interface; otherwise `eth0` is. The
provider resolves that once and writes `interfaces.d/<primary>`, so there is one
address to configure and one to show rather than a section per interface and a
question about which one the console is talking about. A wireless camera's
wired port keeps whatever `interfaces.d/eth0` ships with, which is DHCP.

## Providers and their stores

Each provider owns exactly one file, or one set of environment variables, writes
it atomically, and never rewrites anything it does not own. Four traps are
already known.

**`/etc/hostname` is sysupgrade's witness.** The packaged copy is what
`sysupgrade` reads to decide the image matches the SoC. The provider must write
the overlay copy and leave the packaged file alone. The boot script derives the
default name from the MAC, so an unset hostname is not an empty one.

**The eth0 stanza carries the MAC.** `/etc/network/interfaces.d/eth0` is two
lines, and the second is `hwaddress ether $(fw_printenv -n ethaddr)`, evaluated
by the shell at `ifup` time. Rewriting the file as "iface + address + netmask +
gateway" loses the camera's MAC. The provider must rewrite only the stanza's own
directives and carry every other line through.

**The U-Boot environment holds the MAC too.** Wifi credentials on this base are
not in a file: `/etc/network/interfaces.d/wlan0` runs
`wpa_passphrase "$(fw_printenv -n wlanssid)" "$(fw_printenv -n wlanpass)"` at
ifup, and `S40network` reads `wlandev`, `wlanmac`, `netaddr_fallback` and
`ethaddr` the same way. So the wifi provider writes the environment with
`fw_setenv` and matches the boot path rather than forking it — the alternative
means owning the wlan0 ifup script too. But that same sector carries `ethaddr`,
derived once in U-Boot from the flash unique ID, so the provider sets individual
variables and never rewrites the environment wholesale.

**U-Boot's own `ipaddr` is not a store.** The environment also has `ipaddr`,
`netmask`, `gatewayip` and `serverip`. Those are U-Boot's, for tftp, and have
nothing to do with the running system's address. `netaddr_fallback` is the one
that is Linux's, and `S40network` only applies it on the wireless path — so it
is not a safety net for a wired camera.

## Confirm-or-revert

An address change is delivered over the connection it is about to break.
Applied optimistically, a typo puts a camera on a pole beyond reach.

So `apply` gains a guarded form for keys that declare it. rcd keeps the previous
version of every file it is about to touch, writes the new one, enacts it, and
arms a timer. If `{"cmd":"confirm"}` does not arrive before the timer expires,
the previous files are restored and re-enacted. The client's job is to re-reach
the camera at its new address and confirm; the console knows both addresses, so
it can do that itself and show a countdown while it tries.

```
                apply (guarded key)
     Steady ──────────────────────▶ Armed ──────── confirm ────────▶ Steady
        ▲                             │
        │                             ├── timer expires ──┐
        │                             └── cancel ─────────┤
        └────── previous config re-enacted ──── Reverting ┘
```

- `apply` reports `"guarded": true` and `"revert_in_sec"` when it arms.
- `state` carries `system.revert_in_sec` while armed, so a client that
  reconnects mid-window — the expected case — can find out it is inside one.
- `confirm` disarms. `cancel` reverts immediately rather than waiting the clock
  out.
- **A reboot inside the window reverts.** The timer lives in `/run`, and its
  absence at boot is not a confirmation. This is what makes power-cycling a
  stranded camera a recovery rather than a commitment.

The window is per-key, not global: 90 seconds suits a wired address change —
long enough for DHCP release, ARP settling and a page reload, short enough that
nobody waits it out by accident — while associating a cold radio needs more.

## Provisioning and the portal

### Provisioned is a fact, not a grep

thingino decides at boot whether to run its portal by grepping
`/etc/wpa_supplicant.conf` for `ssid=` and `psk=`; a fresh unit ships a file
that is deliberately an incomplete AP config so the grep fails. The state is
inferred from a missing line in a file that also has to be a valid runtime
config.

Instead: rcd reports `state.system.provisioned` and offers a `provision-reset`
action. The init script asks one question, and "forget the network and come back
in setup mode" is a command rather than the removal of a file.

### The portal is a client

```
    Web console ──┐
                  ├──▶ rhd  POST /api/v1/rcd ──┐
    Portal page ──┘                            │
                                               ├──▶ rcd ──▶ raptor.conf
    raptorctl ─────────────────────────────────┤      ├──▶ /etc/TZ, ntp.conf,
                                               │      │    hostname
    rmq (MQTT) ────────────────────────────────┘      ├──▶ interfaces.d/eth0,
                                                      │    resolv.conf
                                                      └──▶ U-Boot env
```

The portal's save button posts one `set` with an `edits[]` array and then one
`apply` — the batching that already exists. Nothing portal-shaped is added to
the protocol, and the portal cannot accept a value the console would reject.

### Portal mode belongs to rhd

thingino's current shape is one HTTP server with two configurations, selected by
a `/run/portal_mode` flag: portal mode binds the portal address, serves a
different document root, and uses the server's error handler to catch every OS's
connectivity-check URL and 302 it. That is the right shape, and rhd should grow
it rather than gaining a sibling.

- A mode in which any unmatched path answers `302 /`. The list of check URLs
  (`/generate_204`, `/hotspot-detect.html`, `/ncsi.txt`, and the rest) then does
  not need to exist at all — and if it ever does, it is data, not routing code.
- Portal mode implies no authentication and a bind restricted to the setup
  address. Those are the same decision: an open setup network is not a place to
  demand a password nobody has yet, and not a place to expose anything else.

### No radio, no portal

Setup mode exists only where a wifi device does. A camera with no radio never
enters it, however unprovisioned or unreachable it is.

The tempting version — no DHCP lease on `eth0`, so take `172.16.0.1`, serve
DHCP and hijack DNS there — is a camera answering DHCP on somebody's wired
segment. The failure it would address is a network that is misconfigured or
briefly down, and the cure hands out addresses to everything else plugged into
the same switch. A radio is its own segment; a wire is not, and that is the
whole difference.

So the trigger is `not provisioned` **and** a wifi device is present. Presence
is the same question `S40network` already asks — the `wlandev` environment
variable and an interface under `/sys/class/net` — so the init script and the
network script agree by construction rather than by two guesses.

A wired camera that cannot get an address is therefore recovered the way it
always was: serial, or a reflash. That is a real cost and it is the right one;
the alternative is a camera that reacts to a broken network by breaking the
network.

Verified absent on the bench board: `wpa_supplicant`, `hostapd`, `iw`,
`iwconfig`, and busybox `dnsd`. Present: `udhcpd`, `udhcpc`,
`fw_printenv`/`fw_setenv`. This board is wired-only, so it will never run a
portal — the portal ships with the wireless packages, and is tested on a board
that has a radio.

## Security

- **The root password is the one open question here** and should stay out of the
  key table until it has had its own discussion. What that discussion must
  settle: whether the web console may set it at all; whether rcd must refuse the
  key unless rhd has authentication configured, since otherwise an
  unauthenticated console is a one-click root takeover; and whether an SSH key
  is the better primitive to expose instead.
- **A setup portal is unauthenticated by construction.** Whoever is in range, or
  on the cable, can set every key it exposes. That argues for exposing the
  smallest set that gets the camera onto the network, and for setup mode ending
  the moment it succeeds.
- **An open AP carries plaintext.** Hence `V_SECRET` accepting a pre-derived
  form, and hence the schema advertising which keys accept one — thingino
  returns a `features` list for exactly this, and a client that can hash locally
  then never sends the secret.
- **Everything here crosses the LAN in clear** unless rhd has TLS. That is
  already true of the camera credential the console sets today; it does not get
  worse, but the list of things it applies to grows.

## Phasing

1. **Provider hook, timezone and NTP.** *Done.* The hook,
   `RCD_IMPACT_REBOOT`, `V_HOST`, and `rmq_system.c` moved to `rcd_system.c`,
   with rmq's `system-set` deleted. The console needed three changes and not a
   fourth: a text widget for `V_HOST`, a reboot badge, and wording for the two
   keys. It did *not* need a tab — an unclaimed section already falls onto a
   page of its own, which is the property that was under test.
2. **Confirm-or-revert.** The guarded apply, its timer in `/run`,
   `confirm`/`cancel`, and the countdown in `state`. Exercised by hostname
   first, which is harmless, before anything that can actually strand the camera
   depends on it.
3. **Wired network settings.** `V_IPV4`, the `interfaces.d/eth0` provider,
   DHCP-versus-static, and the console flow that re-reaches the camera at its
   new address. The first feature where getting it wrong costs a trip to the
   camera, hence the ordering.
4. **Wireless settings.** `V_TEXT`, `V_SECRET` and the `fw_setenv` provider,
   with the packages that make a radio work at all. Configurable over the wire
   from the console before any portal exists, which is the order that lets the
   keys be wrong somewhere recoverable.
5. **Setup mode.** `state.system.provisioned`, `provision-reset`, rhd's portal
   mode, AP bring-up, `udhcpd` and the DNS hijack. Last because it is the only
   phase that needs hardware this bench does not have, and because by then every
   value it collects is a key that already works.

## Appendix: what thingino does

Read from `thingino-firmware` at `314545f2c`. Their portal works and is verified
on hardware; these notes are about shape, not quality.

| File | Role |
|---|---|
| `package/wifi/files/S38wpa_supplicant.in` | Picks `portal`, `ap` or `client` at boot by grepping `wpa_supplicant.conf`. In portal mode: open AP `THINGINO-<last 2 MAC octets>`, `172.16.0.1/24`, `udhcpd`, `dnsd`, a sound, and a 600s timeout that tears the portal down. Two-phase, via a `portal_confirmed` verb |
| `package/wifi/files/dnsd-portal.conf` | Every OS's connectivity-check hostname mapped to the camera, ending in `* 172.16.0.1`. The wildcard is the whole mechanism |
| `package/wifi/files/httpd-portal.conf` | 15 `P:` redirect rules — Android, Apple, Windows, Kindle — all landing on a five-line CGI that 302s to the page |
| `package/thingino-uhttpd/files/S60uhttpd` | The newer shape: one server, portal document root and bind address chosen off `/run/portal_mode`, an error handler instead of the URL list |
| `package/wifi/files/api.cgi` | Collects hostname, timezone, SSID/passphrase, AP flag, root password and SSH key — then validates and writes each one itself, and reboots |

### Worth taking

- **Pre-derived secrets, advertised as a capability.** `api.cgi` accepts
  `wlan_psk` as 64 hex and `rootpass_hash` as a `$6$` crypt string instead of
  the plaintext, and `get_info` returns `"features": ["wlan_psk",
  "rootpass_hash"]` so a capable client knows it may hash locally. The setup
  network is open by definition; this is the right instinct.
- **One HTTP server with a portal mode**, not two servers.
- **Wildcard DNS plus redirect**, which is the whole of "captive".

### Worth avoiding

- **A second configuration writer.** `api.cgi` re-implements hostname
  validation, the timezone write and the credential store, none of it shared
  with the 2713-line adapter that does the same job for their normal web UI.
- **No recovery from a wrong password.** The trigger is "credentials absent", so
  a typo leaves credentials *present*: the camera comes up as a client, fails to
  associate, and never returns to the portal. The documented escape is an SD
  card carrying `uenv.txt`, which a mounted camera does not have. This is the
  single strongest argument for [Confirm-or-revert](#confirm-or-revert).
