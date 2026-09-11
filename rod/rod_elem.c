/*
 * rod_elem.c -- Element registry, font management, SHM lifecycle
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rod.h"

void sanitize_text(char *s)
{
	for (int i = 0; s[i]; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c < 0x20)
			s[i] = ' ';
		if (c > 0x7E)
			s[i] = '?';
	}
}

uint32_t parse_color(const char *s)
{
	if (!s)
		return 0xFFFFFFFF;
	return (uint32_t)strtoul(s, NULL, 0);
}

/* ── Element registry ── */

rod_element_t *rod_find_element(rod_state_t *st, const char *name)
{
	for (int i = 0; i < st->elem_count; i++) {
		if (st->elements[i].active && strcmp(st->elements[i].name, name) == 0)
			return &st->elements[i];
	}
	return NULL;
}

/*
 * The way back from rss_osd_parse_font_size: a size that arrived over the
 * wire, in a spelling that parser reads the same way again.
 */
void rod_format_font_size(char *out, size_t n, int px, int pct)
{
	if (pct <= 0)
		snprintf(out, n, "%d", px);
	else if (pct % 10)
		snprintf(out, n, "%d.%d%%", pct / 10, pct % 10);
	else
		snprintf(out, n, "%d%%", pct / 10);
}

/*
 * The size that reaches the glyph cache, for one element on one stream.
 *
 * Legibility is a share of the picture either way; the two spellings differ
 * only in where that share is worked out from. A percentage is already per
 * stream and needs no reference. Pixels are pixels on the main stream, and
 * the same count on a quarter-height sub stream is four times the text, so
 * they are scaled by that stream's share of the main's height -- which also
 * keeps a 16:9 and a 4:3 encode of one picture looking alike.
 *
 * The bounds are on the sizes rod works out and not on the one somebody
 * typed, which is honoured as typed. Below the floor the glyph cache stops
 * being legible rather than merely small, and text too small to read is worse
 * than text that overruns its region -- the region is clipped to the frame,
 * so an overrun is at least visible. The ceiling is a share because a caption
 * is one: a line taking a quarter of the frame has stopped being an overlay.
 * It cannot be reached from the config surface, whose range sits well inside
 * it, and catches the hand-edit -- where a misplaced digit in a percentage is
 * a factor of ten.
 */
#define ROD_FONT_MIN 12

static int derived_size(int size, int stream_h)
{
	int max = stream_h / 4;

	if (max > 0 && size > max)
		size = max;
	return size < ROD_FONT_MIN ? ROD_FONT_MIN : size;
}

int rod_font_for_elem(const rod_state_t *st, const rod_element_t *e, int stream_idx)
{
	int px = st->settings.font_size;
	int pct = st->settings.font_pct;
	int h;

	/* An element's own size wins, in whichever spelling it gave it; an
	 * element that gave neither takes the overlay's. */
	if (e && (e->font_size > 0 || e->font_pct > 0)) {
		px = e->font_size;
		pct = e->font_pct;
	}

	if (stream_idx < 0 || stream_idx >= ROD_MAX_STREAMS)
		return px;
	h = st->stream_h[stream_idx];
	if (h <= 0)
		return px;

	if (pct > 0)
		return derived_size(h * pct / 1000, h);
	if (stream_idx > 0 && st->stream_h[0] > 0)
		return derived_size(px * h / st->stream_h[0], h);
	return px;
}

int rod_alloc_font(rod_state_t *st, int stream_idx, int font_size)
{
	/* Reuse existing font context with same size */
	for (int i = 0; i < ROD_MAX_FONTS; i++) {
		if (st->fonts[stream_idx][i].refcount > 0 &&
		    st->fonts[stream_idx][i].size == font_size) {
			st->fonts[stream_idx][i].refcount++;
			return i;
		}
	}
	/* Allocate new slot */
	for (int i = 0; i < ROD_MAX_FONTS; i++) {
		if (st->fonts[stream_idx][i].refcount == 0) {
			if (rod_render_init(st, stream_idx, i, font_size) < 0)
				return -1;
			st->fonts[stream_idx][i].size = font_size;
			st->fonts[stream_idx][i].refcount = 1;
			return i;
		}
	}
	RSS_ERROR("font pool exhausted for stream %d", stream_idx);
	return -1;
}

void release_font(rod_state_t *st, int stream_idx, int font_idx)
{
	if (font_idx < 0 || font_idx >= ROD_MAX_FONTS)
		return;
	rod_font_t *f = &st->fonts[stream_idx][font_idx];
	if (f->refcount > 0)
		f->refcount--;
	if (f->refcount == 0)
		rod_render_deinit(st, stream_idx, font_idx);
}

