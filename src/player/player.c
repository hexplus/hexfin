/* See player/player.h. The pipeline here was main.c's until the player UI
 * needed to run it more than once; the comments that explain why it has the
 * shape it has came with it.
 *
 * Only compiled for the console. */
#if defined(__PSP__)

#include "player/player.h"

#include "media/audio_decoder.h"
#include "media/video_decoder.h"
#include "platform/input.h"
#include "platform/psp_platform.h"
#include "platform/trace.h"
#include "ui/render.h"
#include "ui/text.h"
#include "utils/memwatch.h"

#include <pspctrl.h>
#include <pspkernel.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* How many refused video pushes before the player stops feeding the
 * decoder. Enough to reach several keyframes (one every 5 s), so "the first
 * IDR is refused but a later one decodes" still plays. */
#define PLAYER_VIDEO_MAX_FAILURES 400u

static const player_options *g_opt;

/* ---------------------------------------------------------------- seeking
 *
 * L goes back and R forward, SEEK_STEP_TICKS a press. Presses are gathered
 * for SEEK_SETTLE_US after the last one, with the target shown, before the
 * stream is reopened there: every reopen is a new transcode that takes the
 * server a second or two to start (1.2 s measured, 2026-09-24), so holding
 * R for a minute ahead costs one of them, not six. Holding a shoulder
 * repeats (platform/input.h), which is how a long jump is made. */
#define SEEK_STEP_TICKS (10ull * 10000000ull)
#define SEEK_SETTLE_US  800000ull

static int                g_seek_pending;
static uint64_t           g_seek_target; /* ticks into the item */
static unsigned long long g_seek_deadline_us;

/* A line kept over the picture: drawn again after every frame shown, since
 * each frame replaces the whole buffer it lands in. "" for none. */
static char g_osd_line[160];

/* When a line that goes away by itself -- the volume -- stops being shown.
 * 0 while the line is a seek's, which stays until the seek happens. */
static unsigned long long g_osd_until_us;
#define VOLUME_OSD_US 1500000ull

/* The volume buttons: the new level over the picture for a moment. A seek
 * being chosen keeps the line; its target matters more. */
static void volume_poll(void) {
    int level, muted;

    if (!platform_volume_changed(&level, &muted) || g_seek_pending) return;
    text_volume(g_osd_line, sizeof g_osd_line, level, PLATFORM_VOLUME_MAX, muted);
    g_osd_until_us = platform_now_us() + VOLUME_OSD_US;
    render_osd(g_osd_line);
}

/* Takes a timed line away once its time is up. Returns 1 when it did. */
static int osd_expire(void) {
    if (!g_osd_until_us || g_seek_pending || platform_now_us() < g_osd_until_us) return 0;
    g_osd_until_us = 0;
    g_osd_line[0]  = 0;
    render_invalidate(); /* the band it covered is repainted with the next picture */
    return 1;
}

/* ------------------------------------------------------------ the read ring
 *
 * A bounded staging buffer for the file, refilled from sceIoRead and
 * compacted (never wrapped) as it is consumed -- see span_at()
 * below for why compaction, not a true circular ring, is the right shape
 * here.
 *
 * SIZE: PROMPT.md section 24 suggests a 256-512 kB encoded network ring, and
 * this was 384 kB until hardware said otherwise. fmp4_fragment() needs a
 * whole moof+mdat contiguous in memory (fmp4.h), and reading ahead needs the
 * NEXT fragment in memory too while the current one plays: a 480x272 fixture
 * fragment is ~310 kB, so a 384 kB ring could not hold the next one, and
 * every fragment boundary waited on the reader -- ~50 ms from the memory
 * stick, 1-1.6 s over Wi-Fi (2026-09-24).
 *
 * Then 1 MB, until a film said otherwise: at 600 kbps a 5 s fragment is
 * ~370 kB, and a fragment's bytes stay until its last frame has played, so
 * 1 MB held the one playing, the next, and part of the one after. The next
 * could often only finish arriving once the current one was released, and
 * the audio ran dry for a moment at 7 fragment boundaries in 15 minutes of
 * "Donnie Darko"; on another item, Wi-Fi stalls longer than the 13 s 1 MB
 * holds cost 59 s of waiting (logs/probe-app.log, 2026-09-24).
 *
 * 3 MB is about 40 s of stream: room for the fragments in flight and a
 * stall of half a minute. The partition had ~15 MB free before the decoder's
 * 4 MB block, so 2 MB more still leaves it room. A fragment that does not
 * fit is fatal for the player (see span_at) rather than grown past --
 * PROMPT.md section 35 is why an unbounded size claimed by the network is
 * refused rather than trusted into a larger and larger allocation. */
#define MEDIA_RING_CAPACITY (3u * 1024u * 1024u)

/* Played bytes are not moved out at once: the front of the ring is only
 * marked as free (g_ring_head), and the unplayed rest is slid down once that
 * free front reaches this much. Sliding at every fragment would move up to
 * 3 MB every 5 seconds; this moves at most half of that, a few times a
 * minute. */
#define RING_COMPACT_AT (MEDIA_RING_CAPACITY / 2u)

static uint8_t         g_ring[MEDIA_RING_CAPACITY];
static volatile size_t g_ring_len; /* valid bytes at g_ring[0 .. g_ring_len) */
static volatile int    g_ring_eof; /* the source said EOF: nothing more is coming */
static volatile int    g_ring_io_error;

