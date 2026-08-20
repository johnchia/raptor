/*
 * test_config.c -- rss_config getter/default semantics.
 *
 * Pins the display-only nature of stored defaults: a getter's miss
 * path records its fallback so config-get-section can show resolved
 * values, but that record must never act as configuration -- not for
 * a later reader with a different fallback (rvd cached 1920x1080
 * before the sensor was known and every 720p camera upscaled), not
 * for a presence probe, and not for a save to a fresh file.
 */

#include "greatest.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <rss_common.h>

/* An empty config object via an empty temp file (load returns NULL
 * for a missing path). Caller removes the file. */
static rss_config_t *empty_cfg(char *path, size_t cap)
{
	snprintf(path, cap, "/tmp/rss_cfg_test_%d_%ld.conf", getpid(), (long)random());
	FILE *f = fopen(path, "w");
	if (!f)
		return NULL;
	fclose(f);
	return rss_config_load(path);
}

TEST cfg_default_is_display_only(void)
{
	char path[128];
	rss_config_t *cfg = empty_cfg(path, sizeof(path));
	ASSERT(cfg);

	/* First reader guesses 1920; the later reader knows the sensor
	 * is 720p wide. The first guess must not win. */
	ASSERT_EQ(1920, rss_config_get_int(cfg, "stream0", "width", 1920));
	ASSERT_EQ(1280, rss_config_get_int(cfg, "stream0", "width", 1280));
	ASSERT_EQ(0, rss_config_get_int(cfg, "stream0", "width", 0));

	/* Same for strings and bools. */
	ASSERT_STR_EQ("aac", rss_config_get_str(cfg, "audio", "codec", "aac"));
	ASSERT_STR_EQ("opus", rss_config_get_str(cfg, "audio", "codec", "opus"));
	ASSERT_EQ(true, rss_config_get_bool(cfg, "x", "flag", true));
	ASSERT_EQ(false, rss_config_get_bool(cfg, "x", "flag", false));

	rss_config_free(cfg);
	unlink(path);
	PASS();
}

static void count_key_cb(const char *key, const char *value, void *ud)
{
	char *out = ud;
	if (strcmp(key, "width") == 0)
		snprintf(out, 32, "%s", value);
}

TEST cfg_default_still_visible_to_display(void)
{
	char path[128];
	rss_config_t *cfg = empty_cfg(path, sizeof(path));
	ASSERT(cfg);

	(void)rss_config_get_int(cfg, "stream0", "width", 1920);
	(void)rss_config_get_int(cfg, "stream0", "width", 1280);

	/* config-get-section iterates entries: the display value tracks
	 * the most recent reader (during boot the authoritative reader
	 * runs last, so the display shows what the daemon uses). */
	char seen[32] = "";
	rss_config_foreach(cfg, "stream0", count_key_cb, seen);
	ASSERT_STR_EQ("1280", seen);

	rss_config_free(cfg);
	unlink(path);
	PASS();
}

TEST cfg_file_value_beats_every_default(void)
{
	char path[128];
	snprintf(path, sizeof(path), "/tmp/rss_cfg_test_%d_f.conf", getpid());
	FILE *f = fopen(path, "w");
	ASSERT(f);
	fputs("[stream0]\nwidth = 1280\n", f);
	fclose(f);

	rss_config_t *cfg = rss_config_load(path);
	ASSERT(cfg);
	ASSERT_EQ(1280, rss_config_get_int(cfg, "stream0", "width", 1920));
	ASSERT_EQ(1280, rss_config_get_int(cfg, "stream0", "width", 0));

	rss_config_free(cfg);
	unlink(path);
	PASS();
}

TEST cfg_set_makes_it_real(void)
{
	char path[128];
	rss_config_t *cfg = empty_cfg(path, sizeof(path));
	ASSERT(cfg);

	(void)rss_config_get_int(cfg, "a", "k", 100); /* display-only */
	ASSERT_FALSE(rss_config_has_dirty(cfg));

	rss_config_set_int(cfg, "a", "k", 42);
	ASSERT(rss_config_has_dirty(cfg));
	ASSERT_EQ(42, rss_config_get_int(cfg, "a", "k", 999));

	rss_config_free(cfg);
	unlink(path);
	PASS();
}

/* A set without a section has nowhere real to land: the writer would
 * emit it above the first [section] header, where no loader reads it
 * again. The setter refuses instead of persisting into oblivion -- the
 * enabling half of the JPEG-channel empty-cfg_sect bug (PR #31). */