int rod_add_element(rod_state_t *st, const char *name, rod_elem_type_t type, const char *tmpl,
		    const char *position, int align, int font_size, int max_chars,
		    rod_update_mode_t update_mode)
{
	if (rod_find_element(st, name))
		return -1;

	rod_element_t *e = NULL;
	for (int i = 0; i < st->elem_count; i++) {
		if (!st->elements[i].active) {
			e = &st->elements[i];
			break;
		}
	}
	if (!e) {
		if (st->elem_count >= ROD_MAX_ELEMENTS)
			return -1;
		e = &st->elements[st->elem_count++];
	}
	memset(e, 0, sizeof(*e));
	rss_strlcpy(e->name, name, sizeof(e->name));
	e->type = type;
	e->active = true;
	e->visible = true;
	if (tmpl)
		rss_strlcpy(e->tmpl, tmpl, sizeof(e->tmpl));
	if (position)
		rss_strlcpy(e->position, position, sizeof(e->position));
	e->align = align;
	e->font_size = font_size;
	e->stroke_size = -1;
	e->max_chars = max_chars > 0 ? max_chars : 20;
	if (e->max_chars > 128)
		e->max_chars = 128;
	e->update_mode = update_mode;
	for (int s = 0; s < ROD_MAX_STREAMS; s++)
		e->streams[s].font_idx = -1;

	return 0;
}

void rod_remove_element(rod_state_t *st, const char *name)
{
	for (int i = 0; i < st->elem_count; i++) {
		if (!st->elements[i].active || strcmp(st->elements[i].name, name) != 0)
			continue;

		rod_element_t *e = &st->elements[i];
		for (int s = 0; s < st->stream_count; s++) {
			destroy_elem_shm(e, s);
			release_font(st, s, e->streams[s].font_idx);
			e->streams[s].font_idx = -1;
		}
		free(e->image_data);
		free(e->image_sub_data);
		if (e->receipt.input_fd >= 0) {
			close(e->receipt.input_fd);
			e->receipt.input_fd = -1;
		}
		e->active = false;
		return;
	}
}

/* ── Element state helpers ── */

void mark_all_dirty(rod_state_t *st)
{
	for (int i = 0; i < st->elem_count; i++) {
		if (!st->elements[i].active)
			continue;
		for (int s = 0; s < st->stream_count; s++)
			st->elements[i].streams[s].needs_update = true;
	}
}

void mark_element_dirty(rod_element_t *e, int stream_count)
{
	for (int s = 0; s < stream_count; s++)
		e->streams[s].needs_update = true;
}

/* ── SHM creation ── */

static void font_region_dims(rod_font_t *f, int chars, int stroke, uint32_t *w, uint32_t *h)
{
	int adv = f->max_text_width / 24;
	if (adv < 10)
		adv = 10;
	int pad = stroke > 0 ? stroke * 2 : 0;
	*w = chars * adv + pad;
	*h = f->text_height;
}

void create_elem_shm(rod_state_t *st, rod_element_t *e, int s)
{
	char name[64];
	snprintf(name, sizeof(name), "osd_%d_%s", s, e->name);

	uint32_t w, h;

	if (e->type == ROD_ELEM_TEXT) {
		if (e->streams[s].font_idx < 0)
			return;
		int stroke = e->stroke_size >= 0 ? e->stroke_size : st->settings.font_stroke;
		font_region_dims(&st->fonts[s][e->streams[s].font_idx], e->max_chars, stroke, &w,
				 &h);
	} else if (e->type == ROD_ELEM_IMAGE) {
		if (s == 0) {
			w = e->image_w;
			h = e->image_h;
		} else {
			/* The sub bitmap's own size, and no region at all
			 * without one: an image that did not load draws
			 * nothing, and a region reserved for it is a hole in
			 * the pool that stays blank for as long as the camera
			 * runs. */
			w = e->image_sub_w;
			h = e->image_sub_h;
		}
		if (w == 0 || h == 0)
			return;
	} else if (e->type == ROD_ELEM_RECEIPT) {
		if (e->streams[s].font_idx < 0)
			return;
		rod_font_t *f = &st->fonts[s][e->streams[s].font_idx];
		int adv = f->max_text_width / 24;
		if (adv < 10)
			adv = 10;
		int max_ll = e->receipt.max_line_len > 0 ? e->receipt.max_line_len : 80;
		w = max_ll * adv;
		int max_ln = e->receipt.max_lines > 0 ? e->receipt.max_lines : 20;
		h = max_ln * f->text_height;
	} else if (e->type == ROD_ELEM_OVERLAY) {
		w = st->stream_w[s];
		h = st->stream_h[s];
	} else {
		return;
	}

	/*
	 * Text and receipt sizes are a character count times the font metrics,
	 * and neither count knows the frame size, so the sizes above are
	 * unbounded. Clip: text that runs off the region is visible, a region
	 * larger than the canvas is not.
	 */
	if (st->stream_w[s] > 0 && w > (uint32_t)st->stream_w[s])
		w = (uint32_t)st->stream_w[s];
	if (st->stream_h[s] > 0 && h > (uint32_t)st->stream_h[s])
		h = (uint32_t)st->stream_h[s];

	w = (w + 1) & ~1u;
	h = (h + 1) & ~1u;

	rss_osd_shm_t *shm = rss_osd_create(name, w, h);
	if (!shm) {
		RSS_WARN("failed to create OSD SHM: %s", name);
		return;
	}

	e->streams[s].shm = shm;
	e->streams[s].width = w;
	e->streams[s].height = h;
	e->streams[s].needs_update = true;

	RSS_INFO("osd shm %s: %ux%u", name, w, h);
}

