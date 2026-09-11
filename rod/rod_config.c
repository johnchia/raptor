/*
 * rod_config.c -- Config loading and OSD element initialization
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rod.h"

static int parse_align(const char *s)
{
	if (!s || !s[0])
		return 0;
	if (strcmp(s, "right") == 0)
		return 2;
	if (strcmp(s, "center") == 0)
		return 1;
	return 0;
}

/*
 * The places on the picture, and how text sitting in each one reads.
 *
 * An element whose section is named for one of these sits there, and aligns
 * itself out from that edge, without being told either. [osd.top_right] is a
 * complete element with a template and nothing else.
 *
 * That is what lets the six be offered as fixed slots -- a client can write
 * [osd.bottom_left] template = ... and have it appear where the name says,
 * rather than having to invent a name for the element and then place it. A
 * section named anything else is unaffected and still defaults to top left,
 * and a `position` or `align` line always wins over the name.
 */
static const struct {
	const char *name;
	int align; /* 0 left, 1 center, 2 right -- as parse_align returns */
} slots[] = {
	{"top_left", 0},      {"top_center", 1},   {"top_right", 2}, {"bottom_left", 0},
	{"bottom_center", 1}, {"bottom_right", 2}, {"center", 1},
};

const char *rod_place_name(int i)
{
	if (i < 0 || (size_t)i >= sizeof(slots) / sizeof(slots[0]))
		return NULL;
	return slots[i].name;
}

static int slot_of(const char *name)
{
	for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
		if (strcmp(name, slots[i].name) == 0)
			return (int)i;
	}
	return -1;
}

/*
 * What the picture actually is, rather than what the file asked for.
 *
 * [stream0] may leave width and height out, and rvd then takes them from
 * the sensor -- so the configured fallback is a guess that a 2688x1520
 * encode makes wrong by half. Rotation diverges the same way: rvd turns the
 * picture only if the backend can and leaves it alone when it cannot, while
 * load_config's swap assumes the turn always happens.
 *
 * Everything rod sizes from these is wrong by whatever the two disagree
 * by -- the sub-stream font ratio, the overlay regions, the clip that
 * keeps a region inside the frame -- so ask the process that knows. It is
 * up: rvd starts first and the init script waits for its ring before
 * starting anything that reads from one.
 *
 * Streams are matched by position, which is not a new assumption: an
 * element's shared buffer is named osd_<index>_<element> and rvd looks it
 * up under its own index, so the two orders already have to agree for any
 * overlay to appear at all.
 */
/* Long enough for a round trip to a daemon that is up, short enough that a
 * daemon that is not does not hold up the overlay. */
#define RVD_ASK_MS 1000

static void dims_from_rvd(rod_state_t *st)
{
	cJSON *q = cJSON_CreateObject();
	char req[64];
	char *resp = NULL;
	bool built;

	if (!q)
		return;
	cJSON_AddStringToObject(q, "cmd", "status");
	built = cJSON_PrintPreallocated(q, req, sizeof(req), 0);
	cJSON_Delete(q);
	if (!built)
		return;

	/*
	 * Allocated rather than into a buffer sized by guess: the reply carries
	 * a line per stream, and a fixed buffer the stream table outgrows would
	 * truncate the JSON into something unparseable without saying so.
	 */
	if (rss_ctrl_send_command_alloc(RSS_RUN_DIR "/rvd.sock", req, &resp, RVD_ASK_MS) < 0) {
		RSS_INFO("rvd did not answer; sizing the overlay from the config");
		return;
	}

	cJSON *root = cJSON_Parse(resp);

	free(resp);
	if (!root)
		return;

	cJSON *streams = cJSON_GetObjectItem(root, "streams");
	cJSON *item;
	int s = 0;

	cJSON_ArrayForEach(item, streams)
	{
		if (s >= st->stream_count)
			break;

		int w = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(item, "w"));
		int h = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(item, "h"));

		if (w > 0 && h > 0) {
			if (w != st->stream_w[s] || h != st->stream_h[s])
				RSS_INFO("stream%d encodes %dx%d, not the configured %dx%d", s, w,
					 h, st->stream_w[s], st->stream_h[s]);
			st->stream_w[s] = w;
			st->stream_h[s] = h;
		}
		s++;
	}

	cJSON_Delete(root);
}