TEST cfg_set_refuses_an_empty_section(void)
{
	char path[128];
	rss_config_t *cfg = empty_cfg(path, sizeof(path));
	ASSERT(cfg);

	rss_config_set_int(cfg, "", "orphan", 1);
	rss_config_set_str(cfg, NULL, "orphan2", "x");
	ASSERT_FALSE(rss_config_has_dirty(cfg));
	ASSERT_EQ(0, rss_config_get_int(cfg, "", "orphan", 0));

	/* A real section still works exactly as before. */
	rss_config_set_int(cfg, "a", "k", 7);
	ASSERT(rss_config_has_dirty(cfg));
	ASSERT_EQ(7, rss_config_get_int(cfg, "a", "k", 0));

	rss_config_free(cfg);
	unlink(path);
	PASS();
}

TEST cfg_null_probe_is_order_free(void)
{
	char path[128];
	rss_config_t *cfg = empty_cfg(path, sizeof(path));
	ASSERT(cfg);

	/* ric's ir940 probe pattern: "does the config carry the key" --
	 * historically it HAD to run before the get_bool or the stored
	 * default made the key look present forever. Now both orders
	 * answer the same. */
	ASSERT_EQ(NULL, rss_config_get_str(cfg, "ircut", "ir940", NULL));
	(void)rss_config_get_bool(cfg, "ircut", "ir940", false);
	ASSERT_EQ(NULL, rss_config_get_str(cfg, "ircut", "ir940", NULL));

	rss_config_free(cfg);
	unlink(path);
	PASS();
}

TEST cfg_fresh_file_save_skips_defaults(void)
{
	char path[128];
	rss_config_t *cfg = empty_cfg(path, sizeof(path));
	ASSERT(cfg);
	unlink(path); /* force the whole-config write path on save */

	(void)rss_config_get_int(cfg, "stream0", "width", 1920); /* display-only */
	rss_config_set_int(cfg, "rtsp", "port", 8554);		 /* real */
	ASSERT_EQ(0, rss_config_save(cfg, path));
	rss_config_free(cfg);

	rss_config_t *re = rss_config_load(path);
	ASSERT(re);
	/* The guessed width must not have been frozen into the file... */
	ASSERT_EQ(NULL, rss_config_get_str(re, "stream0", "width", NULL));
	/* ...while the real write survived the round trip. */
	ASSERT_EQ(8554, rss_config_get_int(re, "rtsp", "port", 0));

	rss_config_free(re);
	unlink(path);
	PASS();
}

/*
 * The property rvd's [image] block now rests on: for a tuning knob, "nobody
 * wrote this" and "the operator wrote the neutral value" are different
 * instructions, and the probe has to tell them apart.
 *
 * They used not to be distinguishable in practice, and it cost a real tuned
 * value. rvd read every image key with a fallback of 128 and applied it
 * unconditionally, so a contrast the config never mentioned still wrote
 * op_type = AUTO over Infinity6C imx335.bin's shipped { MANUAL, 65 } on every
 * start. If a later change makes a getter's stored default look like
 * configuration again, rvd goes straight back to overwriting the tuner and
 * nothing else in the tree would notice -- hence pinning it here.
 *
 *   what happened to the key          probe result   rvd does
 *   --------------------------------  -------------  ---------------------
 *   nothing                           absent         leaves the tuning alone
 *   another reader resolved a default absent         leaves the tuning alone
 *   operator wrote the neutral 128    present, 128   writes auto
 *   operator wrote 140                present, 140   writes manual 140
 */
TEST cfg_unwritten_knob_is_not_a_neutral_one(void)
{
	char path[128];
	rss_config_t *cfg = empty_cfg(path, sizeof(path));
	ASSERT(cfg);

	/* Untouched, and after a reader resolved its own 128: still absent. */
	ASSERT_EQ(NULL, rss_config_get_str(cfg, "image", "contrast", NULL));
	(void)rss_config_get_int(cfg, "image", "contrast", 128);
	ASSERT_EQ(NULL, rss_config_get_str(cfg, "image", "contrast", NULL));

	/* Written by the operator, at the very value the getter had guessed:
	 * now present, and distinguishable from the guess above. */
	rss_config_set_int(cfg, "image", "contrast", 128);
	ASSERT_STR_EQ("128", rss_config_get_str(cfg, "image", "contrast", NULL));

	/* A neighbouring key is unaffected by either. */
	ASSERT_EQ(NULL, rss_config_get_str(cfg, "image", "brightness", NULL));

	rss_config_free(cfg);
	unlink(path);
	PASS();
}

SUITE(config_suite)
{
	RUN_TEST(cfg_default_is_display_only);
	RUN_TEST(cfg_default_still_visible_to_display);
	RUN_TEST(cfg_file_value_beats_every_default);
	RUN_TEST(cfg_set_makes_it_real);
	RUN_TEST(cfg_set_refuses_an_empty_section);
	RUN_TEST(cfg_null_probe_is_order_free);
	RUN_TEST(cfg_fresh_file_save_skips_defaults);
	RUN_TEST(cfg_unwritten_knob_is_not_a_neutral_one);
}