void destroy_elem_shm(rod_element_t *e, int s)
{
	if (!e->streams[s].shm)
		return;

	rss_osd_destroy(e->streams[s].shm);
	e->streams[s].shm = NULL;
	e->streams[s].width = 0;
	e->streams[s].height = 0;
}

/*
 * Whether this element puts anything on this stream's picture.
 *
 * An element drawing nothing is not simply an element nobody can see. rvd
 * gives every buffer here a region of its own, and a region is a slot, a share
 * of an overlay pool sized once when the pipeline came up, and -- where the
 * compositor gives a pixel to one region only, as SigmaStar's does -- the
 * place it sits, which is then no longer free for the element that would have
 * drawn there. A hidden element holding all three is how one blanks another.
 *
 * So the buffer goes when the drawing goes, and comes back with it: rvd
 * releases a region whose buffer has gone, and builds one again for a buffer
 * that reappears.
 */
static bool elem_draws(const rod_element_t *e, int s)
{
	if (!e->active || !e->visible)
		return false;
	if (e->sub_streams_only && s == 0)
		return false;
	/* Text is the whole of what a text element draws, so one with none is
	 * one switched off -- and an element with nothing in it is how a place
	 * ends up held by something invisible. The other types keep their
	 * buffers empty on purpose: a receipt fills as lines arrive, and an
	 * overlay is drawn into from outside. An image with no bitmap takes
	 * nothing either, which create_elem_shm settles by size. */
	if (e->type == ROD_ELEM_TEXT && !e->tmpl[0])
		return false;
	return true;
}

/* Where an element sits; an element that named no place sits where rvd puts
 * one that named none. */
static const char *elem_place(const rod_element_t *e)
{
	return e->position[0] ? e->position : "top_left";
}

/*
 * Bring the buffers into line with what the elements now draw.
 *
 * Called after anything that can change that answer rather than from each
 * place that changes it, so that adding, removing, hiding, showing and moving
 * all reach the same decision by the same route.
 *
 * Two elements in one place is the other thing decided here. Where the
 * compositor gives a pixel to one region only, the second of them is not
 * blended with the first: it replaces it, over the whole rectangle, so the
 * element underneath disappears rather than being drawn over. Nothing about
 * the parts makes which one survives predictable -- rvd builds its regions in
 * the order a directory walk hands the buffers back -- so the place goes to
 * the element that asked for it first, which is the order the config file
 * lists them in, and the one that missed is told so rather than left to be
 * discovered by its absence.
 *
 * Places are compared as written, which settles the named corners exactly and
 * says nothing about two elements at nearby x,y coordinates: those are the
 * author's to lay out, and their rectangles are not rod's to know.
 */
void rod_sync_shms(rod_state_t *st)
{
	for (int s = 0; s < st->stream_count; s++) {
		const char *taken[ROD_MAX_ELEMENTS];
		int claims = 0;

		for (int i = 0; i < st->elem_count; i++) {
			rod_element_t *e = &st->elements[i];
			bool draws = elem_draws(e, s);
			bool blocked = false;

			for (int c = 0; draws && c < claims; c++)
				blocked = blocked || strcmp(taken[c], elem_place(e)) == 0;

			if (blocked != e->streams[s].place_taken) {
				e->streams[s].place_taken = blocked;
				if (blocked)
					RSS_WARN("osd %d/%s: %s is already drawn on; this "
						 "element is not shown there",
						 s, e->name, elem_place(e));
			}

			if (draws && !blocked) {
				if (!e->streams[s].shm)
					create_elem_shm(st, e, s);
			} else {
				destroy_elem_shm(e, s);
			}

			if (e->streams[s].shm && claims < ROD_MAX_ELEMENTS)
				taken[claims++] = elem_place(e);
		}
	}
}
