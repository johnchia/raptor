/*
 * rcd_schema.h -- What may be set, by whom, and at what cost
 *
 * The table is the policy. A key it does not name cannot be written and a
 * field an entry does not name is dropped, so the request that reaches a
 * daemon is rebuilt from the table rather than relayed -- which is what keeps
 * `save` with an arbitrary `file`, `shutdown`, and the sections holding
 * credentials out by construction rather than by a rule per hazard.
 *
 * It is also served over the wire. A client renders its forms, its ranges and
 * its warnings from `schema`, so nothing downstream keeps a second copy of
 * this table to drift out of step with it.
 */

#ifndef RCD_SCHEMA_H
#define RCD_SCHEMA_H

#include <stdbool.h>
#include <stddef.h>

#include <cJSON.h>

/*
 * Bounds on one rendered edit. Every value fits: the longest is a wifi
 * passphrase given in its pre-derived form, which is exactly 64 hex digits
 * and needs a 65th byte for the terminator. The next longest is a credential,
 * capped at 63 by the table entry that admits it.
 */
#define RCD_SECT_MAX 24
#define RCD_KEY_MAX  32
#define RCD_VAL_MAX  72

/* Longest daemon request the table can produce, with room to spare. */
#define RCD_REQ_MAX 512

/* Daemons rcd knows how to ask for state and to restart. The order is the
 * order they appear in diagnostics. */
typedef enum {
	RCD_D_RVD = 0,
	RCD_D_RSD,
	RCD_D_RAD,
	RCD_D_ROD,
	RCD_D_RIC,
	RCD_D_RMR,
	RCD_D_RMD,
	RCD_D_RHD,
	RCD_D_RWD,
	RCD_D_COUNT,
} rcd_daemon_t;

const char *rcd_daemon_name(rcd_daemon_t d);
rcd_daemon_t rcd_daemon_by_name(const char *name);

/*
 * What restarting a daemon costs whoever is using the camera. Declared per
 * daemon rather than judged per edit, and reported before the operator commits
 * to it -- the point of the explicit apply is that this number is knowable in
 * advance.
 */
typedef enum {
	RCD_IMPACT_NONE = 0, /* nothing is interrupted; the live tier */
	RCD_IMPACT_SERVICE,  /* one feature pauses; no client notices */
	RCD_IMPACT_STREAM,   /* connected viewers are dropped */
	RCD_IMPACT_PIPELINE, /* capture stops and everything downstream reconnects */
	RCD_IMPACT_NETWORK,  /* every connection to the camera is dropped, including
			      * the one that asked for this */
	RCD_IMPACT_REBOOT,   /* the camera restarts */
} rcd_impact_t;

const char *rcd_impact_name(rcd_impact_t i);
rcd_impact_t rcd_daemon_impact(rcd_daemon_t d);

/*
 * Value grammars.
 *
 * Almost every key is an integer, a boolean or a closed enum, and for those no
 * byte of the payload reaches the file: what is written is the table's own
 * spelling or a number rcd formatted. That is what keeps every path, format
 * string and endpoint alias in raptor.conf unreachable from a transport
 * without a rule naming any of them.
 *
 * V_CRED is the one exception and it exists for exactly the four credential
 * keys -- a username and a password for each of [rtsp] and [http]: a password
 * nobody may choose is not a password. Its grammar is RFC 3986's unreserved
 * set, which an RTSP URL, a Digest header, an INI value and a shell word all
 * accept unescaped, so a credential cannot become a second config directive, a
 * path, or a URL that parses as something else. Do not reach for it for
 * anything but a credential: for a path or a template the grammar is no
 * protection at all, and the reason those keys are absent is that they are
 * absent.
 */
typedef enum {
	V_INT = 0,
	V_BOOL,
	V_ENUM,
	V_CRED,
	V_HOST,
	V_IPV4,
	V_TEXT,
	V_SECRET,
} rcd_val_type_t;