/* The reader thread keeps the ring full while the main thread plays from it.
 * Reading between fragments instead -- as this probe first did -- left the
 * radio idle for the five seconds a fragment plays and the audio ring empty
 * while the next one was fetched: on hardware (2026-09-24) the HTTP build
 * took 56 s to play a 30 s clip, and every fragment boundary was an audible
 * gap even from the memory stick.
 *
 * The reader only ever appends past g_ring_len, and the main thread only
 * ever reads below it, so the bytes a fragment's samples point into are
 * never touched while they are in use. The one operation that moves bytes,
 * ring_consume's compaction, and the append itself run under g_ring_lock.
 * The read from the source runs outside it, into a chunk buffer of the
 * reader's own, because a network read can block for seconds. */
#define READER_CHUNK       (32u * 1024u)
#define READER_THREAD_PRIO 0x18 /* below the audio output thread, above main */
#define READER_STACK       0x4000
#define READER_IDLE_US     5000
#define READER_EXIT_US     1000000

static uint8_t      g_read_chunk[READER_CHUNK];
static SceUID       g_ring_lock   = -1;
static SceUID       g_reader      = -1;
static volatile int g_reader_stop;

static void ring_lock(void) { sceKernelWaitSema(g_ring_lock, 1, NULL); }
static void ring_unlock(void) { sceKernelSignalSema(g_ring_lock, 1); }
/* The source of the run in progress, set by player_run. */
static const media_source *g_source;

static int reader_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    while (!g_reader_stop) {
        uint32_t got = 0, space;
        int      rc;

        space = (uint32_t)(MEDIA_RING_CAPACITY - g_ring_len); /* only grows under us */
        if (space == 0) {
            sceKernelDelayThread(READER_IDLE_US);
            continue;
        }
        if (space > READER_CHUNK) space = READER_CHUNK;

        rc = g_source->read(g_read_chunk, space, &got);
        if (rc == SOURCE_ERR_EOF) {
            g_ring_eof = 1;
            break;
        }
        /* A zero-byte success is treated as a failure rather than retried.
         * media/source.h promises SOURCE_OK means at least one byte, and a
         * source that broke that promise would spin this loop forever. */
        if (rc != SOURCE_OK || got == 0) {
            g_ring_io_error = 1;
            break;
        }

        ring_lock();
        memcpy(g_ring + g_ring_len, g_read_chunk, got);
        g_ring_len += (size_t)got;
        ring_unlock();
    }
    return 0;
}

static int reader_start(void) {
    g_reader_stop = 0;
    g_ring_lock   = sceKernelCreateSema("hexfin_ring", 0, 1, 1, NULL);
    if (g_ring_lock < 0) return g_ring_lock;
    g_reader = sceKernelCreateThread("hexfin_reader", reader_thread, READER_THREAD_PRIO, READER_STACK, 0, NULL);
    if (g_reader < 0) return g_reader;
    return sceKernelStartThread(g_reader, 0, NULL);
}

/* Before the source is closed: the reader is the only thing reading it. On
 * HOME the stop hook has already cancelled any network read it is blocked
 * in, so the wait is short. */
static void reader_stop(void) {
    if (g_reader >= 0) {
        SceUInt timeout = READER_EXIT_US;
        g_reader_stop   = 1;
        if (sceKernelWaitThreadEnd(g_reader, &timeout) < 0) sceKernelTerminateThread(g_reader);
        sceKernelDeleteThread(g_reader);
        g_reader = -1;
    }
    if (g_ring_lock >= 0) {
        sceKernelDeleteSema(g_ring_lock);
        g_ring_lock = -1;
    }
}

/* Waits until the ring holds at least `want` bytes, is full, or the stream
 * has ended or failed. Returns the bytes now held, which may be less than
 * `want` -- the caller decides whether that shortfall is fatal. */
static unsigned long long g_ring_waited_us; /* time the main thread spent waiting on the reader */

static size_t ring_fill(size_t want) {
    unsigned long long t0 = platform_now_us();
    while (g_ring_len < want && g_ring_len < MEDIA_RING_CAPACITY && !g_ring_eof && !g_ring_io_error &&
           !platform_exit_requested())
        sceKernelDelayThread(1000);
    g_ring_waited_us += platform_now_us() - t0;
    return g_ring_len;
}

/* Slides the unread tail down to the front. This IS the compaction that
 * keeps the ring a plain linear buffer rather than a wrap-around one: a
 * fragment fmp4_fragment() is about to read is always contiguous from
 * g_ring[0], never split across a wrap point. */
static void ring_consume(size_t n) {
    ring_lock();
    if (n >= g_ring_len) {
        g_ring_len = 0;
    } else {
        memmove(g_ring, g_ring + n, g_ring_len - n);
        g_ring_len -= n;
    }
    ring_unlock();
}

/* --------------------------------------------------- finding one fragment
 *
 * fmp4_fragment() needs a whole moof+mdat contiguous in `data`. The choice
 * made here: peek each top-level box's own 32-bit size field (the same field
 * media/fmp4.c's next_box() reads, just far enough ahead of the parser to
 * know how much must have arrived), and only once all of it is in the ring
 * hand fmp4_fragment() a span it is guaranteed to be able to walk in one
 * pass.
 * That is cheaper than growing the ring to "the largest fragment that will
 * ever arrive" and betting the guess is high enough, and it is what makes a
 * parser error here mean "this file is malformed", never "the ring was
 * sized wrong". */

