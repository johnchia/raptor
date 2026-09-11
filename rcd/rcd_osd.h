/*
 * rcd_osd.h -- Which element an overlay slot stands for
 *
 * rod's overlay is a list of elements, each named by whoever wrote the config
 * and placed by a `position` line. A table of keys fixed at compile time
 * cannot name a section a person has not written yet, and the six place-named
 * sections that used to stand in for one were a worse fit than they looked. A
 * camera whose config says [osd.timestamp] and [osd.uptime] draws two elements
 * that a page keyed on places cannot reach at all, and offers six corners that
 * are empty because nothing was ever in them.
 *
 * So a slot is an ordinal rather than a place. `osd.1` through `osd.4` are the
 * first four [osd.*] sections of the config file, whatever they are called,
 * and `position` becomes what it always was to rod: a value, and one of the
 * four things a client may set.
 *
 * The ordinal is resolved against the file at each request and recorded
 * nowhere. A note of which section is which would be a second record of
 * something the file already says, and the one that can disagree with it --
 * and the file is what rod reads.
 *
 * What that costs is worth stating: the slots are positions in a list, so
 * inserting a section above one moves everything below it down, and a client
 * holding `osd.2` from a moment ago is then holding a different element. The
 * alternative is a name in the protocol, which is the thing a compile-time
 * table cannot have. Nothing here is destructive when it happens -- the worst
 * case is an edit landing in the neighbouring element -- and a config file is
 * not edited from two places at once often enough to buy anything better.
 */

#ifndef RCD_OSD_H
#define RCD_OSD_H

#include <stdbool.h>
#include <stddef.h>

#include <rss_common.h>

/* How many elements are reachable from the table. Four rather than rod's
 * sixteen because this is a page of forms, not a list editor: the elements
 * past these are still rod's, still drawn, and still hand-edited. */
#define RCD_OSD_SLOTS 4

/* The longest section name the config store holds, with its NUL. Longer than
 * RCD_SECT_MAX on purpose -- a resolved name is used to read the file and to
 * ask rod, never to fill an rcd_key_t, so the protocol's limit is not this
 * one. */
#define RCD_OSD_SECT_MAX 64

/*
 * The section a slot stands for, or `section` itself for anything else.
 *
 * A slot with no element yet resolves to its own literal name, which is what
 * makes an empty slot work in both directions without a flag: reading
 * [osd.3] from a file that has no such section reports the keys unset, and
 * writing one creates it. It is created at the end of the file, which is
 * where the ordinal already says it belongs.
 */
const char *rcd_osd_store(rss_config_t *file, const char *section, char *out, size_t outsz);

/*
 * Whether `section` is one of the ordinals and stands for no element yet.
 *
 * A page of forms has a value in every field whether or not anybody typed
 * one, so an empty slot's controls read as a position, an alignment and an
 * unticked Show long before there is an element to apply them to. Writing
 * those creates a section, and a section is an element: rod draws it, and it
 * takes a place on the picture that the element already drawn there then
 * loses. So an empty slot is filled only by the key that gives it something
 * to draw.
 */
bool rcd_osd_slot_is_empty(rss_config_t *file, const char *section);

#endif /* RCD_OSD_H */