/*
 * V_HOST is a hostname or an IPv4 address and nothing else: letters, digits,
 * '.' and '-', with no leading or trailing punctuation. No slash, space, quote
 * or shell metacharacter can appear, which is what lets such a value be
 * written to a line of its own in a file this daemon does not otherwise parse
 * -- it cannot become a path or a second directive there.
 *
 * It is not a relaxed V_CRED and must not be used for one: it is reported back
 * like any other value.
 *
 * V_TEXT is the one grammar that admits a space, and it exists for a wifi
 * SSID -- a name chosen by whoever owns the network, not by this camera, and
 * routinely containing spaces and punctuation that no other key here allows.
 * It is printable ASCII with two exclusions: a double quote and a backslash,
 * because the value is rendered inside a quoted string in wpa_supplicant.conf
 * and either one would end that string early. Control bytes are refused for
 * the same reason a newline is: the file is line-oriented.
 *
 * The overlay templates are the second use, and they fit the same grammar for
 * the same reason: text a person composes, going into a line of a file this
 * daemon writes.
 *
 * It is not a general string type. A value of this grammar still must not be
 * a path, a format or a command, and the protection is the same as everywhere
 * else in this table -- that no such key exists. A template is not the
 * exception it looks like: rod expands its own %var% names into a bitmap and
 * hands the result to nothing.
 *
 * V_SECRET is a wifi passphrase, and it differs from V_CRED in both
 * directions. Wider, because WPA's own grammar is 8 to 63 printable
 * characters and a passphrase nobody may choose is the V_CRED mistake again;
 * narrower, because it also accepts exactly 64 hex digits, which is a
 * pre-derived PSK. That second form is what lets a client hash the passphrase
 * before sending it, so the plaintext never crosses an open setup network.
 *
 * Like V_CRED it is never reported back, and it is emptiable: an open network
 * has no passphrase, and that is a configuration rather than an omission.
 *
 *
 * V_IPV4 is narrower still: four decimal octets, no leading zeros -- which
 * every C library reads as octal and no operator ever means that way. An empty
 * string is accepted where the table says so, because "no gateway" and "no
 * name server" are configurations rather than omissions.
 */

/*
 * A V_INT may still carry `choices`, and then it is a labelled integer: an
 * anti-flicker mode or an H.264 profile is a name, not a magnitude, and a
 * client given a bare 0-2 can only draw a slider over it.
 *
 * They are not V_ENUM because a V_ENUM writes its spelling, and every one of
 * these keys is read back with rss_config_get_int. The label is what a client
 * shows and may send; the number is what is stored.
 */

/*
 * One writable key.
 *
 * `live_cmd` is the whole tiering. A key that names one is applied to the
 * running daemon by that command and costs nothing; a key that does not is
 * written to the file and read on the owner's next start. Which of the two a
 * key is depends on the hardware, not on preference -- SigmaStar carries
 * orientation and the 3DNR level in one creation-time call, so those keys have
 * no live command however much one would be convenient -- and a caller never
 * has to know, because the answer is here and is reported back.
 */
/*
 * Where a key is kept, when it is not kept in raptor.conf.
 *
 * A key naming one of these is read and written through it instead of through
 * the config file and the owning daemon -- see rcd_system.h. Nothing else
 * about the key changes: the same table validates it, the same `set` batches
 * it, the same `schema` describes it.
 *
 * `set` is handed a value that has already been validated against the table
 * entry, so a provider never parses and never bounds-checks. It returns 0, or
 * -1 for a store that could not be written. `get` returns 0 and fills `out`,
 * or -1 for a value that is not set or not recognisable -- which is reported
 * as unset, exactly like a key absent from the config file.
 *
 * `set("")` means put the store back to having no value at all, and it is the
 * revert path that asks for it: a camera whose address was never configured
 * has to be returned to not having one, not to an empty one. A provider whose
 * store cannot be empty -- a hostname, a timezone -- refuses it and says so.
 * A key the table lets a client send empty gets the same call either way, so
 * there is nothing to distinguish and nothing that can be got wrong.
 */
