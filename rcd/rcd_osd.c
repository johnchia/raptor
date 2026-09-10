/*
 * rcd_osd.c -- see rcd_osd.h
 */

#include "rcd_osd.h"

#include <rss_common.h>

#include <string.h>

/*
 * rod holds an element's name in ROD_ELEM_NAME_LEN bytes and ignores a
 * section whose name will not fit, so such a section is not an element and
 * must not take a slot -- an ordinal that counted something the picture never
 * shows would point every slot below it at the wrong element. Spelled out
 * here rather than included from rod.h, which is another daemon's header.
 */
#define OSD_NAME_MAX 32

/*
 * The last few sections a walk reached, which are the first few of the file.
 *
 * rss_config prepends each new section as it parses, so a walk hands them back
 * in reverse file order: the last [osd.*] in the file arrives first and the
 * first arrives last. Keeping a sliding window of the final RCD_OSD_SLOTS
 * names is therefore the whole of the ordering work, and it holds them in the
 * reverse of the order they are wanted in -- slot 1 is the newest entry.
 */
struct window {
	char name[RCD_OSD_SLOTS][RCD_OSD_SECT_MAX];
	int held; /* how many of `name` are filled */
	int seen; /* how many sections the walk reached, window or not */
};

static void note(const char *section, void *userdata)
{
	struct window *w = userdata;
	const char *dot = strchr(section, '.');

	if (!dot || !dot[1] || strlen(dot + 1) >= OSD_NAME_MAX)
		return;

	if (w->held == RCD_OSD_SLOTS) {
		memmove(w->name[0], w->name[1], sizeof(w->name[0]) * (RCD_OSD_SLOTS - 1));
		w->held--;
	}
	rss_strlcpy(w->name[w->held++], section, sizeof(w->name[0]));
	w->seen++;
}

const char *rcd_osd_store(rss_config_t *file, const char *section, char *out, size_t outsz)
{
	if (!file || !section || !out || strncmp(section, "osd.", 4) != 0)
		return section;

	/* One digit, and one of ours. "osd.top_left" and "osd.10" are section
	 * names like any other and are left exactly as they came. */
	if (section[4] < '1' || section[4] > '0' + RCD_OSD_SLOTS || section[5] != '\0')
		return section;

	int slot = section[4] - '0';
	struct window w = {.held = 0, .seen = 0};

	rss_config_foreach_section(file, "osd.", note, &w);

	/* A slot past the end of the list stands for nothing yet. */
	if (w.seen < slot)
		return section;

	rss_strlcpy(out, w.name[w.held - slot], outsz);
	return out;
}
