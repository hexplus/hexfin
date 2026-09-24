/* The developer diagnostics overlay, PROMPT.md section 45.
 *
 * Split the same way ui/render.h and ui/letterbox.c are: the arithmetic that
 * turns raw counters into the words on screen has nothing to do with
 * sceDisplay, so it lives here, unguarded, and the host build
 * (scripts/test.sh) checks it directly. Only stats_draw, which actually puts
 * text on the panel, is PSP-only -- see stats.c.
 *
 * No floating point anywhere in this module, on purpose. video_psp.c and
 * audio_psp.c already avoid pulling newlib's printf into a PSP build for an
 * eight-digit hex number; asking the same minimal libc to also format %f is
 * a second, needless bet on a printf configuration nobody has confirmed on
 * this toolchain. Every rate below is carried as a scaled integer (a
 * millisecond count "times 10", a percentage "times 100") and split into a
 * whole part and a fractional part with plain integer division, so the only
 * conversion specifier stats_format_lines ever needs is %u. */
#ifndef UI_STATS_H
#define UI_STATS_H

#include <stdint.h>

#define STATS_LINE_LEN   40 /* wide enough for every line below plus a nul */
#define STATS_LINE_COUNT 8

/* Raw counters, gathered by whatever is driving playback -- this module
 * never reads a clock or a queue itself, the same reason media/av_sync.h's
 * clock is injected rather than sampled here. */
typedef struct {
    uint32_t frames_shown;
    uint32_t frames_dropped;
    uint32_t decode_us_avg;   /* running average decode time, microseconds */
    uint32_t decode_us_worst; /* worst decode time seen, microseconds */
    uint32_t audio_underruns;
    uint32_t queue_depth;   /* current video queue depth, av_sync.h's queue_depth */
    uint32_t elapsed_us;    /* the window frames_shown was counted over; 0 makes
                              * the fps line read 0 instead of dividing by it */
    uint32_t mem_min_free_kb;    /* memwatch_min_free_kb() */
    uint32_t mem_min_largest_kb; /* memwatch_min_largest_kb() */
} stats_snapshot;

/* Fills exactly STATS_LINE_COUNT lines, each nul-terminated and never longer
 * than STATS_LINE_LEN - 1 characters. Every field of a degenerate snapshot
 * (all zero) still produces text -- a diagnostics screen that blanks itself
 * on an all-zero reading is useless exactly when playback has not started
 * anything yet, which is itself worth being able to see. */
void stats_format_lines(const stats_snapshot *s, char lines[STATS_LINE_COUNT][STATS_LINE_LEN]);

#if defined(__PSP__)
/* Draws the lines stats_format_lines produced, starting at screen row
 * `row`. PSP-only: it calls pspDebugScreenPrintf, which the host build does
 * not link. */
void stats_draw(const stats_snapshot *s, int row);
#endif

#endif