typedef struct rcd_provider {
	int (*get)(char *out, size_t outsz);
	int (*set)(const char *value);

	/*
	 * Put the stored value into force. Optional, and its presence is what
	 * decides the key's tier: a provider without one is a store the system
	 * reads continuously, so `set` is the whole of it and the key is live.
	 * A provider with one has separated writing the value from paying for
	 * it -- bringing an interface down and back up, renaming a running
	 * host -- and that is exactly what `apply` is for.
	 *
	 * It takes no argument: it re-reads the store, so the same call
	 * enacts an edit and enacts a revert. Providers that share a store
	 * share one of these, and `apply` runs each distinct one once.
	 */
	int (*enact)(void);

	/*
	 * Whether this store has a state that counts as no configuration at
	 * all -- which is what a reset asks it for. True where `set("")`
	 * means something the camera can run on: an address nobody chose, an
	 * interface nobody configured. False where the store must always
	 * hold a value somebody picked, and there is nothing to go back to.
	 *
	 * A key kept in raptor.conf never needs this: removing its line is
	 * always available, and what the key then means is the default at
	 * whichever read site asks for it.
	 */
	bool resettable;

	/*
	 * The store on its own, for a `get` that answers from somewhere else
	 * when the store is silent. Optional, and only the guard uses it.
	 *
	 * A `get` is free to fall back -- "what is this camera resolving
	 * with" is a better answer than an empty box, and it is what an
	 * operator is asking. A snapshot is not: it is written back on a
	 * revert, so a fallback captured as `prev` becomes a setting nobody
	 * chose, pinned in a store that had no opinion and outliving the
	 * source it was read from. The guard needs to know the store held
	 * nothing, so that putting it back means emptying it again.
	 *
	 * Providers whose `get` reads only their own store leave this NULL
	 * and the guard uses `get`.
	 */
	int (*stored)(char *out, size_t outsz);
} rcd_provider_t;

typedef struct rcd_key {
	const char *section;
	const char *key;
	rcd_val_type_t type;
	int min, max;		    /* V_INT range; V_CRED/V_HOST length, min 0 */
	const char *const *choices; /* V_ENUM values, or V_INT labels for
				     * min+i; NULL-terminated */
	const char *live_cmd;	    /* NULL: restart tier */
	const char *live_arg;	    /* field name the live command expects */
	int live_chn;		    /* channel the command needs, or -1 */

	/*
	 * A selector the live command needs alongside the value, sent as its
	 * `key` field. The string analogue of `live_chn`: one command answers
	 * for a family of settings and is told which of them this is.
	 *
	 * A command taking a second *value* belongs in the action table, not
	 * here -- that is why set-rc-mode's bitrate and set-qp-bounds are
	 * actions. A selector is different in kind: it is constant for the
	 * key that names it, so binding it here leaves a command that takes
	 * one value, which is what this tier is.
	 */
	const char *live_sel; /* NULL: the command needs no selector */

	/* Where the value is kept, for a key that is not in raptor.conf. */
	const rcd_provider_t *provider;

	/*
	 * What enacting this key costs, when the owning daemon's impact is not
	 * the answer -- which is every provider-backed key, because no daemon
	 * owns one. Left RCD_IMPACT_NONE to derive it from the owner, which is
	 * what every key in raptor.conf does.
	 */
	rcd_impact_t impact;

	/*
	 * Seconds a client has to confirm this key before rcd puts it back.
	 * 0 for the keys that cannot cost anyone their way in.
	 *
	 * Per key rather than one number for the daemon, because the wait is
	 * how long the client needs to notice it is still there: a wired
	 * address change is a lease and an ARP cache, associating a cold radio
	 * is longer. See rcd_guard.h.
	 */
	int guard_sec;

	/*
	 * The word "auto" is a value for this key as well as a number, and is
	 * stored and reported as the word.
	 *
	 * It is the ISP knobs and only them. A tuning binary holds a curve per
	 * knob -- what the module does at each sensor gain -- and setting a
	 * number replaces that curve with one constant, so "leave it to the
	 * tuning" is a third thing to say and not a value on the scale. It
	 * used to be said by writing the neutral, which meant the one value a
	 * tuner might have chosen deliberately could not be asked for.
	 *
	 * Grammar, not capability: this says the word parses here, not that
	 * this camera's knob has an auto mode. Whether it does belongs to the
	 * silicon and the loaded tuning, and rvd answers it per knob in
	 * get-isp's `caps`, which rcd forwards in `state`. A knob without one
	 * refuses the word, with an error naming the key.
	 */
	bool auto_ok;

	/*
	 * The command that puts this key back, for a key whose owner can do
	 * that without being restarted.
	 *
	 * A reset normally has no value to hand a live command -- the daemon's
	 * own default is the point, and it reaches it by reading the file at
	 * its next start -- which is why a reset is restart-tier whatever the
	 * key's tier is when it carries a value. That reasoning holds for
	 * state a daemon keeps in its own process, and fails for state that
	 * outlives it: an ISP knob lives in the driver, so removing the key
	 * un-writes nothing and the last value stays applied.
	 *
	 * A key naming a command here has an owner that can undo the write
	 * itself, and the reset is sent rather than deferred. The command takes
	 * the key's name and no value -- what to put back is the owner's to
	 * know, which is the whole point of asking it. A refusal falls back to
	 * the restart, so naming one can only improve on the old behaviour.
	 */
	const char *live_reset; /* NULL: a reset waits for the restart */
} rcd_key_t;

