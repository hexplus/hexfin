/* Playing one stream, start to finish, from an already open byte source.
 *
 * This is the pipeline the probe proved on hardware -- a reader thread
 * filling a 1 MB ring, fragmented-MP4 fragments walked ahead of playback,
 * audio on its own thread as the clock, video decoded on the Media Engine
 * when due (docs/RESEARCH.md section 10) -- moved out of main.c so the
 * player UI and the probe run the same code.
 *
 * One stream at a time. player_run opens the decoders, plays, and closes
 * them again before it returns, so it can be called once per item for as
 * long as the program runs. The byte source stays the caller's: opened
 * before, closed after.
 *
 * Only compiled for the console. */
#ifndef PLAYER_PLAYER_H
#define PLAYER_PLAYER_H

#include "media/av_sync.h"
#include "media/fmp4.h"
#include "media/source.h"

#include <stdint.h>

typedef struct {
    int         show_all;    /* show every picture whatever the clock says (probe only) */
    int         skip_audio;  /* never open one decoder, to isolate a fault */
    int         skip_video;
    int         interactive; /* buttons pause (X, START) and stop (O) */
    const char *title;       /* for the pause screen */
    uint64_t    duration_ticks; /* for the pause screen; 0 when unknown */
    int         show_clock;     /* the time of day on the pause screen */
    int         show_battery;   /* the battery level on the pause screen */
    uint64_t    start_ticks;    /* where in the item this stream begins: its timestamps start at 0 there */
} player_options;

typedef enum {
    PLAYER_ENDED,   /* the stream finished */
    PLAYER_STOPPED, /* the person stopped it */
    PLAYER_HOME,    /* HOME was pressed: the program is leaving */
    PLAYER_FAILED,  /* see player_session.parse_fatal */
    PLAYER_SEEK     /* L or R asked for another position: player_session.seek_to_ticks */
} player_outcome;

/* Everything a run did, for the probe's report and the log. */
typedef struct {
    fmp4    m;
    av_sync sync;

    int      video_open_failed;
    uint32_t video_push_failures;
    int      audio_open_failed;

    uint32_t fragments_parsed;
    uint32_t video_samples;
    uint32_t audio_samples;

    uint64_t video_decode_us_total;
    uint32_t video_decode_count;
    uint32_t video_decode_us_worst;
    uint32_t audio_underruns;
    uint32_t queue_overflow;

    /* The wall-clock stand-in for the audio clock, when there is no audio
     * to time against: set on first use. */
    unsigned long long wall_origin_us;
    int                wall_origin_set;

    /* The window the frame rate is measured over: first to last picture
     * shown. */
    unsigned long long first_shown_us;
    unsigned long long last_shown_us;

    uint64_t position_us;   /* where playback got to, on the playback clock */

    /* Kept current while playing, for a progress-report thread to read:
     * the playback clock in ms (from where this stream began) and whether
     * the person has paused. 32-bit, so a read on another thread is whole. */
    volatile uint32_t live_ms;
    volatile int      live_paused;
    uint64_t seek_to_ticks; /* with PLAYER_SEEK: where in the item to start again */

    const char *parse_fatal; /* NULL unless parsing stopped for a reason other than the end */
    int         done_parsing;
} player_session;

/* Plays `src` (already open) to its end, a stop, HOME or a failure. `ps` is
 * cleared first and filled in as it goes. */
player_outcome player_run(player_session *ps, const media_source *src, const player_options *opt);

#endif /* PLAYER_PLAYER_H */
