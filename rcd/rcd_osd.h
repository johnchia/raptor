/*
 * rcd_osd.h -- The overlay elements a camera has
 *
 * rod's overlay is a list of elements, each a [osd.<name>] section named by
 * whoever wrote the config. rcd's key table names the shape of one -- see the
 * `osd.*` rows in rcd_schema.c -- and this is the other half: which of them
 * this camera's file actually has, and what a new one may be called.
 *
 * The order is the file's. rss_config prepends each section as it parses, so
 * a walk hands them back in reverse, and a client that lists elements should
 * see them the way the file does -- which is also the order rod resolves two
 * elements asking for one place in.
 *
 * Two schemes came before this one and both addressed an element as something
 * other than itself. The first offered the six places on the picture as the
 * sections, and could not reach [osd.timestamp] at all. The second made a
 * slot an ordinal -- `osd.1` for the first section of the file, whatever it
 * was called -- which moved under a client when a section was added above it,
 * spelled a new element's name `osd.4`, and could not say that a slot was
 * empty because there was nothing there to be empty. A name in the protocol
 * is what both were working around.
 */

#ifndef RCD_OSD_H
#define RCD_OSD_H

#include <stdbool.h>
#include <stddef.h>

#include <rss_common.h>

#include "rcd_schema.h"

/* As many as rod draws (its ROD_MAX_ELEMENTS). A file with more is not an
 * error -- rod ignores the rest too, and the ones a client cannot see are
 * the ones rod is not drawing. */
#define RCD_OSD_MAX 16

/* The section a key of an element lives under, and the pattern that stands
 * for all of them. */
#define RCD_OSD_PREFIX	"osd."
#define RCD_OSD_PATTERN "osd.*"

/*
 * The [osd.*] sections `file` has, in file order, into `out`. Returns how
 * many were written, at most `max`.
 */
int rcd_osd_elements(rss_config_t *file, char out[][RCD_SECT_MAX], int max);

/*
 * Whether `section` is a name an element may be reached by, with the refusal
 * in `err`.
 *
 * rod holds an element's name in 32 bytes and ignores a section whose name
 * will not fit, so a longer one is a section that is never drawn and never
 * explained. The rest keeps a name to what both a config file and a URL carry
 * without quoting, which is what makes it safe to pass on: it is the one
 * argument in rcd's table whose bytes are the caller's own.
 */
bool rcd_osd_name_ok(const char *section, char *err, size_t errsz);

/*
 * And whether it may name a *new* one, which is stricter by one rule: a name
 * is not a number. That is what the ordinals spelled their sections, and a
 * config with [osd.4] in it beside a client that means the fourth element by
 * `osd.4` reads as neither.
 *
 * Two rules rather than one because a camera set up by hand may hold any name
 * at all, including the ones this refuses -- and those elements are drawn.
 * Being unable to remove one because of how it is spelled would be the worse
 * rule, and a camera has one of those in its config right now.
 */
bool rcd_osd_new_name_ok(const char *section, char *err, size_t errsz);

#endif /* RCD_OSD_H */