/*
 * What a client is told about when a key takes effect.
 *
 * Live means the value is in force the moment `set` returns: a command reached
 * the running daemon, or a provider wrote a store that is read continuously.
 * Everything else waits for `apply`, or -- for RCD_IMPACT_REBOOT -- for the
 * camera to come back.
 */
bool rcd_key_live(const rcd_key_t *k);
rcd_impact_t rcd_key_impact(const rcd_key_t *k);

/* Whether this key can be put back to its default. See rcd_provider_t. */
bool rcd_key_resettable(const rcd_key_t *k);

/* Argument grammars for the actions below. */
typedef enum {
	A_END = 0,
	A_INT,	   /* whole JSON number within [min,max] */
	A_ENUM,	   /* JSON string, one of `choices` */
	A_SECTION, /* JSON string, one of the readable sections */
} rcd_arg_type_t;

typedef struct {
	const char *key;
	rcd_arg_type_t type;
	bool required;
	int min, max;
	const char *const *choices;
} rcd_arg_t;

/*
 * A verb that is not a config value: a momentary command, or one that takes
 * more arguments than a single value. Almost every action here takes effect on
 * the running camera without interrupting it -- nothing that merely stops a
 * daemon is in this table, and `apply` and `restart` are protocol commands
 * rather than entries here precisely because they do interrupt.
 *
 * `reboot` is the exception and is not a hole in that rule. What is kept out
 * above is stopping part of the camera on a caller's say-so, which leaves it
 * running and broken with nothing saying so; restarting all of it is a whole
 * operation that ends in a working camera, and it is one an operator asks for
 * on purpose. It says what it costs in `impact` like any other local action,
 * so no client has to know which of the two it is.
 *
 * `local` is the exception, and there is one of it. An action naming a handler
 * is performed by rcd rather than forwarded: no daemon owns the store behind
 * it, so there is nothing to route to and `daemon` stays NULL. Such an action
 * says what it costs in `impact` -- the tier the daemons' own actions all sit
 * in is the answer for a command that reaches a running daemon, and not for
 * one that writes a store nothing has read yet.
 */
typedef struct {
	const char *name;
	const char *daemon;   /* NULL: rcd's own, or resolved from `section` */
	const char *ctrl_cmd; /* NULL: same as name */
	const rcd_arg_t *args;
	bool persists; /* the daemon records this in its config */

	/*
	 * Performed here. Returns 0, or -1 with `err` filled in -- the
	 * sentence a client shows, so it says what did not happen rather
	 * than which call returned what.
	 *
	 * `resp` is the reply being built, for an action that answers with
	 * something rather than only doing something. Most add nothing to it.
	 */
	int (*local)(cJSON *resp, char *err, size_t errsz);

	/*
	 * Whether this camera can do it at all, asked of the hardware the way
	 * a key's availability is. NULL means always. A client hides an
	 * action reported unavailable, which is the same treatment as a key
	 * the silicon has no block for.
	 */
	bool (*avail)(void);

	/* What performing it costs, for a `local` action. Daemon actions are
	 * the live tier by construction and leave this alone. */
	rcd_impact_t impact;

	/* When it takes effect, for an action whose answer is not "now".
	 * Reported with the result and in the schema, so the one sentence a
	 * client shows lives beside the entry it describes. */
	const char *note;
} rcd_action_t;

const rcd_key_t *rcd_key_find(const char *section, const char *key);
const rcd_action_t *rcd_action_find(const char *name);

/* Walk the key table in declaration order. NULL past the end. */
const rcd_key_t *rcd_key_at(int i);

/* Which daemon re-reads a section, or RCD_D_COUNT for one nothing owns. */
rcd_daemon_t rcd_section_owner(const char *section);

/* Which daemon holds a readable section, by name, or NULL when the section is
 * not readable. A section that exists but carries a credential and one that
 * does not exist are the same answer from out here. */
const char *rcd_section_reader(const char *section);

/* Serialize the whole table, or one section of it, into `out`. */
void rcd_schema_emit(cJSON *out, const char *section_filter);

#endif /* RCD_SCHEMA_H */
