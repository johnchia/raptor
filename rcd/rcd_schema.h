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

/* Bounds on one rendered edit. Every value fits: the longest is a credential,
 * capped at 63 by the table entry that admits it. */
#define RCD_SECT_MAX 24
#define RCD_KEY_MAX  32
#define RCD_VAL_MAX  64

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
} rcd_val_type_t;

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
typedef struct rcd_key {
	const char *section;
	const char *key;
	rcd_val_type_t type;
	int min, max;		    /* V_INT range; V_CRED length, min 0 */
	const char *const *choices; /* V_ENUM, NULL-terminated */
	const char *live_cmd;	    /* NULL: restart tier */
	const char *live_arg;	    /* field name the live command expects */
	int live_chn;		    /* channel the command needs, or -1 */
} rcd_key_t;

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
 * more arguments than a single value. Every action here takes effect on the
 * running camera without interrupting it -- nothing that merely stops a daemon
 * is in this table, and `apply` and `restart` are protocol commands rather
 * than entries here precisely because they do interrupt.
 */
typedef struct {
	const char *name;
	const char *daemon;   /* NULL: resolved from the section argument */
	const char *ctrl_cmd; /* NULL: same as name */
	const rcd_arg_t *args;
	bool persists; /* the daemon records this in its config */
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
