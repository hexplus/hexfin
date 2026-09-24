/* See media/av_sync.h. The rescale is the case worth the most scrutiny: video
 * and audio arrive in different timescales, and a naive `dts * 1000000`
 * either overflows or, done in floating point to dodge that, comes back
 * merely close rather than exact -- so the overflow checks below use a
 * two-hour film's tick counts and assert exact equality, not a tolerance. */

#include "media/av_sync.h"

#include "test.h"

#include <stdio.h>

static int t_a_dts_converts_to_microseconds_exactly(char *note, unsigned n) {
    if (av_rescale_us(44100, 44100) != 1000000) {
        snprintf(note, n, "44100/44100 timescale gave %llu, not 1000000",
                 (unsigned long long)av_rescale_us(44100, 44100));
        return 1;
    }
    if (av_rescale_us(90000, 90000) != 1000000) {
        snprintf(note, n, "90000/90000 timescale gave %llu, not 1000000",
                 (unsigned long long)av_rescale_us(90000, 90000));
        return 1;
    }
    /* The two above, and the two-hour pair below, all divide exactly. That
     * made the check's name a lie: an implementation reduced to
     * `(ticks / timescale) * 1000000` -- which throws sub-second precision
     * away entirely and would misplace every single frame -- passes all four
     * of them. These two do not divide, which is the only way "exactly"
     * means anything.
     *
     * 3754 ticks at 90000 is one frame at 23.976 fps (90000/23.976 =
     * 3753.75). 3754 * 1000000 / 90000 = 41711.11..., truncated to 41711.
     * 1024 samples at 44100 is one AAC frame: 1024 * 1000000 / 44100 =
     * 23219.95..., truncated to 23219. The reduced implementation returns 0
     * for both. */
    if (av_rescale_us(3754, 90000) != 41711) {
        snprintf(note, n, "one 23.976fps frame (3754 ticks at 90000) gave %llu, not 41711",
                 (unsigned long long)av_rescale_us(3754, 90000));
        return 1;
    }
    if (av_rescale_us(1024, 44100) != 23219) {
        snprintf(note, n, "one AAC frame (1024 samples at 44100) gave %llu, not 23219",
                 (unsigned long long)av_rescale_us(1024, 44100));
        return 1;
    }

    snprintf(note, n, "one-second dts round-trips in both timescales, and 3754/90000 and 1024/44100 -- which do not "
                      "divide -- land on 41711us and 23219us");
    return 0;
}

static int t_a_two_hour_dts_does_not_overflow(char *note, unsigned n) {
    uint64_t audio_us = av_rescale_us(317520000ULL, 44100);
    uint64_t video_us  = av_rescale_us(648000000ULL, 90000);

    if (audio_us != 7200000000ULL) {
        snprintf(note, n, "44100 timescale, dts 317520000 gave %llu, not 7200000000",
                 (unsigned long long)audio_us);
        return 1;
    }
    if (video_us != 7200000000ULL) {
        snprintf(note, n, "90000 timescale, dts 648000000 gave %llu, not 7200000000",
                 (unsigned long long)video_us);
        return 1;
    }
    snprintf(note, n, "a two hour film's dts in either timescale lands on exactly 7200000000us");
    return 0;
}

static int t_a_zero_timescale_is_refused(char *note, unsigned n) {
    if (av_rescale_us(123456, 0) != 0) {
        snprintf(note, n, "a zero timescale did not come back as 0");
        return 1;
    }
    snprintf(note, n, "a zero timescale returns 0 instead of dividing by zero");
    return 0;
}

static int t_a_frame_due_now_is_shown(char *note, unsigned n) {
    av_sync s;
    av_sync_init(&s, 8, 100000);
    av_sync_set_clock_us(&s, 5000000);

    if (av_decide(&s, 5000000, 0) != AV_SHOW) {
        snprintf(note, n, "a frame at exactly the clock was not shown");
        return 1;
    }
    snprintf(note, n, "frame_us == clock_us shows");
    return 0;
}

static int t_a_frame_ahead_of_the_clock_is_held(char *note, unsigned n) {
    av_sync s;
    av_sync_init(&s, 8, 100000);
    av_sync_set_clock_us(&s, 5000000);

    if (av_decide(&s, 5200000, 0) != AV_HOLD) {
        snprintf(note, n, "a frame 200ms ahead of the clock was not held");
        return 1;
    }
    snprintf(note, n, "frame_us > clock_us holds, neither shown nor dropped");
    return 0;
}