#define MAIN_FOURCC(a, b, c, d) \
    (((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(uint8_t)(b) << 16) | ((uint32_t)(uint8_t)(c) << 8) | (uint32_t)(uint8_t)(d))

static uint32_t peek_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static const char *g_fatal_reason;

/* A copy of the source's own words, not a pointer to them.
 *
 * media/source.h says a reason is only valid until the next call into the
 * source, and this one stays on the screen for the rest of the run -- through
 * every later read, and through the close in teardown(). Holding the pointer
 * would work right up until it didn't. */
static char g_fatal_buf[192];

static const char *keep(const char *msg) {
    size_t i = 0;
    while (msg[i] && i + 1 < sizeof g_fatal_buf) {
        g_fatal_buf[i] = msg[i];
        i++;
    }
    g_fatal_buf[i] = 0;
    return g_fatal_buf;
}

typedef enum { SPAN_READY, SPAN_NEED, SPAN_EOF, SPAN_FATAL } span_result;

/* Why a box at `off` cannot be walked yet: not all of it has arrived. Only
 * fatal when nothing more ever will. */
static span_result span_short(size_t have) {
    if (g_ring_io_error) {
        g_fatal_reason = keep(g_source->error());
        return SPAN_FATAL;
    }
    if (g_ring_eof) {
        g_fatal_reason = have ? "the stream ended in the middle of a box" : "the stream ended before a fragment finished";
        return SPAN_FATAL;
    }
    return SPAN_NEED; /* the reader is still bringing it in */
}

/* Looks at the top-level box starting `off` bytes into the ring, WITHOUT
 * waiting: SPAN_READY with its length when it is wholly in memory (a
 * moof+mdat pair counted as one, `*out_fragment` 1; any other box -- a stray
 * `free` or the trailing `mfra` -- as itself, `*out_fragment` 0, to be
 * stepped over rather than interpreted), SPAN_NEED when more bytes are still
 * to come, SPAN_EOF at a clean end of stream.
 *
 * Non-blocking because the caller is also the one keeping the audio fed:
 * waiting here is exactly what emptied the audio ring at every fragment
 * boundary (docs/RESEARCH.md section 10). Offset-based because the next
 * fragment is walked while the current one is still playing.
 *
 * Assumes mdat immediately follows moof, the same assumption fmp4.h's own
 * reader documents; fmp4_fragment() independently re-walks the span and
 * refuses one that does not hold both. */
static span_result span_at(size_t off, size_t *out_len, int *out_fragment) {
    size_t   have = g_ring_len - off; /* only grows under us */
    uint64_t box_size;
    uint32_t type;
    size_t   hdr = 8;

    if (have < 8) {
        if (have == 0 && g_ring_eof && !g_ring_io_error) return SPAN_EOF; /* clean end of stream */
        return span_short(have);
    }

    box_size = peek_be32(g_ring + off);
    type     = peek_be32(g_ring + off + 4);
    if (box_size == 1) {
        if (have < 16) return span_short(have);
        box_size = ((uint64_t)peek_be32(g_ring + off + 8) << 32) | peek_be32(g_ring + off + 12);
        hdr      = 16;
    } else if (box_size == 0) {
        g_fatal_reason = "a top-level box runs to end-of-file, which this bounded reader does not support";
        return SPAN_FATAL;
    }
    if (box_size < hdr) {
        g_fatal_reason = "a box is smaller than its own header";
        return SPAN_FATAL;
    }
    if (box_size > MEDIA_RING_CAPACITY) {
        g_fatal_reason = "a top-level box is larger than the player's ring";
        return SPAN_FATAL;
    }

    if (type != MAIN_FOURCC('m', 'o', 'o', 'f')) {
        if (have < box_size) return span_short(have);
        *out_len      = (size_t)box_size;
        *out_fragment = 0;
        return SPAN_READY;
    }

    {
        size_t   moof_total = (size_t)box_size;
        uint64_t mdat_size, fragment_total;
        size_t   mdat_hdr = 8;

        if (have < moof_total + 8) return span_short(have);
        mdat_size = peek_be32(g_ring + off + moof_total);
        if (mdat_size == 1) {
            if (have < moof_total + 16) return span_short(have);
            mdat_size = ((uint64_t)peek_be32(g_ring + off + moof_total + 8) << 32) |
                        peek_be32(g_ring + off + moof_total + 12);
            mdat_hdr  = 16;
        } else if (mdat_size == 0) {
            g_fatal_reason = "an mdat box runs to end-of-file, which this bounded reader does not support";
            return SPAN_FATAL;
        }
        if (mdat_size < mdat_hdr) {
            g_fatal_reason = "a moof's mdat is smaller than its own header";
            return SPAN_FATAL;
        }
        fragment_total = (uint64_t)moof_total + mdat_size;
        if (fragment_total > MEDIA_RING_CAPACITY) {
            g_fatal_reason = "a fragment is larger than the player's ring";
            return SPAN_FATAL;
        }
        if (have < fragment_total) return span_short(have);
        *out_len      = (size_t)fragment_total;
        *out_fragment = 1;
        return SPAN_READY;
    }
}

/* --------------------------------------------------------------- av sync
 *
 * "a few access units, not several seconds" (PROMPT.md section 24). Video is
 * decoded the moment it is due and shown at once, so there is no queue of
 * decoded pictures here for this to bound; the number is kept for the
 * player that will have one. */
#define VIDEO_QUEUE_MAX 4u


/* ---------------------------------------------------- the sample queues
 *
 * fmp4_fragment hands a fragment's samples over track by track -- all of its
 * video, then all of its audio -- so decoding them in that order would play
 * five seconds of pictures before a note of sound. The callback therefore
 * only queues references to them (the bytes stay where they are, in the read
 * ring), and the play loop interleaves the two tracks by time.
 *
 * The queues run ACROSS fragments. The next fragment is walked and queued
 * while the current one is still playing, so its audio can be fed to the
 * output while the current fragment's last pictures are still waiting for
 * their time. Played one fragment at a time, the audio ring could hold no
 * more than what was left of the current fragment -- about one video frame's
 * worth at its end -- and every fragment boundary was an audible gap on
 * hardware (2026-09-24).
 *
 * A fragment's bytes are released from the ring only once every sample of
 * it has been played; the release moves the bytes after it down, and the
 * queued references into them are moved with them (retire_fragments). */
#define FRAG_VIDEO_MAX 1024u /* the most samples one fragment may add: 40 s at 25 fps */
#define FRAG_AUDIO_MAX 2048u /* 47 s of 44.1 kHz AAC */
#define VQ_CAP         (2u * FRAG_VIDEO_MAX)
#define AQ_CAP         (2u * FRAG_AUDIO_MAX)

/* How many fragments to hold queued: the one playing and the ones read
 * ahead of it. Bounded in practice by the ring, which must hold them all at
 * once; 8 is what 3 MB holds at this bitrate. */
#define FRAGS_AHEAD 8u

typedef struct {
    const uint8_t *data;
    uint32_t       size;
    uint64_t       dts;
    uint8_t        sync;
} sample_ref;

/* Positions are running counts, never reset: slot = position % capacity,
 * and tail - head is how many are queued. */
static sample_ref g_vq[VQ_CAP];
static sample_ref g_aq[AQ_CAP];
static uint32_t   g_vhead, g_vtail, g_ahead, g_atail;
static uint32_t   g_queue_overflow; /* samples dropped because a queue was full; reported */

typedef struct {
    size_t   len;          /* bytes of ring it occupies, from the front of what is queued */
    uint32_t v_end, a_end; /* queue positions just past its last sample */
} queued_frag;

static queued_frag g_frags[FRAGS_AHEAD];
static unsigned    g_nfrags;
static size_t      g_parse_off; /* ring bytes already walked and queued */
static size_t      g_ring_head; /* ring bytes at the front already played: free, not yet slid out */

/* The first samples are traced one by one; after that only whole fragments
 * are (platform/trace.h). */
#define TRACE_DETAILED_SAMPLES 8u

static int on_sample(const fmp4_sample *s, void *user) {
    player_session *ps = (player_session *)user;
    sample_ref   r;

    if ((ps->video_samples + ps->audio_samples) < TRACE_DETAILED_SAMPLES)
        trace("sample track %u size %u dts %llu sync %d", (unsigned)s->track_id, (unsigned)s->size,
              (unsigned long long)s->dts, (int)s->is_sync);

    r.data = s->data;
    r.size = s->size;
    r.dts  = s->dts;
    r.sync = (uint8_t)(s->is_sync != 0);

    if (ps->m.have_video && s->track_id == ps->m.video.track_id) {
        ps->video_samples++;
        if (g_vtail - g_vhead < VQ_CAP) g_vq[g_vtail++ % VQ_CAP] = r;
        else g_queue_overflow++;
    } else if (ps->m.have_audio && s->track_id == ps->m.audio.track_id) {
        ps->audio_samples++;
        if (g_atail - g_ahead < AQ_CAP) g_aq[g_atail++ % AQ_CAP] = r;
        else g_queue_overflow++;
    }
    return 0; /* never stop the walk early */
}

static int audio_active(const player_session *ps) { return ps->m.have_audio && !ps->audio_open_failed; }
static int video_active(const player_session *ps) { return ps->m.have_video && !ps->video_open_failed; }

/* The clock video is timed against. Audio is the primary clock (PROMPT.md
 * section 7): what the output thread has handed to the hardware, less the
 * one chunk the hardware is still holding. With no audio to read -- no
 * track, or its decoder refused -- the wall clock stands in, started the
 * first time it is asked for so it begins where playback does. */
static uint64_t playback_clock_us(player_session *ps) {
    if (audio_active(ps)) {
        uint32_t played = audio_decoder_played_frames();
        played          = played > 1024u ? played - 1024u : 0u;
        return av_rescale_us(played, ps->m.audio.sample_rate);
    }
    if (!ps->wall_origin_set) {
        ps->wall_origin_us  = platform_now_us();
        ps->wall_origin_set = 1;
    }
    return platform_now_us() - ps->wall_origin_us;
}

/* One video access unit, decided against the clock: decoded and shown when
 * due, decoded without a picture when too late to show (later frames are
 * predicted from it), not touched while it is still ahead. Returns 1 when it
 * is finished with the access unit, 0 to be asked again later. */
static int play_video(player_session *ps, const sample_ref *v) {
    uint64_t           frame_us = av_rescale_us(v->dts, ps->m.video.timescale);
    av_action          act;
    unsigned long long t0, t1;
    int                rc;

    av_sync_set_clock_us(&ps->sync, playback_clock_us(ps));
    act = av_decide(&ps->sync, frame_us, v->sync);
    if (act == AV_HOLD) return 0;
    if (g_opt->show_all) act = AV_SHOW;

    t0 = platform_now_us();
    rc = video_decoder_push(v->data, v->size, v->dts, act == AV_SHOW);
    t1 = platform_now_us();
    if (t1 - t0 > 60000u)
        trace("slow video: %u us at %u ms (sync %d, show %d), audio buffered %u", (unsigned)(t1 - t0),
              (unsigned)(frame_us / 1000u), (int)v->sync, act == AV_SHOW,
              (unsigned)(audio_active(ps) ? audio_decoder_buffered_frames() : 0u));

    if (rc != VIDEO_OK) {
        ps->video_push_failures++;
        if (ps->video_push_failures <= 20 || v->sync)
            trace("video push (sync %d) -> %d %s", (int)v->sync, rc, video_decoder_error());
        /* Not the first refusal: a decoder may reject some access units and
         * take later ones. Past the budget it stops, so the reason survives
         * to be shown; audio keeps going regardless. */
        if (ps->video_push_failures >= PLAYER_VIDEO_MAX_FAILURES) ps->video_open_failed = 1;
        return 1;
    }

    {
        uint32_t us = (uint32_t)(t1 - t0);
        ps->video_decode_us_total += us;
        ps->video_decode_count++;
        if (us > ps->video_decode_us_worst) ps->video_decode_us_worst = us;
    }

    if (act == AV_DROP) {
        av_sync_note_dropped(&ps->sync);
        return 1;
    }
    {
        video_frame vf;
        if (video_decoder_next_frame(&vf) == VIDEO_OK) {
            render_present();
            if (g_osd_line[0]) render_osd(g_osd_line);
            av_sync_note_shown(&ps->sync);
            ps->last_shown_us = platform_now_us();
            if (!ps->first_shown_us) ps->first_shown_us = ps->last_shown_us;
        }
    }
    return 1;
}

/* One turn of the play loop: feeds the audio ring as far as it has room,
 * then decodes the next video frame if the clock has reached it. Returns
 * whether anything moved. */
static int play_step(player_session *ps) {
    int progressed = 0;

    if (!audio_active(ps)) g_ahead = g_atail;
    if (!video_active(ps)) g_vhead = g_vtail;

    while (g_ahead != g_atail) {
        const sample_ref *a  = &g_aq[g_ahead % AQ_CAP];
        int               rc = audio_decoder_push(a->data, a->size, a->dts);

        if (rc == AUDIO_ERR_AGAIN) break; /* the ring is full: it is playing */
        if (rc != AUDIO_OK) {
            trace("audio push -> %d %s", rc, audio_decoder_error());
            /* Falling back to the wall clock from where the audio clock
             * stood, so the video does not jump. */
            ps->wall_origin_us    = platform_now_us() - playback_clock_us(ps);
            ps->wall_origin_set   = 1;
            ps->audio_open_failed = 1;
            g_ahead               = g_atail;
            break;
        }
        g_ahead++;
        progressed = 1;
    }

    if (g_vhead != g_vtail && play_video(ps, &g_vq[g_vhead % VQ_CAP])) {
        g_vhead++;
        progressed = 1;
    }
    return progressed;
}

/* Releases, from the front of the ring, every queued fragment whose samples
 * have all been played -- and moves the references into the fragments after
 * it down with the bytes they point at. */
static void retire_fragments(void) {
    while (g_nfrags > 0 && (int32_t)(g_vhead - g_frags[0].v_end) >= 0 && (int32_t)(g_ahead - g_frags[0].a_end) >= 0) {
        g_ring_head += g_frags[0].len;
        memmove(&g_frags[0], &g_frags[1], (g_nfrags - 1u) * sizeof g_frags[0]);
        g_nfrags--;
    }

    /* Also whenever the tail is nearly out of room: the reader must never
     * wait for space that is sitting free at the front. */
    if (g_ring_head >= RING_COMPACT_AT || (g_ring_head > 0 && MEDIA_RING_CAPACITY - g_ring_len < 512u * 1024u)) {
        size_t   len = g_ring_head;
        uint32_t k;

        ring_consume(len);
        for (k = g_vhead; k != g_vtail; k++) g_vq[k % VQ_CAP].data -= len;
        for (k = g_ahead; k != g_atail; k++) g_aq[k % AQ_CAP].data -= len;
        g_parse_off -= len;
        g_ring_head = 0;
    }
}

/* --------------------------------------------------------------- pausing
 *
 * The audio output thread stops taking from its ring and the audio clock
 * stops with it, so the picture waits exactly where the sound did. The
 * reader keeps filling the read ring meanwhile, and stops by itself once it
 * is full. */

static void draw_pause(player_session *ps) {
    char     pos[16], dur[16], line[160], title[48], status[32];
    uint64_t ticks = g_opt->start_ticks + playback_clock_us(ps) * 10u;

    text_ticks(ticks, pos, sizeof pos);
    text_to_ascii(g_opt->title ? g_opt->title : "", title, sizeof title);
    platform_status(status, sizeof status, g_opt->show_clock, g_opt->show_battery);
    if (g_opt->duration_ticks) text_ticks(g_opt->duration_ticks, dur, sizeof dur);
    else dur[0] = 0;
    /* The status first, right after the position, so the title is what gets
     * cut when the line is too long. */
    snprintf(line, sizeof line, "PAUSED %s%s%s  %s%sX resume  O stop  %s", pos, dur[0] ? " / " : "", dur, status,
             status[0] ? "  " : "", title);
    render_osd(line);
}

/* Where in the item playback is: the stream's own clock plus where the
 * stream began. */
static uint64_t position_ticks(player_session *ps) { return g_opt->start_ticks + playback_clock_us(ps) * 10u; }

/* L or R pressed: moves the target, clamped to the item, and shows it. */
static void seek_press(player_session *ps, int dir) {
    uint64_t from = g_seek_pending ? g_seek_target : position_ticks(ps);
    uint64_t end  = g_opt->duration_ticks;
    char     at[16], dur[16];

    if (dir < 0) g_seek_target = from > SEEK_STEP_TICKS ? from - SEEK_STEP_TICKS : 0;
    else g_seek_target = from + SEEK_STEP_TICKS;
    /* Not past the last few seconds: a stream asked to start at its very end
     * has nothing to send. */
    if (end > 2 * SEEK_STEP_TICKS && g_seek_target > end - SEEK_STEP_TICKS) g_seek_target = end - SEEK_STEP_TICKS;
    g_seek_pending     = 1;
    g_seek_deadline_us = platform_now_us() + SEEK_SETTLE_US;
    g_osd_until_us     = 0;

    text_ticks(g_seek_target, at, sizeof at);
    if (end) {
        text_ticks(end, dur, sizeof dur);
        snprintf(g_osd_line, sizeof g_osd_line, "%s to %s / %s   (L back 10s, R ahead 10s)", dir < 0 ? "Back" : "Ahead",
                 at, dur);
    } else {
        snprintf(g_osd_line, sizeof g_osd_line, "%s to %s   (L back 10s, R ahead 10s)", dir < 0 ? "Back" : "Ahead", at);
    }
    render_osd(g_osd_line);
}

/* Returns 0 to carry on, 1 when the person stopped from the pause screen,
 * 2 when they chose a new position with L or R and it has settled. */
static int pause_until_resumed(player_session *ps) {
    unsigned long long t0 = platform_now_us();
    int                stop = 0;

    if (audio_active(ps)) audio_decoder_pause(1);
    ps->live_paused = 1;
    trace("paused at %u ms", (unsigned)(playback_clock_us(ps) / 1000u));
    draw_pause(ps);

    while (!platform_exit_requested()) {
        unsigned                  b = input_pressed();
        static unsigned long long last_draw;

        volume_poll();
        if (osd_expire()) last_draw = 0; /* the pause line comes straight back */
        /* The clock moves on while paused -- but a volume line shown just
         * now keeps its moment. */
        if (!g_osd_until_us && platform_now_us() - last_draw > 1000000ull) {
            draw_pause(ps);
            last_draw = platform_now_us();
        }
        if (b & PSP_CTRL_CIRCLE) {
            stop = 1;
            break;
        }
        if (b & PSP_CTRL_LTRIGGER) seek_press(ps, -1);
        if (b & PSP_CTRL_RTRIGGER) seek_press(ps, 1);
        if (b & (PSP_CTRL_CROSS | PSP_CTRL_START)) {
            if (g_seek_pending) stop = 2; /* resume there, not here */
            break;
        }
        if (g_seek_pending && platform_now_us() >= g_seek_deadline_us) {
            stop = 2;
            break;
        }
        /* A pending seek's line would be covered by the pause line. */
        if (g_seek_pending) last_draw = platform_now_us();
        sceKernelDelayThread(20 * 1000);
    }

    if (audio_active(ps)) audio_decoder_pause(0);
    ps->live_paused = 0;
    /* With no audio, the wall clock is the clock, and it kept running. */
    if (!audio_active(ps) && ps->wall_origin_set) ps->wall_origin_us += platform_now_us() - t0;
    render_invalidate();
    return stop;
}

/* ------------------------------------------------------------------ run */

static void reset_state(void) {
    g_ring_len       = 0;
    g_ring_eof       = 0;
    g_ring_io_error  = 0;
    g_ring_waited_us = 0;
    g_vhead = g_vtail = g_ahead = g_atail = 0;
    g_queue_overflow = 0;
    g_nfrags         = 0;
    g_parse_off      = 0;
    g_ring_head      = 0;
    g_fatal_reason   = NULL;
    g_seek_pending   = 0;
    g_seek_target    = 0;
    g_osd_line[0]    = 0;
    g_osd_until_us   = 0;
}

/* Everything after the source opened: the init segment, the decoders.
 * Returns 0 when playback can start. */
static int start_playback(player_session *ps) {
    fmp4_err ie   = FMP4_ERR_TRUNCATED;
    size_t   want = 64u * 1024u;

    if (reader_start() < 0) {
        ps->parse_fatal = "the reader thread could not be started";
        return -1;
    }

    /* Grown into rather than filled to the brim first. The init segment
     * is a few kilobytes; against a live transcode, filling the whole ring
     * before looking at any of it would hold the screen blank for several
     * seconds of perfectly good stream, which is exactly the "download,
     * then play" shape PROMPT.md Rule 3 rules out. TRUNCATED is the only
     * answer from fmp4_init that means "ask for more" rather than "this
     * file is wrong", so it is the only one that goes round again. */
    for (;;) {
        ring_fill(want);
        memset(&ps->m, 0, sizeof ps->m);
        ie = fmp4_init(&ps->m, g_ring, g_ring_len);
        if (ie != FMP4_ERR_TRUNCATED) break;
        if (want >= MEDIA_RING_CAPACITY || g_ring_eof || g_ring_io_error || platform_exit_requested()) break;
        want *= 2;
        if (want > MEDIA_RING_CAPACITY) want = MEDIA_RING_CAPACITY;
    }

    trace("source %s open; fmp4_init -> %d with %u bytes buffered", g_source->name, (int)ie, (unsigned)g_ring_len);
    if (ie != FMP4_OK) {
        if (g_ring_io_error) ps->parse_fatal = keep(g_source->error());
        else ps->parse_fatal = ps->m.err[0] ? ps->m.err : "the stream's header (ftyp/moov) could not be parsed";
        return -1;
    }
    ring_consume(fmp4_init_size(&ps->m));

    /* Audio before video. The other order, on hardware, gives a picture
     * with no colour in it; audio claims nothing from the
     * partition (its decoder memory is the Media Engine's own EDRAM), so
     * design section 3.3's order -- the video's 4 MB block before anything
     * else -- still holds. */
    if (ps->m.have_audio && g_opt->skip_audio) {
        trace("audio decoder skipped");
        ps->audio_open_failed = 1;
    } else if (ps->m.have_audio) {
        int rc = audio_decoder_open(ps->m.audio.asc, ps->m.audio.asc_len, ps->m.audio.sample_rate, ps->m.audio.channels);
        trace("audio %u Hz %u ch open -> %d %s", (unsigned)ps->m.audio.sample_rate, (unsigned)ps->m.audio.channels, rc,
              rc != AUDIO_OK ? audio_decoder_error() : "");
        if (rc != AUDIO_OK) ps->audio_open_failed = 1;
    }
    if (ps->m.have_video && g_opt->skip_video) {
        trace("video decoder skipped");
        ps->video_open_failed = 1;
    } else if (ps->m.have_video) {
        int rc = video_decoder_open(ps->m.video.sps, ps->m.video.sps_len, ps->m.video.pps, ps->m.video.pps_len,
                                    ps->m.video.nal_length_size, (int)ps->m.video.width, (int)ps->m.video.height);
        trace("video %ux%u open -> %d %s", (unsigned)ps->m.video.width, (unsigned)ps->m.video.height, rc,
              rc != VIDEO_OK ? video_decoder_error() : "");
        if (rc != VIDEO_OK) ps->video_open_failed = 1;
    }
    return 0;
}

player_outcome player_run(player_session *ps, const media_source *src, const player_options *opt) {
    static const player_options defaults = {0};
    int                         stopped  = 0;
    player_outcome              outcome;

    memset(ps, 0, sizeof *ps);
    g_source = src;
    g_opt    = opt ? opt : &defaults;
    reset_state();
    av_sync_init(&ps->sync, VIDEO_QUEUE_MAX, AV_SYNC_DEFAULT_LATE_THRESHOLD_US);
    if (g_opt->interactive) input_pressed(); /* forget the press that started playback */

    if (start_playback(ps) != 0) {
        ps->done_parsing = 1;
    } else {
        /* ------------------------------------------------ the play loop
         *
         * Three things per turn, none of which waits: walk and queue
         * fragments while there is room to read ahead, play whatever is
         * due, and release fragments that have been played. Only when none
         * of them could move does it sleep, briefly. HOME and the buttons
         * are looked at every turn. */
        int                eof        = 0;
        unsigned long long starved_us = 0; /* nothing queued, nothing arrived: the stream is late */

        while (!ps->done_parsing && !platform_exit_requested()) {
            int progressed = 0;

            if (g_opt->interactive) {
                unsigned b = input_pressed();
                if (b & PSP_CTRL_CIRCLE) {
                    stopped = 1;
                    break;
                }
                if (b & PSP_CTRL_LTRIGGER) seek_press(ps, -1);
                if (b & PSP_CTRL_RTRIGGER) seek_press(ps, 1);
                if (b & (PSP_CTRL_CROSS | PSP_CTRL_START)) {
                    int r = pause_until_resumed(ps);
                    if (r == 1) {
                        stopped = 1;
                        break;
                    }
                    if (r == 2) break; /* g_seek_pending: reopen there */
                }
                if (g_seek_pending && platform_now_us() >= g_seek_deadline_us) break;
                volume_poll();
                osd_expire();
            }

            while (!eof && !ps->parse_fatal && g_nfrags < FRAGS_AHEAD &&
                   VQ_CAP - (g_vtail - g_vhead) >= FRAG_VIDEO_MAX && AQ_CAP - (g_atail - g_ahead) >= FRAG_AUDIO_MAX) {
                size_t      len         = 0;
                int         is_fragment = 0;
                span_result sr          = span_at(g_parse_off, &len, &is_fragment);

                if (sr == SPAN_NEED) break;
                if (sr == SPAN_EOF) {
                    eof = 1;
                    break;
                }
                if (sr == SPAN_FATAL) {
                    ps->parse_fatal = g_fatal_reason;
                    break;
                }
                if (is_fragment) {
                    size_t   consumed = 0;
                    fmp4_err fe       = fmp4_fragment(&ps->m, g_ring + g_parse_off, len, on_sample, ps, &consumed);
                    if (fe != FMP4_OK) {
                        ps->parse_fatal = ps->m.err[0] ? ps->m.err : "a fragment could not be parsed";
                        break;
                    }
                    ps->fragments_parsed++;
                    /* Held in memory from the first fragment on: a line
                     * written mid-playback costs the audio more than it can
                     * spare. */
                    trace_defer(1);
                    /* Every fragment for the first minute, then one a
                     * minute, and any time the audio is running low: a
                     * whole film logged fragment by fragment overflowed the
                     * held-log buffer and lost its own summary (2026-09-24). */
                    if (ps->fragments_parsed <= 12 || ps->fragments_parsed % 12 == 0 ||
                        (audio_active(ps) && audio_decoder_buffered_frames() < 4096u))
                    trace("fragment %u queued: %u video %u audio waiting, clock %u ms, audio buffered %u, starved %u ms, "
                          "ring %u kB",
                          (unsigned)ps->fragments_parsed, (unsigned)(g_vtail - g_vhead), (unsigned)(g_atail - g_ahead),
                          (unsigned)(playback_clock_us(ps) / 1000u),
                          (unsigned)(audio_active(ps) ? audio_decoder_buffered_frames() : 0u),
                          (unsigned)(starved_us / 1000u), (unsigned)(g_ring_len / 1024u));
                    memwatch_sample(platform_free_kb(), platform_largest_free_kb());
                }
                g_frags[g_nfrags].len   = len;
                g_frags[g_nfrags].v_end = g_vtail;
                g_frags[g_nfrags].a_end = g_atail;
                g_nfrags++;
                g_parse_off += len;
                progressed = 1;
            }

            if (play_step(ps)) progressed = 1;
            retire_fragments();
            ps->live_ms = (uint32_t)(playback_clock_us(ps) / 1000u);

            if (g_vhead == g_vtail && g_ahead == g_atail) {
                if (eof || ps->parse_fatal) ps->done_parsing = 1;
                else if (!progressed) starved_us += 2000u;
            }
            if (!progressed && !ps->done_parsing) sceKernelDelayThread(2000);
        }
        trace("starved for %u ms in all", (unsigned)(starved_us / 1000u));
    }

    if (platform_exit_requested()) outcome = PLAYER_HOME;
    else if (stopped) outcome = PLAYER_STOPPED;
    else if (g_seek_pending) {
        outcome           = PLAYER_SEEK;
        ps->seek_to_ticks = g_seek_target;
    }
    else if (ps->parse_fatal) outcome = PLAYER_FAILED;
    else outcome = PLAYER_ENDED;

    ps->position_us     = (ps->first_shown_us || audio_active(ps) || ps->wall_origin_set) ? playback_clock_us(ps) : 0u;
    ps->audio_underruns = audio_active(ps) ? audio_decoder_underruns() : 0u;
    ps->queue_overflow  = g_queue_overflow;
    trace("playback %s: %s; shown %u dropped %u, decode avg %u us worst %u us, underruns %u, overflow %u",
          outcome == PLAYER_HOME ? "left by HOME" : outcome == PLAYER_STOPPED ? "stopped" :
          outcome == PLAYER_FAILED ? "failed" : outcome == PLAYER_SEEK ? "left to seek" : "ended",
          ps->parse_fatal ? ps->parse_fatal : "-", (unsigned)ps->sync.frames_shown, (unsigned)ps->sync.frames_dropped,
          ps->video_decode_count ? (unsigned)(ps->video_decode_us_total / ps->video_decode_count) : 0u,
          (unsigned)ps->video_decode_us_worst, (unsigned)ps->audio_underruns, (unsigned)g_queue_overflow);
    {
        uint32_t k;
        for (k = 0; k < ps->audio_underruns && k < 32u; k++)
            trace("underrun %u at %u ms", (unsigned)k,
                  (unsigned)(av_rescale_us(audio_decoder_underrun_at(k), ps->m.audio.sample_rate) / 1000u));
    }
    trace_defer(0); /* everything held during playback, written now */

    /* At the end of the stream, the last of the sound plays out before the
     * decoders close; a stop is meant to be immediate. */
    if (outcome == PLAYER_ENDED) {
        int waited;
        for (waited = 0; waited < 50 && audio_active(ps) && audio_decoder_buffered_frames() > 0; waited++)
            sceKernelDelayThread(10 * 1000);
    }

    reader_stop();
    audio_decoder_close();
    video_decoder_close();
    return outcome;
}

#else

typedef int player_needs_a_psp;

#endif /* __PSP__ */
