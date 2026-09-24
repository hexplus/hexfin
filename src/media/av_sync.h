/* A/V synchronisation: audio is the clock, video follows it.
 *
 * PROMPT.md section 7 says audio is the primary clock unless testing shows
 * otherwise, and section 28 gives the priority order when playback falls
 * behind: audio continuity first, then synchronisation, then the current
 * video frame, and preserving every frame comes last. In practice that means
 * this module never touches audio timing at all -- it only decides what
 * happens to a video frame given where the audio has already got to.
 *
 * A late frame is dropped rather than shown late, with one exception: a sync
 * frame (fmp4_sample.is_sync -- a keyframe) is never dropped, because every
 * frame decoded after it depends on it having been decoded. Losing a
 * reference frame does not lose one frame's worth of picture, it loses
 * everything downstream until the next one.
 *
 * The clock's SOURCE is injected rather than read here: this module never
 * calls platform_now_us() or touches the audio decoder, so the host can
 * drive it with whatever audio position a test wants. See av_sync_set_clock_us.
 *
 * No platform headers, so the host compiles and tests this. */
#ifndef MEDIA_AV_SYNC_H
#define MEDIA_AV_SYNC_H

#include <stdint.h>

typedef enum {
    AV_SHOW, /* due now (or within the late threshold) -- display it */
    AV_HOLD, /* ahead of the clock -- not due yet, keep waiting */
    AV_DROP  /* behind the clock by more than the late threshold, and not a
              * sync frame -- skip it rather than show it late */
} av_action;

/* The late threshold this project uses in real playback, in microseconds.
 * av_sync_init takes the threshold as a parameter rather than hardcoding it
 * (so a test can pin exact boundary behaviour with round numbers), but a
 * real caller should start here.
 *
 * 100 ms. A frame interval at cinema rates is already ~41.7 ms (23.976 fps)
 * or 40 ms (25 fps), so a threshold has to clear one interval or ordinary
 * decode-time jitter -- one frame arriving a little late because the
 * previous one took a little long -- would trip it on every other frame.
 * Twice that is roughly the point audible/visible lip-sync drift becomes
 * noticeable to begin with, which is also the point PROMPT.md section 28's
 * "do not gradually drift seconds behind real playback" stops being
 * optional: past 100 ms of video lag the picture is visibly behind the
 * sound, so holding out for a frame that is that late buys nothing --
 * showing it late looks the same as never showing it, and dropping it lets
 * the next frame (which is closer to on time) take its place instead. */
#define AV_SYNC_DEFAULT_LATE_THRESHOLD_US 100000

typedef struct {
    /* The audio clock, in microseconds, as of the last call to
     * av_sync_set_clock_us. Never advanced by this module. */
    uint64_t clock_us;

    /* How far behind the clock a frame may be and still count as "on time".
     * Chosen in av_sync_init's caller -- see av_sync.c for the value this
     * project uses and why. */
    uint64_t late_threshold_us;

    /* The video queue's depth and the bound it must not cross. PROMPT.md
     * section 28: never let the queue grow without bound. */
    uint32_t queue_depth;
    uint32_t queue_max;

    /* Diagnostics feeding the overlay in src/ui/stats.h (PROMPT.md section
     * 45): how many frames made it to the screen and how many did not.
     * Kept apart from av_decide on purpose -- av_decide takes a const
     * pointer and only answers a question, it does not also have the side
     * effect of counting its own answer. The caller counts, once, after it
     * has acted on the decision. */
    uint32_t frames_shown;
    uint32_t frames_dropped;
} av_sync;

/* Zeroes the counters and the queue depth, and records the bound and
 * threshold this session will use. */
void av_sync_init(av_sync *s, uint32_t queue_max, uint64_t late_threshold_us);

/* Tells this module where the audio has got to. The caller reads that
 * position off the audio decoder (or, in a test, makes one up); this module
 * never reads it itself. */
void av_sync_set_clock_us(av_sync *s, uint64_t clock_us);

/* Converts a timestamp from a track's own timescale (ticks per second) to
 * microseconds, exactly -- not merely approximately, which rules out a
 * floating-point conversion. A `timescale` of 0 is a malformed or
 * uninitialised track description, not a licence to divide by zero: this
 * returns 0 rather than trapping or invoking undefined behaviour. */
uint64_t av_rescale_us(uint64_t ticks, uint32_t timescale);

/* The decision for one video frame due at `frame_us` (already rescaled to
 * microseconds, in the same clock as s->clock_us). `is_sync` marks a
 * keyframe, which this never drops regardless of how late it is. */
av_action av_decide(const av_sync *s, uint64_t frame_us, int is_sync);

/* Queue discipline (PROMPT.md section 28: never let the queue grow without
 * bound). Returns 1 and increments queue_depth if there was room, 0 and
 * leaves queue_depth unchanged if the queue was already at queue_max. */
int av_sync_queue_push(av_sync *s);

/* One frame left the queue (shown or dropped). Floors at 0 rather than
 * wrapping a uint32_t negative, in case a caller pops without a matching
 * push having succeeded. */
void av_sync_queue_pop(av_sync *s);

/* Diagnostics counters. The caller calls exactly one of these after acting
 * on an av_decide result of AV_SHOW or AV_DROP (an AV_HOLD is neither yet). */
void av_sync_note_shown(av_sync *s);
void av_sync_note_dropped(av_sync *s);

#endif