static int t_a_frame_late_beyond_the_threshold_is_dropped(char *note, unsigned n) {
    av_sync s;
    av_sync_init(&s, 8, 100000);
    av_sync_set_clock_us(&s, 5200001);

    if (av_decide(&s, 5000000, 0) != AV_DROP) {
        snprintf(note, n, "a frame 200001us behind a 100000us threshold was not dropped");
        return 1;
    }
    snprintf(note, n, "clock ahead of frame by more than the threshold drops a non-sync frame");
    return 0;
}

static int t_a_late_sync_frame_is_shown_never_dropped(char *note, unsigned n) {
    av_sync s;
    av_sync_init(&s, 8, 100000);
    av_sync_set_clock_us(&s, 5200001);

    if (av_decide(&s, 5000000, 1) != AV_SHOW) {
        snprintf(note, n, "a sync frame 200001us late was not shown");
        return 1;
    }
    snprintf(note, n, "a sync frame is shown no matter how late -- everything after it depends on it");
    return 0;
}

static int t_a_frame_exactly_on_the_threshold_is_not_dropped(char *note, unsigned n) {
    av_sync s;
    av_sync_init(&s, 8, 100000);
    av_sync_set_clock_us(&s, 5100000); /* exactly late_threshold_us behind frame_us */

    if (av_decide(&s, 5000000, 0) != AV_SHOW) {
        snprintf(note, n, "a frame exactly at the threshold was dropped instead of shown");
        return 1;
    }
    /* The other side of the same boundary. Pinned only from below, an
     * implementation using >= rather than > would keep this check green
     * while dropping every frame that is exactly on time; one microsecond
     * further out has to drop, or the threshold is not a threshold. */
    av_sync_set_clock_us(&s, 5100001);
    if (av_decide(&s, 5000000, 0) != AV_DROP) {
        snprintf(note, n, "a frame one microsecond past the threshold was not dropped");
        return 1;
    }

    snprintf(note, n, "lateness == threshold still shows, threshold + 1us drops -- the boundary belongs to \"on time\"");
    return 0;
}

static int t_the_video_queue_refuses_to_grow_past_its_bound(char *note, unsigned n) {
    av_sync s;
    int     i;
    av_sync_init(&s, 3, 100000);

    for (i = 0; i < 3; i++) {
        if (!av_sync_queue_push(&s)) {
            snprintf(note, n, "push %d of 3 was refused, but the queue was not yet full", i + 1);
            return 1;
        }
    }
    if (av_sync_queue_push(&s)) {
        snprintf(note, n, "a 4th push was accepted past a bound of 3");
        return 1;
    }
    if (s.queue_depth != 3) {
        snprintf(note, n, "queue depth reads %u after a refused push, not 3", s.queue_depth);
        return 1;
    }
    snprintf(note, n, "a bound of 3 stays at 3 no matter how many more frames arrive");
    return 0;
}

static int t_a_drop_is_counted(char *note, unsigned n) {
    av_sync s;
    av_sync_init(&s, 8, 100000);

    av_sync_note_dropped(&s);
    av_sync_note_dropped(&s);
    av_sync_note_shown(&s);

    if (s.frames_dropped != 2) {
        snprintf(note, n, "frames_dropped reads %u, not 2", s.frames_dropped);
        return 1;
    }
    if (s.frames_shown != 1) {
        snprintf(note, n, "frames_shown reads %u, not 1", s.frames_shown);
        return 1;
    }
    snprintf(note, n, "2 drops and 1 show land in separate counters, for the diagnostics overlay");
    return 0;
}

void test_av_sync_register(void) {
    test_add("sync", "a dts converts to microseconds exactly", t_a_dts_converts_to_microseconds_exactly);
    test_add("sync", "a two hour dts does not overflow", t_a_two_hour_dts_does_not_overflow);
    test_add("sync", "a zero timescale is refused rather than dividing by zero", t_a_zero_timescale_is_refused);
    test_add("sync", "a frame due now is shown", t_a_frame_due_now_is_shown);
    test_add("sync", "a frame ahead of the clock is held", t_a_frame_ahead_of_the_clock_is_held);
    test_add("sync", "a frame late beyond the threshold is dropped", t_a_frame_late_beyond_the_threshold_is_dropped);
    test_add("sync", "a late sync frame is shown, never dropped", t_a_late_sync_frame_is_shown_never_dropped);
    test_add("sync", "a frame exactly on the threshold is not dropped",
              t_a_frame_exactly_on_the_threshold_is_not_dropped);
    test_add("sync", "the video queue refuses to grow past its bound",
              t_the_video_queue_refuses_to_grow_past_its_bound);
    test_add("sync", "a drop is counted", t_a_drop_is_counted);
}
