/*
 * rcd_osd.c -- see rcd_osd.h
 */

#include "rcd_osd.h"

#include <rss_common.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* rod's ROD_ELEM_NAME_LEN, spelled out rather than included from rod.h, which
 * is another daemon's header. A section whose name will not fit is not an
 * element: rod skips it. */
#define OSD_NAME_MAX 32

/*
 * The last few sections a walk reached, which are the first few of the file.
 *
 * rss_config prepends each new section as it parses, so a walk hands them back
 * in reverse file order: the last [osd.*] in the file arrives first and the
 * first arrives last. Keeping a sliding window of the final `max` names is
 * therefore the whole of the ordering work, and it holds them in the reverse
 * of the order they are wanted in.
 */
struct window {
	char (*name)[RCD_SECT_MAX];
	int max;
	int held;
};

static void note(const char *section, void *userdata)
{
	struct window *w = userdata;
	const char *dot = strchr(section, '.');

	if (!dot || !dot[1] || strlen(dot + 1) >= OSD_NAME_MAX)
		return;

	if (w->held == w->max) {
		memmove(w->name[0], w->name[1], sizeof(w->name[0]) * (size_t)(w->max - 1));
		w->held--;
	}
	rss_strlcpy(w->name[w->held++], section, sizeof(w->name[0]));
}

int rcd_osd_elements(rss_config_t *file, char out[][RCD_SECT_MAX], int max)
{
	struct window w = {.name = out, .max = max, .held = 0};

	if (!file || !out || max <= 0)
		return 0;

	rss_config_foreach_section(file, RCD_OSD_PREFIX, note, &w);

	/* Back into file order, in place. */
	for (int i = 0, j = w.held - 1; i < j; i++, j--) {
		char tmp[RCD_SECT_MAX];

		rss_strlcpy(tmp, out[i], sizeof(tmp));
		rss_strlcpy(out[i], out[j], sizeof(tmp));
		rss_strlcpy(out[j], tmp, sizeof(tmp));
	}
	return w.held;
}

bool rcd_osd_name_ok(const char *section, char *err, size_t errsz)
{
	size_t plen = strlen(RCD_OSD_PREFIX);
	const char *name;
	bool all_digits = true;

	if (!section || strncmp(section, RCD_OSD_PREFIX, plen) != 0) {
		snprintf(err, errsz, "an element is named %s<something>", RCD_OSD_PREFIX);
		return false;
	}

	name = section + plen;
	if (!name[0] || strlen(name) >= OSD_NAME_MAX) {
		snprintf(err, errsz, "a name runs to %d characters", OSD_NAME_MAX - 1);
		return false;
	}

	for (const char *c = name; *c; c++) {
		if (!isdigit((unsigned char)*c))
			all_digits = false;
		if (isalnum((unsigned char)*c) || *c == '_' || *c == '-')
			continue;
		snprintf(err, errsz, "a name holds letters, digits, '-' and '_'");
		return false;
	}

	if (all_digits) {
		snprintf(err, errsz, "a name is not a number");
		return false;
	}
	return true;
}