void load_config(rod_state_t *st)
{
	rss_config_t *cfg = st->cfg;
	rod_config_t *c = &st->settings;

	c->enabled = rss_config_get_bool(cfg, "osd", "enabled", true);
	rss_strlcpy(c->font_path,
		    rss_config_get_str(cfg, "osd", "font", "/usr/share/fonts/default.ttf"),
		    sizeof(c->font_path));
	/* Either spelling -- pixels, or a percentage of the stream's own
	 * height -- and a size that is neither leaves the default in place. */
	{
		const char *fs = rss_config_get_str(cfg, "osd", "font_size", "");

		if (!rss_osd_parse_font_size(fs, &c->font_size, &c->font_pct)) {
			c->font_size = 24;
			c->font_pct = 0;
			if (fs[0])
				RSS_WARN("font_size \"%s\" is neither pixels nor a percentage; "
					 "using %d",
					 fs, c->font_size);
		}
	}
	c->font_color = parse_color(rss_config_get_str(cfg, "osd", "font_color", "0xFFFFFFFF"));
	c->stroke_color = parse_color(rss_config_get_str(cfg, "osd", "stroke_color", "0xFF000000"));
	c->font_stroke = rss_config_get_int(cfg, "osd", "font_stroke", 1);
	rss_strlcpy(c->time_format,
		    rss_config_get_str(cfg, "osd", "time_format", "%Y-%m-%d %H:%M:%S"),
		    sizeof(c->time_format));
	c->frame_rate = rss_config_get_int(cfg, "stream0", "fps", 25);
	if (c->frame_rate < 1)
		c->frame_rate = 25;

	st->stream_w[0] = rss_config_get_int(cfg, "stream0", "width", 1920);
	st->stream_h[0] = rss_config_get_int(cfg, "stream0", "height", 1080);
	st->stream_count = 1;

	if (rss_config_get_bool(cfg, "stream1", "enabled", true)) {
		st->stream_w[1] = rss_config_get_int(cfg, "stream1", "width", 640);
		st->stream_h[1] = rss_config_get_int(cfg, "stream1", "height", 360);
		st->stream_count = 2;
	}

	static const char *sensor_sections[] = {"sensor1_stream0", "sensor1_stream1",
						"sensor2_stream0", "sensor2_stream1"};
	for (int i = 0; i < 4 && st->stream_count < ROD_MAX_STREAMS; i++) {
		const char *sec = sensor_sections[i];
		if (rss_config_get_str(cfg, sec, "codec", "")[0] != '\0' ||
		    rss_config_get_bool(cfg, sec, "enabled", false)) {
			int s = st->stream_count;
			st->stream_w[s] = rss_config_get_int(cfg, sec, "width",
							     (i % 2 == 0) ? st->stream_w[0]
									  : st->stream_w[1]);
			st->stream_h[s] = rss_config_get_int(cfg, sec, "height",
							     (i % 2 == 0) ? st->stream_h[0]
									  : st->stream_h[1]);
			st->stream_count++;
		}
	}

	/* [image] rotate: a stream turned 90 or 270 degrees is encoded with
	 * its width and height swapped (rvd_pipeline.c), and the overlay is
	 * placed on the encoded picture. */
	{
		int rotate = rss_config_get_int(cfg, "image", "rotate", 0);

		if (rotate == 90 || rotate == 270) {
			for (int s = 0; s < st->stream_count; s++) {
				int w = st->stream_w[s];

				st->stream_w[s] = st->stream_h[s];
				st->stream_h[s] = w;
			}
		}
	}

	dims_from_rvd(st);

	st->detect_enabled = rss_config_get_bool(cfg, "motion", "enabled", false);
	gethostname(st->hostname, sizeof(st->hostname));
	st->hostname[sizeof(st->hostname) - 1] = '\0';
}

/* ── [osd.*] section parser ── */

/*
 * The elements of the config file, in the order it lists them.
 *
 * rss_config prepends each section as it parses, so a walk hands them back
 * last-first. The names are collected and then loaded from the end, because
 * the order elements are added in is the order a place is claimed in -- and
 * the order the file reads in is the one an author means by it, and the one
 * rcd numbers the console's slots by (rcd_osd.h). The window keeps the first
 * ROD_MAX_ELEMENTS of the file, which are the last the walk reaches.
 */
struct osd_section_ctx {
	rod_state_t *st;
	int count;
	char name[ROD_MAX_ELEMENTS][64];
	int held;
};

static void note_osd_section(const char *section, void *userdata)
{
	struct osd_section_ctx *ctx = userdata;

	if (ctx->held == ROD_MAX_ELEMENTS) {
		memmove(ctx->name[0], ctx->name[1], sizeof(ctx->name[0]) * (ROD_MAX_ELEMENTS - 1));
		ctx->held--;
	}
	rss_strlcpy(ctx->name[ctx->held++], section, sizeof(ctx->name[0]));
}

