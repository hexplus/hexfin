/* See ui/stats.h. Each check compares every line, not just the ones a given
 * input happens to exercise -- a diagnostics overlay with seven right lines
 * and one silently wrong one is worse than one that is obviously broken,
 * since a wrong number here is the kind of thing nobody double-checks. */

#include "ui/stats.h"

#include "test.h"

#include <stdio.h>
#include <string.h>

static int check_lines(char note[128], unsigned note_len, char got[STATS_LINE_COUNT][STATS_LINE_LEN],
                        const char *const *want) {
    int i;
    for (i = 0; i < STATS_LINE_COUNT; i++) {
        if (strcmp(got[i], want[i]) != 0) {
            snprintf(note, note_len, "line %d: got \"%s\", want \"%s\"", i, got[i], want[i]);
            return 1;
        }
    }
    return 0;
}

/* Values lifted straight from PROMPT.md section 45's own example overlay, so
 * this check also pins that stats.c's rounding matches the number the spec
 * itself shows for a 28400us average and a 41200us worst. */
static int t_a_representative_snapshot_formats_every_line(char *note, unsigned n) {
    stats_snapshot     s = {0};
    char                lines[STATS_LINE_COUNT][STATS_LINE_LEN];
    static const char *want[STATS_LINE_COUNT] = {
        "FPS             24.00",
        "Video decode    28.4 ms avg",
        "Video worst     41.2 ms",
        "Dropped         18",
        "Audio underruns 0",
        "Video queue     2",
        "RAM free   min  18200 kB",
        "RAM largest min 15300 kB",
    };

    s.frames_shown        = 24;
    s.frames_dropped      = 18;
    s.decode_us_avg       = 28400;
    s.decode_us_worst     = 41200;
    s.audio_underruns     = 0;
    s.queue_depth         = 2;
    s.elapsed_us          = 1000000;
    s.mem_min_free_kb     = 18200;
    s.mem_min_largest_kb  = 15300;

    stats_format_lines(&s, lines);
    if (check_lines(note, n, lines, want)) return 1;
    snprintf(note, n, "PROMPT.md's own 28400us/41200us example renders as 28.4 ms / 41.2 ms");
    return 0;
}

/* A cinema-rate frame count, and two microsecond figures that do not divide
 * evenly, so the rounding is pinned rather than hidden by numbers that land
 * on a whole tenth.
 *
 * The comment here used to say the rounding was truncating. It is not:
 * stats.c's split_ms_tenths adds half a tenth before dividing, which is why
 * 16667us reads 16.7 and not the 16.6 a truncating split would give -- and
 * 16.6 is what this check would have to expect if the comment were right.
 * (The fps split, separately, does truncate; 1198 over 50s divides exactly,
 * so this case does not exercise that.) */
static int t_a_cinema_rate_snapshot_rounds_the_fraction(char *note, unsigned n) {
    stats_snapshot      s = {0};
    char                lines[STATS_LINE_COUNT][STATS_LINE_LEN];
    static const char *want[STATS_LINE_COUNT] = {
        "FPS             23.96",
        "Video decode    16.7 ms avg",
        "Video worst     33.3 ms",
        "Dropped         0",
        "Audio underruns 1",
        "Video queue     5",
        "RAM free   min  9000 kB",
        "RAM largest min 4500 kB",
    };

    s.frames_shown       = 1198;
    s.elapsed_us         = 50000000; /* 1198 frames over 50s -> 23.96 fps exactly */
    s.decode_us_avg      = 16667;    /* one 60Hz field's worth, rounds to 16.7 ms */
    s.decode_us_worst    = 33333;    /* rounds to 33.3 ms */
    s.audio_underruns    = 1;
    s.queue_depth        = 5;
    s.mem_min_free_kb    = 9000;
    s.mem_min_largest_kb = 4500;

    stats_format_lines(&s, lines);
    if (check_lines(note, n, lines, want)) return 1;
    snprintf(note, n, "1198 frames over 50s is exactly 23.96 fps; 16667/33333us round to 16.7/33.3 ms");
    return 0;
}

/* Nothing has played yet: every counter, including elapsed_us, is 0. The
 * case worth having is that this does not divide by elapsed_us to get fps --
 * it reads 0.00, not a crash. */
static int t_an_all_zero_snapshot_still_renders(char *note, unsigned n) {
    stats_snapshot      s = {0};
    char                lines[STATS_LINE_COUNT][STATS_LINE_LEN];
    static const char *want[STATS_LINE_COUNT] = {
        "FPS             0.00",
        "Video decode    0.0 ms avg",
        "Video worst     0.0 ms",
        "Dropped         0",
        "Audio underruns 0",
        "Video queue     0",
        "RAM free   min  0 kB",
        "RAM largest min 0 kB",
    };

    stats_format_lines(&s, lines);
    if (check_lines(note, n, lines, want)) return 1;
    snprintf(note, n, "elapsed_us == 0 reads fps as 0.00 instead of dividing by it");
    return 0;
}

void test_stats_register(void) {
    test_add("stats", "a representative snapshot formats every line", t_a_representative_snapshot_formats_every_line);
    test_add("stats", "a cinema rate snapshot rounds the fraction", t_a_cinema_rate_snapshot_rounds_the_fraction);
    test_add("stats", "an all zero snapshot still renders", t_an_all_zero_snapshot_still_renders);
}
