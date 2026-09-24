/* See media/av_sync.h. */

#include "media/av_sync.h"

#include <stddef.h>

void av_sync_init(av_sync *s, uint32_t queue_max, uint64_t late_threshold_us) {
    s->clock_us          = 0;
    s->late_threshold_us = late_threshold_us;
    s->queue_depth       = 0;
    s->queue_max         = queue_max;
    s->frames_shown      = 0;
    s->frames_dropped    = 0;
}

void av_sync_set_clock_us(av_sync *s, uint64_t clock_us) {
    s->clock_us = clock_us;
}

uint64_t av_rescale_us(uint64_t ticks, uint32_t timescale) {
    uint64_t whole, remainder;

    /* A track with no timescale is malformed input (PROMPT.md section 35 --
     * this number comes off the network), not a licence for a hardware trap.
     * Refuse explicitly rather than let the % below fault. */
    if (timescale == 0) return 0;

    /* ticks * 1000000 done directly can overflow a 64-bit intermediate for
     * ticks large enough, and doing it in floating point to dodge that gives
     * back something merely close instead of exact. Splitting into a whole
     * part and a remainder scaled separately keeps every multiplication
     * bounded: `remainder` is always less than `timescale`, so
     * remainder * 1000000 is always less than timescale * 1000000, nowhere
     * near a 64-bit ceiling for any timescale this project deals with
     * (44100 or 90000), and the division by `timescale` at the end is
     * therefore exact, not approximated. */
    whole     = ticks / timescale;
    remainder = ticks % timescale;

    return whole * 1000000ULL + (remainder * 1000000ULL) / timescale;
}

av_action av_decide(const av_sync *s, uint64_t frame_us, int is_sync) {
    uint64_t late_us;

    /* Not due yet: hold it rather than show it early or drop it. */
    if (frame_us > s->clock_us) return AV_HOLD;

    late_us = s->clock_us - frame_us;

    /* On time, or late only within the threshold: show it. The boundary
     * itself (late_us == late_threshold_us) counts as on time, not late --
     * see av_sync.h and the "exactly on the threshold" test, which pins
     * that the comparison below is > and not >=. */
    if (late_us <= s->late_threshold_us) return AV_SHOW;

    /* Late beyond the threshold. Ordinarily dropped (PROMPT.md section 28:
     * a late frame is dropped rather than shown late), but a sync frame is
     * never dropped -- everything decoded after it depends on it having
     * been decoded, so showing it late is still better than losing every
     * frame downstream of it until the next one. */
    if (is_sync) return AV_SHOW;

    return AV_DROP;
}

int av_sync_queue_push(av_sync *s) {
    if (s->queue_depth >= s->queue_max) return 0;
    s->queue_depth++;
    return 1;
}

void av_sync_queue_pop(av_sync *s) {
    if (s->queue_depth == 0) return;
    s->queue_depth--;
}

void av_sync_note_shown(av_sync *s) {
    s->frames_shown++;
}

void av_sync_note_dropped(av_sync *s) {
    s->frames_dropped++;
}