static void load_osd_section(const char *section, void *userdata)
{
	struct osd_section_ctx *ctx = userdata;
	rod_state_t *st = ctx->st;
	rss_config_t *cfg = st->cfg;

	const char *dot = strchr(section, '.');
	if (!dot)
		return;
	const char *name = dot + 1;
	if (!name[0] || strlen(name) >= ROD_ELEM_NAME_LEN)
		return;

	int slot = slot_of(name);
	const char *type_str = rss_config_get_str(cfg, section, "type", "text");
	const char *tmpl = rss_config_get_str(cfg, section, "template", "");
	const char *position =
		rss_config_get_str(cfg, section, "position", slot >= 0 ? name : "top_left");
	const char *align_str = rss_config_get_str(cfg, section, "align", "");
	int font_size = 0, font_pct = 0;

	/* Neither spelling given is not a failure: the element takes the
	 * overlay's size, which is what most of them do. */
	rss_osd_parse_font_size(rss_config_get_str(cfg, section, "font_size", ""), &font_size,
				&font_pct);
	int max_chars = rss_config_get_int(cfg, section, "max_chars", 20);
	bool visible = rss_config_get_bool(cfg, section, "visible", true);
	bool sub_only = rss_config_get_bool(cfg, section, "sub_only", false);
	const char *update_str = rss_config_get_str(cfg, section, "update", "tick");

	rod_elem_type_t type = ROD_ELEM_TEXT;
	if (strcmp(type_str, "image") == 0)
		type = ROD_ELEM_IMAGE;
	else if (strcmp(type_str, "overlay") == 0)
		type = ROD_ELEM_OVERLAY;
	else if (strcmp(type_str, "receipt") == 0)
		type = ROD_ELEM_RECEIPT;

	/* An explicit align wins; a named slot supplies one; anything else is
	 * left, which is what parse_align answers for the empty string. */
	int align = align_str[0] || slot < 0 ? parse_align(align_str) : slots[slot].align;
	rod_update_mode_t update = ROD_UPDATE_TICK;
	if (strcmp(update_str, "change") == 0)
		update = ROD_UPDATE_CHANGE;

	if (rod_add_element(st, name, type, tmpl, position, align, font_size, max_chars, update) <
	    0)
		return;

	rod_element_t *e = rod_find_element(st, name);
	if (!e)
		return;

	e->visible = visible;
	e->sub_streams_only = sub_only;
	e->font_pct = font_pct;

	if (type == ROD_ELEM_IMAGE) {
		const char *path = rss_config_get_str(cfg, section, "path", "");
		if (path[0])
			rss_strlcpy(e->image_path, path, sizeof(e->image_path));
		e->image_w = rss_config_get_int(cfg, section, "width", 0);
		e->image_h = rss_config_get_int(cfg, section, "height", 0);
	}

	if (type == ROD_ELEM_RECEIPT) {
		e->receipt.max_lines = rss_config_get_int(cfg, section, "max_lines", 20);
		if (e->receipt.max_lines > ROD_RECEIPT_MAX_LINES)
			e->receipt.max_lines = ROD_RECEIPT_MAX_LINES;
		e->receipt.max_line_len = rss_config_get_int(cfg, section, "max_line_length", 80);
		if (e->receipt.max_line_len > ROD_RECEIPT_MAX_LINE)
			e->receipt.max_line_len = ROD_RECEIPT_MAX_LINE;
		e->receipt.accum_timeout = rss_config_get_int(cfg, section, "timeout", 0);
		e->receipt.bg_color =
			parse_color(rss_config_get_str(cfg, section, "bg_color", "0x80000000"));
		e->receipt.input_fd = -1;
		const char *source = rss_config_get_str(cfg, section, "source", "");
		if (strcmp(source, "fifo") == 0) {
			const char *dev = rss_config_get_str(cfg, section, "device", "");
			if (dev[0]) {
				rss_strlcpy(e->receipt.input_path, dev,
					    sizeof(e->receipt.input_path));
				mkfifo(dev, 0666);
				int fd = open(dev, O_RDONLY | O_NONBLOCK);
				if (fd >= 0) {
					e->receipt.input_fd = fd;
					RSS_INFO("receipt '%s': opened FIFO %s", name, dev);
				} else {
					RSS_WARN("receipt '%s': failed to open FIFO %s: %s", name,
						 dev, strerror(errno));
				}
			}
		} else if (strcmp(source, "uart") == 0) {
			const char *dev = rss_config_get_str(cfg, section, "device", "");
			if (dev[0]) {
				rss_strlcpy(e->receipt.input_path, dev,
					    sizeof(e->receipt.input_path));
				int fd = open(dev, O_RDONLY | O_NOCTTY | O_NONBLOCK);
				if (fd >= 0) {
					e->receipt.input_fd = fd;
					RSS_INFO("receipt '%s': opened UART %s", name, dev);
				} else {
					RSS_WARN("receipt '%s': failed to open %s: %s", name, dev,
						 strerror(errno));
				}
			}
		}
	}

	int stroke = rss_config_get_int(cfg, section, "stroke_size", -1);
	if (stroke >= 0)
		e->stroke_size = stroke;

	const char *color_str = rss_config_get_str(cfg, section, "color", NULL);
	if (color_str) {
		e->color = (uint32_t)strtoul(color_str, NULL, 0);
		e->has_color = true;
	}

	const char *sc_str = rss_config_get_str(cfg, section, "stroke_color", NULL);
	if (sc_str) {
		e->stroke_color = (uint32_t)strtoul(sc_str, NULL, 0);
		e->has_stroke_color = true;
	}

	ctx->count++;
	RSS_DEBUG("osd element from config: [%s] name=%s type=%s", section, name, type_str);
}

