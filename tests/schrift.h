/*
 * schrift.h -- enough of libschrift for rod.h to compile in the host suite.
 *
 * rod.h holds an SFT by value in every font slot, so the type must be
 * complete for the struct to have a size. Nothing here is called: the suite
 * links rod's element bookkeeping, not its glyph rendering. Same reason and
 * same shape as the sdp_parse.c shim -- a test that needs one file out of a
 * daemon should not drag the daemon's third-party tree in behind it.
 */

#ifndef TESTS_SCHRIFT_H
#define TESTS_SCHRIFT_H

typedef struct SFT_Font SFT_Font;

typedef struct SFT {
	SFT_Font *font;
	double xScale;
	double yScale;
	double xOffset;
	double yOffset;
	int flags;
} SFT;

#endif /* TESTS_SCHRIFT_H */
