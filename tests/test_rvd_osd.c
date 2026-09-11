/*
 * test_rvd_osd.c -- The overlay region lifecycle in rvd.
 *
 * A region is not only a picture. It is a slot in the backend's region table
 * and a share of an OSD pool sized once, at pipeline init, and neither comes
 * back by drawing nothing -- so what rvd does when a producer goes away
 * decides whether a camera can be reconfigured twice. These tests run the
 * real rvd_osd.c against the mock backend's finite region table, with real
 * SHM buffers standing in for rod.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#include "greatest.h"
#include "../rvd/rvd.h"

/* mock_hal.c: the backend's region table, which is what a leak shows up in. */
void mock_osd_reset(void);
void mock_osd_set_capacity(int n);
int mock_osd_live_regions(void);
int mock_osd_region_creates(void);

static rvd_state_t st;
static rss_hal_ctx_t *test_hal;
static rss_config_t *test_cfg;

static void setup(void)
{
	memset(&st, 0, sizeof(st));
	mock_osd_reset();

	test_hal = rss_hal_create();
	st.hal_ctx = test_hal;
	st.ops = rss_hal_get_ops(test_hal);

	test_cfg = rss_config_load("/dev/null");
	st.cfg = test_cfg;
	st.config_path = "/dev/null";

	pthread_mutex_init(&st.osd_lock, NULL);
	st.osd_enabled = true;

	st.stream_count = 1;
	st.streams[0].chn = 0;
	st.streams[0].fs_chn = 0;
	st.streams[0].enc_cfg.codec = RSS_CODEC_H264;
	st.streams[0].enc_cfg.width = 1920;
	st.streams[0].enc_cfg.height = 1080;
	snprintf(st.streams[0].cfg_sect, sizeof(st.streams[0].cfg_sect), "stream0");
	for (int s = 0; s < RVD_MAX_STREAMS; s++)
		st.privacy_handles[s] = -1;
}

static void teardown(void)
{
	rvd_osd_deinit_stream(&st, 0);
	rss_config_free(test_cfg);
	rss_hal_destroy(test_hal);
	pthread_mutex_destroy(&st.osd_lock);
}

/* rod's side: a buffer under the name rvd scans for. */
static rss_osd_shm_t *producer(const char *name)
{
	char shm_name[64];

	snprintf(shm_name, sizeof(shm_name), "osd_0_%s", name);
	return rss_osd_create(shm_name, 64, 32);
}

/* The 10 Hz OSD tick, run n times. */
static void tick(int n)
{
	for (int i = 0; i < n; i++)
		rvd_osd_check(&st);
}

/* ══════════════════════════════════════════════════════════════════
 *  A region lives exactly as long as the element behind it
 * ══════════════════════════════════════════════════════════════════ */

TEST an_element_that_goes_takes_its_region_with_it(void)
{
	setup();
	rss_osd_shm_t *shm = producer("ticker");
	ASSERT(shm != NULL);

	rvd_osd_init_stream(&st, 0);
	tick(10); /* the region takes its mapping on the first second's tick */

	rvd_osd_region_t *reg = rvd_osd_find_region(&st, 0, "ticker");
	ASSERT(reg != NULL);
	ASSERT(reg->hal_handle >= 0);

	int live = mock_osd_live_regions();

	/* rod removes the element: the buffer is unlinked under us. */
	rss_osd_destroy(shm);
	tick(10);

	ASSERTm("the region outlived its element", rvd_osd_find_region(&st, 0, "ticker") == NULL);
	ASSERT_EQm("the backend's region was not given back", live - 1, mock_osd_live_regions());

	teardown();
	PASS();
}

/*
 * The bug this file exists for: elements that come and go used up the pool,
 * a share at a time, until a camera that had been reconfigured a few times
 * stopped drawing anything new -- the create failed for want of a region and
 * only the log said so.
 */
TEST elements_that_come_and_go_do_not_use_the_backend_up(void)
{
	setup();
	rvd_osd_init_stream(&st, 0);
	/* Room for two overlays at a time, and twelve asked for one by one. */
	mock_osd_set_capacity(mock_osd_live_regions() + 2);

	for (int i = 0; i < 12; i++) {
		char name[32];

		snprintf(name, sizeof(name), "churn%d", i);
		rss_osd_shm_t *shm = producer(name);
		ASSERT(shm != NULL);

		tick(RVD_OSD_RETRY_INTERVAL);
		ASSERTm("an element added later never got a region",
			rvd_osd_find_region(&st, 0, name) != NULL);

		rss_osd_destroy(shm);
		tick(10);
		ASSERT(rvd_osd_find_region(&st, 0, name) == NULL);
	}

	teardown();
	PASS();
}

/*
 * A producer that replaces its buffer -- rod restarting, or resizing an
 * element -- is not a producer that has gone, and its region stays put. The
 * two look alike for a moment: the name is there, the buffer behind it is a
 * different one.
 */
TEST a_producer_that_comes_straight_back_keeps_its_region(void)
{
	setup();
	rss_osd_shm_t *shm = producer("ticker");
	ASSERT(shm != NULL);
	rvd_osd_init_stream(&st, 0);
	tick(10);

	rvd_osd_region_t *reg = rvd_osd_find_region(&st, 0, "ticker");
	ASSERT(reg != NULL);
	int live = mock_osd_live_regions();
	int creates = mock_osd_region_creates();

	rss_osd_destroy(shm);
	shm = producer("ticker");
	ASSERT(shm != NULL);
	tick(10);

	reg = rvd_osd_find_region(&st, 0, "ticker");
	ASSERT(reg != NULL);
	ASSERT_EQm("the region was rebuilt rather than reopened", creates,
		   mock_osd_region_creates());
	ASSERT_EQ(live, mock_osd_live_regions());

	rss_osd_destroy(shm);
	teardown();
	PASS();
}

/*
 * time and uptime share one region on T20, because two regions spanning
 * opposite edges of the same scanline stall the IPU. The one merged in has no
 * hardware of its own, so it cannot outlive its host -- and when the host
 * goes it is owed a region of its own rather than silence.
 */
TEST losing_the_host_of_a_merge_gives_the_other_a_region_of_its_own(void)
{
	setup();
	rss_osd_shm_t *time_shm = producer("time");
	ASSERT(time_shm != NULL);
	rvd_osd_init_stream(&st, 0);

	rss_osd_shm_t *up_shm = producer("uptime");
	ASSERT(up_shm != NULL);
	tick(RVD_OSD_RETRY_INTERVAL);

	rvd_osd_region_t *up = rvd_osd_find_region(&st, 0, "uptime");
	ASSERT(up != NULL);
	ASSERT_EQm("uptime was expected to be merged into time", -1, up->hal_handle);

	rss_osd_destroy(time_shm);
	tick(10);
	ASSERT(rvd_osd_find_region(&st, 0, "time") == NULL);

	tick(RVD_OSD_RETRY_INTERVAL);
	up = rvd_osd_find_region(&st, 0, "uptime");
	ASSERT(up != NULL);
	ASSERTm("uptime was left with nothing to draw into", up->hal_handle >= 0);

	rss_osd_destroy(up_shm);
	teardown();
	PASS();
}

SUITE(rvd_osd_suite)
{
	RUN_TEST(an_element_that_goes_takes_its_region_with_it);
	RUN_TEST(elements_that_come_and_go_do_not_use_the_backend_up);
	RUN_TEST(a_producer_that_comes_straight_back_keeps_its_region);
	RUN_TEST(losing_the_host_of_a_merge_gives_the_other_a_region_of_its_own);
}