/*
 * "Is this camera's video reachable by anyone who can route to it?"
 *
 * rsd serves RTSP with Digest only when [rtsp] has both a username and a
 * password; rhd's snapshot and MJPEG routes are Basic-gated on the same terms
 * from [http]. Either one left unset is a camera anybody on the network can
 * watch. rhd's configuration route is not part of this -- it authenticates
 * against the system account in /etc/shadow, whatever raptor.conf says.
 *
 * [system] unsafe = true is the way to say the openness is deliberate: a feed
 * into an NVR on an isolated VLAN is a real thing to want. It is a positive
 * statement in the config file rather than a default, so the file records that
 * somebody meant it.
 */
static bool section_has_credentials(rss_config_t *cfg, const char *section)
{
	return rss_config_get_str(cfg, section, "username", "")[0] != '\0' &&
	       rss_config_get_str(cfg, section, "password", "")[0] != '\0';
}

static bool media_is_open(rss_config_t *cfg)
{
	if (rss_config_get_bool(cfg, "system", "unsafe", false))
		return false;
	return !section_has_credentials(cfg, "rtsp") || !section_has_credentials(cfg, "http");
}

void init_elements_from_config(rod_state_t *st)
{
	struct osd_section_ctx ctx = {.st = st, .count = 0, .held = 0};

	rss_config_foreach_section(st->cfg, "osd.", note_osd_section, &ctx);
	for (int i = ctx.held - 1; i >= 0; i--)
		load_osd_section(ctx.name[i], &ctx);

	if (ctx.count == 0)
		RSS_WARN("no [osd.*] sections found in config -- OSD will be empty");
	else
		RSS_INFO("loaded %d OSD elements from config", ctx.count);

	if (!rod_find_element(st, "privacy")) {
		rod_add_element(st, "privacy", ROD_ELEM_TEXT, "Privacy Mode", "center", 1, 0, 20,
				ROD_UPDATE_CHANGE);
		rod_element_t *e = rod_find_element(st, "privacy");
		if (e)
			e->visible = false;
	}

	/*
	 * The unsecured banner.
	 *
	 * An open stream is convenient and is the right default on a bench: it
	 * is what makes a freshly flashed camera useful without configuring
	 * anything. The failure it invites is nobody closing it afterwards, and
	 * that failure is silent -- a log line goes to a console no one reads,
	 * and documentation is read before the mistake rather than after.
	 *
	 * The video is the one surface the person who can fix this is certainly
	 * looking at, and they are looking at it during exactly the phase when
	 * the fix is cheap. So the warning goes there. It costs an OSD region,
	 * it clears itself the moment a credential is set, and it never refuses
	 * to stream -- convenience is the point, and blocking would spend it.
	 */
	if (media_is_open(st->cfg) && !rod_find_element(st, "unsecured")) {
		rod_add_element(st, "unsecured", ROD_ELEM_TEXT, "UNSECURED - NO PASSWORD SET",
				"bottom_center", 1, 0, 40, ROD_UPDATE_CHANGE);
	}

	if (st->detect_enabled && !rod_find_element(st, "detect")) {
		rod_add_element(st, "detect", ROD_ELEM_OVERLAY, NULL, "0,0", 0, 0, 0,
				ROD_UPDATE_TICK);
		rod_element_t *e = rod_find_element(st, "detect");
		if (e)
			e->sub_streams_only = true;
	}
}
