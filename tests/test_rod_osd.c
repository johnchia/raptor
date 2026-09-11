/*
 * test_rod_osd.c -- Which elements rod gives a buffer to.
 *
 * rod's buffers are what rvd builds overlay regions from, one region per
 * buffer, and a region costs more than the picture in it: a slot in the
 * backend's table, a share of a pool sized once when the pipeline came up,
 * and -- where the compositor gives a pixel to one region only, as
 * SigmaStar's does -- the place it sits. An element drawing nothing that
 * holds all three is an element that blanks another, which is what these
 * tests are about.
 *
 * They run the real rod_elem.c with real SHM buffers, and ask the question
 * rvd asks: is there a buffer under this name?
 */

#include <string.h>
#include <stdio.h>
#include <sys/mman.h>

#include "greatest.h"
#include "../rod/rod.h"

/*
 * rod_elem.c reaches the glyph cache only to size a text element's buffer.
 * These elements are overlays, whose size is the picture's, so the renderer
 * is linked out and stubbed here.
 */
int rod_render_init(rod_state_t *st, int stream_idx, int font_idx, int font_size)
{
	(void)st;
	(void)stream_idx;
	(void)font_idx;
	(void)font_size;
	return -1;
}

void rod_render_deinit(rod_state_t *st, int stream_idx, int font_idx)
{
	(void)st;
	(void)stream_idx;
	(void)font_idx;
}

static rod_state_t st;

/*
 * A buffer outlives the process that made it, and a failed assertion returns
 * before the teardown that would have removed it -- so one run's leftovers
 * would answer the next run's question, and a test that had failed once would
 * go on failing. Each test starts by forgetting the names it is about to use.
 */
static void forget(const char *name)
{
	char obj[80];

	for (int s = 0; s < 2; s++) {
		snprintf(obj, sizeof(obj), "/rss_osd_osd_%d_%s", s, name);
		shm_unlink(obj);
	}
}

static void setup(void)
{
	memset(&st, 0, sizeof(st));
	forget("clock");
	forget("spare");
	forget("logo");
	st.stream_count = 2;
	st.stream_w[0] = 320;
	st.stream_h[0] = 240;
	st.stream_w[1] = 160;
	st.stream_h[1] = 120;
}

static void teardown(void)
{
	for (int i = 0; i < st.elem_count; i++) {
		if (st.elements[i].active)
			rod_remove_element(&st, st.elements[i].name);
	}
}

/* An element whose size needs no font, so no glyph cache is needed to get one. */
static void element(const char *name, const char *position)
{
	rod_add_element(&st, name, ROD_ELEM_OVERLAY, "", position, 0, 0, 20, ROD_UPDATE_TICK);
}

/* rvd's question, asked the way rvd asks it. */
static bool has_buffer(const char *name, int s)
{
	char shm_name[64];
	rss_osd_shm_t *c;

	snprintf(shm_name, sizeof(shm_name), "osd_%d_%s", s, name);
	c = rss_osd_open(shm_name);
	if (!c)
		return false;
	rss_osd_close(c);
	return true;
}

/* ══════════════════════════════════════════════════════════════════
 *  A buffer lasts as long as there is something to draw in it
 * ══════════════════════════════════════════════════════════════════ */

TEST an_element_that_draws_gets_a_buffer_on_every_stream(void)
{
	setup();
	element("clock", "top_left");
	rod_sync_shms(&st);

	ASSERT(has_buffer("clock", 0));
	ASSERT(has_buffer("clock", 1));

	teardown();
	PASS();
}

/*
 * The bug this file exists for. An element hidden in the config still had a
 * buffer, so rvd still built it a region, and on a part that gives a pixel to
 * one region only that region took the corner away from the element that was
 * drawing there -- which then vanished from the picture with nothing in the
 * log to say why.
 */
TEST a_hidden_element_holds_no_buffer(void)
{
	setup();
	element("spare", "top_left");
	rod_find_element(&st, "spare")->visible = false;
	rod_sync_shms(&st);

	ASSERTm("a hidden element reserved a region it draws nothing in", !has_buffer("spare", 0));
	ASSERT(!has_buffer("spare", 1));

	teardown();
	PASS();
}

TEST hiding_an_element_gives_its_buffer_back(void)
{
	setup();
	element("clock", "top_left");
	rod_sync_shms(&st);
	ASSERT(has_buffer("clock", 0));

	rod_find_element(&st, "clock")->visible = false;
	rod_sync_shms(&st);

	ASSERTm("hiding an element left its region behind", !has_buffer("clock", 0));
	ASSERT(!has_buffer("clock", 1));

	/* And it is the same element still: showing it again draws it again. */
	rod_find_element(&st, "clock")->visible = true;
	rod_sync_shms(&st);
	ASSERT(has_buffer("clock", 0));
	ASSERT(has_buffer("clock", 1));

	teardown();
	PASS();
}

/* A sub-only element is absent from the main stream for the same reason and
 * by the same route, which is worth pinning now that one route decides. */
TEST a_sub_only_element_takes_nothing_from_the_main_stream(void)
{
	setup();
	element("logo", "bottom_right");
	rod_find_element(&st, "logo")->sub_streams_only = true;
	rod_sync_shms(&st);

	ASSERT(!has_buffer("logo", 0));
	ASSERT(has_buffer("logo", 1));

	teardown();
	PASS();
}

SUITE(rod_osd_suite)
{
	RUN_TEST(an_element_that_draws_gets_a_buffer_on_every_stream);
	RUN_TEST(a_hidden_element_holds_no_buffer);
	RUN_TEST(hiding_an_element_gives_its_buffer_back);
	RUN_TEST(a_sub_only_element_takes_nothing_from_the_main_stream);
}
