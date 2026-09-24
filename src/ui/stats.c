/* See ui/stats.h. */

#include "ui/stats.h"

#include <stdio.h>

/* Splits a microsecond count into whole milliseconds and tenths of a
 * millisecond -- e.g. 28400us -> whole=28, tenth=4 ("28.4"). Rounds to the
 * nearest tenth rather than truncating, so a worst-case reading a hair under
 * a whole millisecond does not read as a whole millisecond lower than it was. */
static void split_ms_tenths(uint32_t us, uint32_t *whole, uint32_t *tenth) {
    uint32_t tenths_total = (us + 50) / 100; /* 100us == one tenth of a ms */
    *whole                = tenths_total / 10;
    *tenth                = tenths_total % 10;
}

/* Same idea for frames per second, kept to hundredths (23.92) rather than
 * tenths, because a frame-rate that only ever reads out to one decimal place
 * cannot distinguish 23.9 (film-rate NTSC, 23.976) from a genuine 24.0, and
 * that distinction is exactly the kind of thing this overlay exists to show. */
static void split_fps_hundredths(uint32_t frames, uint32_t elapsed_us, uint32_t *whole, uint32_t *hundredth) {
    uint64_t hundredths_total;

    if (elapsed_us == 0) {
        *whole     = 0;
        *hundredth = 0;
        return;
    }

    /* frames * 1e8 / elapsed_us, i.e. frames/(elapsed_us/1e6) scaled by 100.
     * 1e8 fits a uint64_t intermediate with room to spare for any frame
     * count this project will ever see (PROMPT.md section 8 -- there is no
     * frame rate here anywhere near what would need it), the same reasoning
     * media/av_sync.c's av_rescale_us relies on for its own intermediate. */
    hundredths_total = (uint64_t)frames * 100000000ULL / elapsed_us;
    *whole           = (uint32_t)(hundredths_total / 100);
    *hundredth       = (uint32_t)(hundredths_total % 100);
}

void stats_format_lines(const stats_snapshot *s, char lines[STATS_LINE_COUNT][STATS_LINE_LEN]) {
    uint32_t fps_whole, fps_hundredth;
    uint32_t avg_whole, avg_tenth;
    uint32_t worst_whole, worst_tenth;

    split_fps_hundredths(s->frames_shown, s->elapsed_us, &fps_whole, &fps_hundredth);
    split_ms_tenths(s->decode_us_avg, &avg_whole, &avg_tenth);
    split_ms_tenths(s->decode_us_worst, &worst_whole, &worst_tenth);

    /* Cast to plain unsigned for the same reason the fields below are: on
     * the PSP's MIPS toolchain uint32_t is "long unsigned int", a distinct
     * type from "unsigned int" as far as -Wformat is concerned, even though
     * both are 32 bits wide -- an uncast uint32_t against %u warns there
     * despite matching cleanly on the host's LLP64 toolchain, where
     * "unsigned int" and "long unsigned int" are the same size and the
     * warning does not fire. */
    snprintf(lines[0], STATS_LINE_LEN, "FPS             %u.%02u", (unsigned)fps_whole, (unsigned)fps_hundredth);
    snprintf(lines[1], STATS_LINE_LEN, "Video decode    %u.%u ms avg", (unsigned)avg_whole, (unsigned)avg_tenth);
    snprintf(lines[2], STATS_LINE_LEN, "Video worst     %u.%u ms", (unsigned)worst_whole, (unsigned)worst_tenth);
    snprintf(lines[3], STATS_LINE_LEN, "Dropped         %u", (unsigned)s->frames_dropped);
    snprintf(lines[4], STATS_LINE_LEN, "Audio underruns %u", (unsigned)s->audio_underruns);
    snprintf(lines[5], STATS_LINE_LEN, "Video queue     %u", (unsigned)s->queue_depth);
    snprintf(lines[6], STATS_LINE_LEN, "RAM free   min  %u kB", (unsigned)s->mem_min_free_kb);
    snprintf(lines[7], STATS_LINE_LEN, "RAM largest min %u kB", (unsigned)s->mem_min_largest_kb);
}

#if defined(__PSP__)

#include <pspdebug.h>

void stats_draw(const stats_snapshot *s, int row) {
    char lines[STATS_LINE_COUNT][STATS_LINE_LEN];
    int  i;

    stats_format_lines(s, lines);

    for (i = 0; i < STATS_LINE_COUNT; i++) {
        pspDebugScreenSetXY(0, row + i);
        pspDebugScreenPrintf("%s\n", lines[i]);
    }
}

#endif
