/* See media/fmp4.h. Checks 1-3 are synthetic and need no fixture, so they run
 * everywhere; checks 4-8 read fixtures/probe360.mp4 and skip (not fail) if it
 * cannot be opened, per tests/test.h's rule that a skip is not a pass -- the
 * skip still costs the run its green exit code, it just does not lie about
 * having verified anything.
 *
 * Written before src/media/fmp4.c exists, so the first run of this file is
 * expected to fail to LINK -- see Step 4 of the task. That failure is itself
 * evidence the checks call real fmp4_* entry points rather than nothing. */

#include "media/fmp4.h"

#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- a tiny box builder, for the synthetic checks -------------------- */

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} byte_buf;

static void bb_init(byte_buf *b) {
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

static void bb_free(byte_buf *b) { free(b->data); }

static void bb_append(byte_buf *b, const void *p, size_t n) {
    if (b->len + n > b->cap) {
        size_t newcap = b->cap ? b->cap * 2 : 64;
        while (newcap < b->len + n) newcap *= 2;
        b->data = (uint8_t *)realloc(b->data, newcap);
        b->cap  = newcap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

static void bb_u8(byte_buf *b, uint8_t v) { bb_append(b, &v, 1); }

static void bb_u16(byte_buf *b, uint16_t v) {
    uint8_t be[2];
    be[0] = (uint8_t)(v >> 8);
    be[1] = (uint8_t)v;
    bb_append(b, be, 2);
}

static void bb_u32(byte_buf *b, uint32_t v) {
    uint8_t be[4];
    be[0] = (uint8_t)(v >> 24);
    be[1] = (uint8_t)(v >> 16);
    be[2] = (uint8_t)(v >> 8);
    be[3] = (uint8_t)v;
    bb_append(b, be, 4);
}

static void bb_zeros(byte_buf *b, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) bb_u8(b, 0);
}

/* Wraps `content` in a box of the given four-character type and appends the
 * result to `out`; `out` and `content` may not be the same buffer. */
static void bb_box(byte_buf *out, const char *type, const byte_buf *content) {
    bb_u32(out, (uint32_t)(8 + content->len));
    bb_append(out, type, 4);
    if (content->len) bb_append(out, content->data, content->len);
}

/* Builds the smallest moov that reaches fmp4_init's avcC parsing: one video
 * trak whose avcC content is exactly `avcc`. Nothing above stbl is populated
 * beyond what parsing actually looks at, per this reader's own scope. */
static void build_video_only_moov(byte_buf *out, const byte_buf *avcc) {
    byte_buf avc1, stsd, stbl, minf, mdhd, mdia, tkhd, trak, moov;

    bb_init(&avc1);
    bb_zeros(&avc1, 78); /* reserved/data_ref_index/pre_defined/width/height/... up to where boxes start */
    bb_box(&avc1, "avcC", avcc);

    bb_init(&stsd);
    bb_u32(&stsd, 0);  /* version+flags */
    bb_u32(&stsd, 1);  /* entry_count */
    bb_box(&stsd, "avc1", &avc1);
    bb_free(&avc1);

    bb_init(&stbl);
    bb_box(&stbl, "stsd", &stsd);
    bb_free(&stsd);

    bb_init(&minf);
    bb_box(&minf, "stbl", &stbl);
    bb_free(&stbl);

    bb_init(&mdhd);
    bb_u32(&mdhd, 0);     /* version+flags */
    bb_u32(&mdhd, 0);     /* creation_time */
    bb_u32(&mdhd, 0);     /* modification_time */
    bb_u32(&mdhd, 12800); /* timescale */
    bb_u32(&mdhd, 0);     /* duration */

    bb_init(&mdia);
    bb_box(&mdia, "mdhd", &mdhd);
    bb_free(&mdhd);
    bb_box(&mdia, "minf", &minf);
    bb_free(&minf);

    bb_init(&tkhd);
    bb_u32(&tkhd, 0); /* version+flags */
    bb_u32(&tkhd, 0); /* creation_time */
    bb_u32(&tkhd, 0); /* modification_time */
    bb_u32(&tkhd, 1); /* track_id */

    bb_init(&trak);
    bb_box(&trak, "tkhd", &tkhd);
    bb_free(&tkhd);
    bb_box(&trak, "mdia", &mdia);
    bb_free(&mdia);

    bb_init(&moov);
    bb_box(&moov, "trak", &trak);
    bb_free(&trak);

    bb_init(out);
    bb_box(out, "moov", &moov);
    bb_free(&moov);
}

/* ---- fixture loading --------------------------------------------------- */

/* The test binary runs from build/host/, two levels under the repo root. */
static uint8_t *load_fixture(size_t *out_len) {
    static const char *candidates[2] = {"fixtures/probe360.mp4", "../../fixtures/probe360.mp4"};
    unsigned               i;

    for (i = 0; i < 2; i++) {
        FILE  *f = fopen(candidates[i], "rb");
        long   sz;
        uint8_t *buf;

        if (!f) continue;
        if (fseek(f, 0, SEEK_END) != 0) {
            fclose(f);
            continue;
        }
        sz = ftell(f);
        if (sz <= 0) {
            fclose(f);
            continue;
        }
        rewind(f);
        buf = (uint8_t *)malloc((size_t)sz);
        if (!buf) {
            fclose(f);
            continue;
        }
        if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
            free(buf);
            fclose(f);
            continue;
        }
        fclose(f);
        *out_len = (size_t)sz;
        return buf;
    }
    return NULL;
}

/* ---- 1: a box smaller than its own header ------------------------------ */

static int t_box_smaller_than_header(char *note, unsigned n) {
    uint8_t  buf[16];
    fmp4     m;
    fmp4_err e;

    memset(buf, 0, sizeof buf);
    buf[3] = 4; /* size = 4, less than the 8-byte header */
    memcpy(buf + 4, "moov", 4);

    e = fmp4_init(&m, buf, sizeof buf);
    if (e != FMP4_ERR_MALFORMED) {
        snprintf(note, n, "expected FMP4_ERR_MALFORMED, got %d (%s)", (int)e, m.err);
        return 1;
    }
    snprintf(note, n, "rejected without looping: %s", m.err);
    return 0;
}

/* ---- 2: a box longer than the buffer ----------------------------------- */

static int t_box_longer_than_buffer(char *note, unsigned n) {
    uint8_t  buf[32];
    fmp4     m;
    fmp4_err e;

    memset(buf, 0, sizeof buf);
    buf[0] = 0x7F;
    buf[1] = 0xFF;
    buf[2] = 0xFF;
    buf[3] = 0xFF; /* size = 0x7FFFFFFF */
    memcpy(buf + 4, "moov", 4);

    e = fmp4_init(&m, buf, sizeof buf);
    if (e != FMP4_ERR_TRUNCATED) {
        snprintf(note, n, "expected FMP4_ERR_TRUNCATED, got %d (%s)", (int)e, m.err);
        return 1;
    }
    snprintf(note, n, "truncated, not read past the end: %s", m.err);
    return 0;
}

/* ---- 3: an SPS longer than the buffer ---------------------------------- */

static int t_sps_longer_than_buffer(char *note, unsigned n) {
    byte_buf avcc, moov;
    fmp4     m;
    fmp4_err e;

    /* configurationVersion, profile, compat, level, lengthSizeMinusOne=3,
     * numSPS=1, then an SPS length that outruns both FMP4_MAX_PARAM_SET and
     * the avcC box itself -- no SPS bytes follow it at all. */
    bb_init(&avcc);
    bb_u8(&avcc, 1);
    bb_u8(&avcc, 0x42);
    bb_u8(&avcc, 0x00);
    bb_u8(&avcc, 0x0d);
    bb_u8(&avcc, 0xFF);
    bb_u8(&avcc, 0xE1);
    bb_u16(&avcc, 0xFFFF);

    build_video_only_moov(&moov, &avcc);
    bb_free(&avcc);

    e = fmp4_init(&m, moov.data, moov.len);
    bb_free(&moov);

    if (e != FMP4_ERR_TOOBIG && e != FMP4_ERR_TRUNCATED) {
        snprintf(note, n, "expected TOOBIG or TRUNCATED, got %d (%s)", (int)e, m.err);
        return 1;
    }
    snprintf(note, n, "refused rather than copied: %s", m.err);
    return 0;
}

/* ---- 4: the fixture's parameter sets ----------------------------------- */

static int t_fixture_parameter_sets(char *note, unsigned n) {
    uint8_t *file;
    size_t   len;
    fmp4     m;
    fmp4_err e;

    file = load_fixture(&len);
    if (!file) {
        snprintf(note, n, "fixtures/probe360.mp4 not found -- skipping");
        return -1;
    }

    e = fmp4_init(&m, file, len);
    free(file);
    if (e != FMP4_OK) {
        snprintf(note, n, "fmp4_init failed: %s", m.err);
        return 1;
    }
    if (!m.have_video) {
        snprintf(note, n, "no video track found");
        return 1;
    }
    if (m.video.nal_length_size != 4 || m.video.width != 360 || m.video.height != 204 || m.video.sps_len == 0 ||
        m.video.pps_len == 0 || (m.video.sps[0] & 0x1F) != 7 || (m.video.pps[0] & 0x1F) != 8) {
        snprintf(note, n, "nal_length_size=%u %ux%u sps_len=%u pps_len=%u sps_nal=%u pps_nal=%u",
                 m.video.nal_length_size, m.video.width, m.video.height, m.video.sps_len, m.video.pps_len,
                 m.video.sps[0] & 0x1F, m.video.pps[0] & 0x1F);
        return 1;
    }
    snprintf(note, n, "%ux%u, nal_length_size=4, sps_len=%u, pps_len=%u", m.video.width, m.video.height,
             m.video.sps_len, m.video.pps_len);
    return 0;
}

/* ---- 5: the fixture's audio config ------------------------------------- */

static int t_fixture_audio_config(char *note, unsigned n) {
    uint8_t *file;
    size_t   len;
    fmp4     m;
    fmp4_err e;

    file = load_fixture(&len);
    if (!file) {
        snprintf(note, n, "fixtures/probe360.mp4 not found -- skipping");
        return -1;
    }

    e = fmp4_init(&m, file, len);
    free(file);
    if (e != FMP4_OK) {
        snprintf(note, n, "fmp4_init failed: %s", m.err);
        return 1;
    }
    if (!m.have_audio) {
        snprintf(note, n, "no audio track found");
        return 1;
    }
    if (m.audio.sample_rate != 44100 || m.audio.channels != 2 || m.audio.asc_len == 0) {
        snprintf(note, n, "sample_rate=%u channels=%u asc_len=%u", m.audio.sample_rate, m.audio.channels,
                 m.audio.asc_len);
        return 1;
    }
    snprintf(note, n, "44100 Hz, %u channels, asc_len=%u", m.audio.channels, m.audio.asc_len);
    return 0;
}

/* ---- 6: the first fragment yields samples in order --------------------- */

typedef struct {
    const uint8_t *buf_start;
    size_t         buf_len;
    uint32_t       video_track_id;
    uint32_t       audio_track_id;
    uint64_t       last_dts_video;
    uint64_t       last_dts_audio;
    int            have_last_video;
    int            have_last_audio;
    int            order_ok;
    int            bounds_ok;
    int            first_video_seen;
    int            first_video_is_sync;
    unsigned       count;
} order_ctx;

static int on_sample_order(const fmp4_sample *s, void *user) {
    order_ctx *c = (order_ctx *)user;

    c->count++;
    if (s->data < c->buf_start || s->data + s->size > c->buf_start + c->buf_len) c->bounds_ok = 0;

    if (s->track_id == c->video_track_id) {
        if (!c->first_video_seen) {
            c->first_video_seen    = 1;
            c->first_video_is_sync = s->is_sync;
        }
        if (c->have_last_video && s->dts < c->last_dts_video) c->order_ok = 0;
        c->last_dts_video  = s->dts;
        c->have_last_video = 1;
    } else if (s->track_id == c->audio_track_id) {
        if (c->have_last_audio && s->dts < c->last_dts_audio) c->order_ok = 0;
        c->last_dts_audio  = s->dts;
        c->have_last_audio = 1;
    }
    return 0;
}

static int t_first_fragment_in_order(char *note, unsigned n) {
    uint8_t  *file;
    size_t    len, consumed;
    fmp4      m;
    fmp4_err  e;
    order_ctx ctx;

    file = load_fixture(&len);
    if (!file) {
        snprintf(note, n, "fixtures/probe360.mp4 not found -- skipping");
        return -1;
    }

    e = fmp4_init(&m, file, len);
    if (e != FMP4_OK) {
        snprintf(note, n, "fmp4_init failed: %s", m.err);
        free(file);
        return 1;
    }

    memset(&ctx, 0, sizeof ctx);
    ctx.order_ok        = 1;
    ctx.bounds_ok        = 1;
    ctx.buf_start        = file;
    ctx.buf_len          = len;
    ctx.video_track_id   = m.video.track_id;
    ctx.audio_track_id   = m.audio.track_id;

    e = fmp4_fragment(&m, file + fmp4_init_size(&m), len - fmp4_init_size(&m), on_sample_order, &ctx, &consumed);
    free(file);

    if (e != FMP4_OK) {
        snprintf(note, n, "fmp4_fragment failed: %s", m.err);
        return 1;
    }
    if (ctx.count == 0) {
        snprintf(note, n, "no samples emitted");
        return 1;
    }
    if (!ctx.order_ok) {
        snprintf(note, n, "DTS went backwards within a track");
        return 1;
    }
    if (!ctx.bounds_ok) {
        snprintf(note, n, "a sample's data+size fell outside the buffer");
        return 1;
    }
    if (!ctx.first_video_seen || !ctx.first_video_is_sync) {
        snprintf(note, n, "first video sample was not sync");
        return 1;
    }
    snprintf(note, n, "%u samples, %zu bytes consumed, first video sample sync", ctx.count, consumed);
    return 0;
}

/* ---- 7: walking the whole file consumes it exactly ---------------------- */

typedef struct {
    uint32_t video_track_id;
    unsigned video_count;
} count_ctx;

static int on_sample_count(const fmp4_sample *s, void *user) {
    count_ctx *c = (count_ctx *)user;
    if (s->track_id == c->video_track_id) c->video_count++;
    return 0;
}

static int t_whole_file_consumed_exactly(char *note, unsigned n) {
    uint8_t *file;
    size_t   len;
    fmp4     m;
    fmp4_err e;
    size_t   cursor, init_size, total_consumed;
    unsigned video_samples;

    file = load_fixture(&len);
    if (!file) {
        snprintf(note, n, "fixtures/probe360.mp4 not found -- skipping");
        return -1;
    }

    e = fmp4_init(&m, file, len);
    if (e != FMP4_OK) {
        snprintf(note, n, "fmp4_init failed: %s", m.err);
        free(file);
        return 1;
    }

    init_size      = fmp4_init_size(&m);
    cursor         = init_size;
    total_consumed = 0;
    video_samples  = 0;

    while (cursor < len) {
        size_t    consumed = 0;
        count_ctx cctx;

        cctx.video_track_id = m.video.track_id;
        cctx.video_count    = 0;

        e = fmp4_fragment(&m, file + cursor, len - cursor, on_sample_count, &cctx, &consumed);
        if (e != FMP4_OK) {
            snprintf(note, n, "fmp4_fragment failed at byte %zu: %s", cursor, m.err);
            free(file);
            return 1;
        }
        if (consumed == 0) {
            snprintf(note, n, "no progress at byte %zu -- would loop forever", cursor);
            free(file);
            return 1;
        }
        video_samples += cctx.video_count;
        cursor += consumed;
        total_consumed += consumed;
    }

    free(file);

    if (total_consumed != len - init_size) {
        snprintf(note, n, "consumed %zu, expected %zu", total_consumed, len - init_size);
        return 1;
    }
    if (video_samples < 600 || video_samples > 900) {
        snprintf(note, n, "%u video samples, expected 600..900", video_samples);
        return 1;
    }
    snprintf(note, n, "consumed %zu bytes exactly (file minus init), %u video samples", total_consumed,
             video_samples);
    return 0;
}

/* ---- 8: mutated headers are refused, never crashed on ------------------- */

static int t_mutated_headers_never_crash(char *note, unsigned n) {
    uint8_t *file;
    size_t   len, chunk_len;
    uint8_t *chunk;
    uint32_t lcg;
    int      i;
    int      tally[FMP4_ERR_TOOBIG + 1];

    file = load_fixture(&len);
    if (!file) {
        snprintf(note, n, "fixtures/probe360.mp4 not found -- skipping");
        return -1;
    }

    chunk_len = len < 4096 ? len : 4096;
    chunk     = (uint8_t *)malloc(chunk_len);
    if (!chunk) {
        free(file);
        snprintf(note, n, "malloc failed");
        return 1;
    }
    memcpy(chunk, file, chunk_len);
    free(file);

    memset(tally, 0, sizeof tally);
    lcg = 0x2545F491u; /* fixed seed: a failing seed here reproduces exactly */

    for (i = 0; i < 200; i++) {
        fmp4     m;
        fmp4_err e;
        size_t   idx;
        uint8_t  orig;

        lcg = lcg * 1103515245u + 12345u;
        idx = lcg % chunk_len;

        orig       = chunk[idx];
        chunk[idx] = (uint8_t)(orig ^ 0xFF); /* flip the byte */

        e          = fmp4_init(&m, chunk, chunk_len);
        chunk[idx] = orig;

        if ((int)e < FMP4_OK || (int)e > FMP4_ERR_TOOBIG) {
            snprintf(note, n, "iteration %d (offset %zu): out-of-range return %d", i, idx, (int)e);
            free(chunk);
            return 1;
        }
        tally[e]++;
    }

    free(chunk);
    snprintf(note, n, "200 mutations: ok=%d truncated=%d malformed=%d unsupported=%d toobig=%d", tally[FMP4_OK],
             tally[FMP4_ERR_TRUNCATED], tally[FMP4_ERR_MALFORMED], tally[FMP4_ERR_UNSUPPORTED],
             tally[FMP4_ERR_TOOBIG]);
    return 0;
}

/* A 64-bit largesize near 2^64 once wrapped `off + size > len` to a small
 * number, passed the bounds check, and handed a near-2^64 content_size to a
 * child parser that walked off the end of the allocation. Twenty-eight bytes
 * of input reached it. The fix computes `size > len - off` instead, and this
 * check is what stops it coming back. */
static int t_a_largesize_that_wraps_is_refused(char *note, unsigned n) {
    static const uint8_t buf[28] = {
        0x00, 0x00, 0x00, 0x08, 'f',  't',  'y',  'p',  /* a real box, so off advances to 8 */
        0x00, 0x00, 0x00, 0x01, 'm',  'o',  'o',  'v',  /* size == 1: a largesize follows */
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF8, /* 2^64 - 8; 8 + this wraps to 0 */
        0x00, 0x00, 0x00, 0x00
    };
    fmp4     m;
    fmp4_err e = fmp4_init(&m, buf, sizeof(buf));

    if (e == FMP4_OK) {
        snprintf(note, n, "a 2^64-8 largesize was accepted");
        return 1;
    }
    if (e != FMP4_ERR_TRUNCATED) {
        snprintf(note, n, "refused, but as %d rather than truncation", (int)e);
        return 1;
    }
    snprintf(note, n, "2^64-8 largesize at offset 8 refused as truncation, not wrapped");
    return 0;
}

/* The same arithmetic, reached through the fragment path rather than init. */
static int t_a_wrapping_largesize_in_a_fragment_is_refused(char *note, unsigned n) {
    static const uint8_t buf[28] = {
        0x00, 0x00, 0x00, 0x08, 's',  't',  'y',  'p',
        0x00, 0x00, 0x00, 0x01, 'm',  'o',  'o',  'f',
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF8,
        0x00, 0x00, 0x00, 0x00
    };
    fmp4     m;
    size_t   consumed = 0;
    fmp4_err e;

    memset(&m, 0, sizeof(m));
    e = fmp4_fragment(&m, buf, sizeof(buf), 0, 0, &consumed);

    if (e == FMP4_OK) {
        snprintf(note, n, "the fragment walker accepted a wrapping largesize");
        return 1;
    }
    snprintf(note, n, "refused through the fragment path too");
    return 0;
}

/* ---- 9: mutated fragments are refused, never spun on -------------------- */

/* Check 8 above mutates bytes in [0, 4096) and calls only fmp4_init, which
 * stops at the end of moov -- byte 1399 in this fixture. Two thirds of its
 * mutations therefore land in mdat bytes fmp4_init never reads: no-ops that
 * pad a green tally while fmp4_fragment, parse_moof_and_emit,
 * parse_traf_and_emit, parse_tfhd, parse_tfdt and parse_trun_and_emit see no
 * malformed input at all. This check covers exactly that half of the reader.
 *
 * Mutation offsets are drawn from inside moof boxes (and each mdat's header),
 * found by a one-pass scan of the region, rather than uniformly: a byte flip
 * in the middle of a 283 kB mdat payload changes no parsing decision, and a
 * fuzzer whose iterations mostly do nothing is the problem this replaces.
 *
 * What it asserts, beyond "did not crash":
 *   - the walk TERMINATES. Both a callback cap and an iteration cap are
 *     failures when hit, not passes -- a parser that spins on malformed input
 *     freezes the console for minutes, and that is the whole point here.
 *   - every emitted sample's data+size lies inside the buffer handed in.
 *   - a FMP4_OK return always advances `consumed`; zero advance is a failure,
 *     because main.c's loop would spin on it forever.
 * ASan (scripts/test.sh compiles with /fsanitize=address) is what turns a
 * modest over-read into a loud failure rather than a byte from the next
 * fragment; the region is copied into an exact-sized allocation so that a
 * read one past the end has no slack to land in. */

#define FRAG_FUZZ_ITERATIONS 6000
#define FRAG_FUZZ_CALL_CAP   100000u
#define FRAG_FUZZ_LOOP_CAP   100000u
#define FRAG_FUZZ_MAX_RANGES 128

typedef struct {
    const uint8_t *buf_start;
    size_t         buf_len;
    unsigned long  calls;
    int            bounds_ok;
    int            capped;
} frag_fuzz_ctx;

static int on_sample_frag_fuzz(const fmp4_sample *s, void *user) {
    frag_fuzz_ctx *c = (frag_fuzz_ctx *)user;

    c->calls++;

    if (s->data < c->buf_start) {
        c->bounds_ok = 0;
    } else {
        size_t off = (size_t)(s->data - c->buf_start);
        if (off > c->buf_len || (size_t)s->size > c->buf_len - off) c->bounds_ok = 0;
    }

    /* Stop the walk once the cap is reached -- the outer loop turns that into
     * a failure. Without this a single runaway trun would never return. */
    if (c->calls > FRAG_FUZZ_CALL_CAP) {
        c->capped = 1;
        return 1;
    }
    return 0;
}

typedef struct {
    size_t off;
    size_t len;
} fuzz_range;

static uint32_t fuzz_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int t_mutated_fragments_never_spun_on(char *note, unsigned n) {
    uint8_t   *file;
    uint8_t   *region;
    size_t     len, init_size, region_len, scan;
    fmp4       m0;
    fmp4_err   e;
    uint32_t   lcg;
    unsigned   nranges = 0;
    fuzz_range ranges[FRAG_FUZZ_MAX_RANGES];
    int        tally[FMP4_ERR_TOOBIG + 1];
    int        iter;
    unsigned   emitted_iterations = 0;

    file = load_fixture(&len);
    if (!file) {
        snprintf(note, n, "fixtures/probe360.mp4 not found -- skipping");
        return -1;
    }

    e = fmp4_init(&m0, file, len);
    if (e != FMP4_OK) {
        snprintf(note, n, "fmp4_init failed on the fixture: %s", m0.err);
        free(file);
        return 1;
    }
    init_size = fmp4_init_size(&m0);
    if (init_size >= len) {
        snprintf(note, n, "fixture has no fragment region after init (%zu of %zu bytes)", init_size, len);
        free(file);
        return 1;
    }

    region_len = len - init_size;
    region     = (uint8_t *)malloc(region_len); /* exact size: no slack for an over-read to hide in */
    if (!region) {
        snprintf(note, n, "malloc failed");
        free(file);
        return 1;
    }
    memcpy(region, file + init_size, region_len);
    free(file);

    /* One pass over the top-level boxes, reading each 32-bit size the same way
     * main.c's next_fragment_span does, to find the bytes a parsing decision
     * actually depends on. */
    scan = 0;
    while (scan + 8 <= region_len && nranges < FRAG_FUZZ_MAX_RANGES) {
        uint32_t sz = fuzz_be32(region + scan);
        uint32_t ty = fuzz_be32(region + scan + 4);

        if (sz < 8 || (size_t)sz > region_len - scan) break;
        if (ty == 0x6D6F6F66u) { /* moof: every header the fragment path reads */
            ranges[nranges].off = scan;
            ranges[nranges].len = sz;
            nranges++;
        } else if (ty == 0x6D646174u) { /* mdat: only its header decides anything */
            ranges[nranges].off = scan;
            ranges[nranges].len = sz < 16 ? (size_t)sz : 16;
            nranges++;
        }
        scan += sz;
    }

    if (nranges == 0) {
        snprintf(note, n, "no moof/mdat boxes found in the fragment region");
        free(region);
        return 1;
    }

    memset(tally, 0, sizeof tally);
    lcg = 0x13579BDFu; /* fixed seed 0x13579BDF: a failing iteration reproduces exactly */

    for (iter = 0; iter < FRAG_FUZZ_ITERATIONS; iter++) {
        fmp4          m;
        frag_fuzz_ctx ctx;
        fuzz_range    r;
        size_t        idx, cursor;
        unsigned long loops;
        uint8_t       orig;
        fmp4_err      fe = FMP4_OK;

        lcg = lcg * 1103515245u + 12345u;
        r   = ranges[(lcg >> 16) % nranges];
        lcg = lcg * 1103515245u + 12345u;
        idx = r.off + (size_t)(lcg % (uint32_t)r.len);

        orig        = region[idx];
        region[idx] = (uint8_t)(orig ^ 0xFF);

        memcpy(&m, &m0, sizeof m); /* the init state is unmutated, so it is copied, not re-parsed */
        memset(&ctx, 0, sizeof ctx);
        ctx.bounds_ok = 1;

        cursor = 0;
        loops  = 0;

        while (cursor < region_len) {
            size_t consumed = 0;

            ctx.buf_start = region + cursor;
            ctx.buf_len   = region_len - cursor;

            fe = fmp4_fragment(&m, region + cursor, region_len - cursor, on_sample_frag_fuzz, &ctx, &consumed);
            if (fe != FMP4_OK) break;

            if (consumed == 0) {
                snprintf(note, n, "iteration %d (offset %zu): FMP4_OK with consumed == 0 at %zu -- main.c would spin",
                         iter, idx, cursor);
                region[idx] = orig;
                free(region);
                return 1;
            }
            if (consumed > region_len - cursor) {
                snprintf(note, n, "iteration %d (offset %zu): consumed %zu of %zu remaining", iter, idx, consumed,
                         region_len - cursor);
                region[idx] = orig;
                free(region);
                return 1;
            }
            cursor += consumed;

            if (++loops > FRAG_FUZZ_LOOP_CAP) {
                snprintf(note, n, "iteration %d (offset %zu): hit the %u-iteration walk cap -- did not terminate", iter,
                         idx, (unsigned)FRAG_FUZZ_LOOP_CAP);
                region[idx] = orig;
                free(region);
                return 1;
            }
            if (ctx.capped) break;
        }

        region[idx] = orig;

        if (ctx.capped || ctx.calls > FRAG_FUZZ_CALL_CAP) {
            snprintf(note, n, "iteration %d (offset %zu): hit the %u-sample cap -- did not terminate", iter, idx,
                     (unsigned)FRAG_FUZZ_CALL_CAP);
            free(region);
            return 1;
        }
        if (!ctx.bounds_ok) {
            snprintf(note, n, "iteration %d (offset %zu): a sample's data+size fell outside the buffer", iter, idx);
            free(region);
            return 1;
        }
        if ((int)fe < FMP4_OK || (int)fe > FMP4_ERR_TOOBIG) {
            snprintf(note, n, "iteration %d (offset %zu): out-of-range return %d", iter, idx, (int)fe);
            free(region);
            return 1;
        }
        tally[fe]++;
        if (ctx.calls) emitted_iterations++;
    }

    free(region);
    snprintf(note, n,
             "%d mutations in %u moof/mdat ranges (seed 0x13579BDF): ok=%d truncated=%d malformed=%d unsupported=%d "
             "toobig=%d; %u emitted samples, all in bounds, all terminated",
             FRAG_FUZZ_ITERATIONS, nranges, tally[FMP4_OK], tally[FMP4_ERR_TRUNCATED], tally[FMP4_ERR_MALFORMED],
             tally[FMP4_ERR_UNSUPPORTED], tally[FMP4_ERR_TOOBIG], emitted_iterations);
    return 0;
}

/* ---- 10: a leading box before the moof does not shift the samples ------- */

/* trun's data_offset is defined relative to the first byte of the enclosing
 * moof, not to wherever the caller's buffer happens to start. Those two
 * coincide only when the moof is at offset 0 -- which fmp4_fragment does not
 * require, since it tolerates leading boxes it does not interpret. A styp,
 * free, skip, sidx or emsg in front therefore shifted every sample slice by
 * that box's size: still inside the buffer, so every bounds check passed, but
 * the wrong bytes, and misaligned NAL length prefixes then reached the Media
 * Engine. Jellyfin emits a styp per segment, so this is not a hypothetical
 * input.
 *
 * The fragment is built twice, with and without the leading box, and the
 * emitted sample bytes must be identical. */

typedef struct {
    unsigned count;
    uint8_t  bytes[64];
    size_t   len;
    int      overflow;
} capture_ctx;

static int on_sample_capture(const fmp4_sample *s, void *user) {
    capture_ctx *c = (capture_ctx *)user;

    c->count++;
    if (s->size > sizeof c->bytes - c->len) {
        c->overflow = 1;
        return 1;
    }
    memcpy(c->bytes + c->len, s->data, s->size);
    c->len += s->size;
    return 0;
}

#define SYN_PAYLOAD_LEN 10
#define SYN_SAMPLE_A    4
#define SYN_SAMPLE_B    6

/* One moof carrying one traf: tfhd (track 1), tfdt (dts 0), trun (two
 * samples, each with its own duration and size, and the given data_offset).
 * `bdo_present` sets tfhd's base-data-offset-present flag and writes the
 * eight bytes that go with it. */
static void build_syn_moof(byte_buf *out, int32_t data_offset, int bdo_present, uint32_t bdo_value) {
    byte_buf tfhd, tfdt, trun, traf, moof;

    bb_init(&tfhd);
    bb_u32(&tfhd, bdo_present ? 0x00000001u : 0x00000000u); /* version + tf_flags */
    bb_u32(&tfhd, 1);                                       /* track_id */
    if (bdo_present) {
        bb_u32(&tfhd, 0);         /* base_data_offset, an absolute file offset */
        bb_u32(&tfhd, bdo_value);
    }

    bb_init(&tfdt);
    bb_u32(&tfdt, 0); /* version 0 + flags */
    bb_u32(&tfdt, 0); /* baseMediaDecodeTime */

    bb_init(&trun);
    bb_u32(&trun, 0x00000301u); /* data-offset + sample-duration + sample-size present */
    bb_u32(&trun, 2);           /* sample_count */
    bb_u32(&trun, (uint32_t)data_offset);
    bb_u32(&trun, 100);
    bb_u32(&trun, SYN_SAMPLE_A);
    bb_u32(&trun, 100);
    bb_u32(&trun, SYN_SAMPLE_B);

    bb_init(&traf);
    bb_box(&traf, "tfhd", &tfhd);
    bb_free(&tfhd);
    bb_box(&traf, "tfdt", &tfdt);
    bb_free(&tfdt);
    bb_box(&traf, "trun", &trun);
    bb_free(&trun);

    bb_init(&moof);
    bb_box(&moof, "traf", &traf);
    bb_free(&traf);

    bb_init(out);
    bb_box(out, "moof", &moof);
    bb_free(&moof);
}

/* moof + mdat, optionally behind a leading styp. data_offset is measured from
 * the moof's own first byte, per the spec, so it does not change when the
 * styp is added -- which is the whole point of the check. */
static void build_syn_fragment(byte_buf *out, int with_styp, int bdo_present, uint32_t bdo_value) {
    byte_buf moof, mdat, payload, styp;
    size_t   moof_size;
    unsigned i;

    /* Built once with a placeholder to measure it, then again for real: the
     * data_offset field is fixed-width, so the size does not move. */
    build_syn_moof(&moof, 0, bdo_present, bdo_value);
    moof_size = moof.len;
    bb_free(&moof);
    build_syn_moof(&moof, (int32_t)(moof_size + 8), bdo_present, bdo_value);

    bb_init(&payload);
    for (i = 0; i < SYN_PAYLOAD_LEN; i++) bb_u8(&payload, (uint8_t)(0xA0 + i));

    bb_init(&mdat);
    bb_box(&mdat, "mdat", &payload);
    bb_free(&payload);

    bb_init(out);
    if (with_styp) {
        bb_init(&styp);
        bb_box(out, "styp", &styp); /* an empty box this reader does not interpret */
        bb_free(&styp);
    }
    bb_append(out, moof.data, moof.len);
    bb_append(out, mdat.data, mdat.len);
    bb_free(&moof);
    bb_free(&mdat);
}

static int t_leading_box_does_not_shift_samples(char *note, unsigned n) {
    byte_buf    plain, with_styp;
    fmp4        m;
    fmp4_err    e;
    size_t      consumed;
    capture_ctx a, b;
    unsigned    i;

    build_syn_fragment(&plain, 0, 0, 0);
    build_syn_fragment(&with_styp, 1, 0, 0);

    memset(&m, 0, sizeof m);
    memset(&a, 0, sizeof a);
    consumed = 0;
    e        = fmp4_fragment(&m, plain.data, plain.len, on_sample_capture, &a, &consumed);
    if (e != FMP4_OK || a.count != 2 || a.len != SYN_SAMPLE_A + SYN_SAMPLE_B) {
        snprintf(note, n, "the plain fragment did not walk: err %d (%s), %u samples, %zu bytes", (int)e, m.err, a.count,
                 a.len);
        bb_free(&plain);
        bb_free(&with_styp);
        return 1;
    }

    memset(&m, 0, sizeof m);
    memset(&b, 0, sizeof b);
    consumed = 0;
    e        = fmp4_fragment(&m, with_styp.data, with_styp.len, on_sample_capture, &b, &consumed);
    bb_free(&plain);
    bb_free(&with_styp);

    if (e != FMP4_OK) {
        snprintf(note, n, "the fragment behind a styp did not walk: %s", m.err);
        return 1;
    }
    if (b.count != a.count || b.len != a.len) {
        snprintf(note, n, "styp changed the sample count/size: %u/%zu vs %u/%zu", b.count, b.len, a.count, a.len);
        return 1;
    }
    for (i = 0; i < a.len; i++) {
        if (a.bytes[i] != b.bytes[i]) {
            snprintf(note, n, "byte %u differs: 0x%02X without the styp, 0x%02X with it -- data_offset applied from "
                              "the buffer start rather than the moof",
                     i, a.bytes[i], b.bytes[i]);
            return 1;
        }
    }

    snprintf(note, n, "a leading styp left all %zu sample bytes identical", a.len);
    return 0;
}

/* ---- 11: an unresolvable tfhd base_data_offset is refused --------------- */

/* base_data_offset is an offset into the WHOLE file, while everything in the
 * sample walk addresses bytes as moof_start + data_offset. The two agree only
 * when the base is the moof's own position -- which is what ffmpeg, and so
 * Jellyfin, writes: every tfhd in fixtures/probe360.mp4 carries flags 0x39
 * with base_data_offset equal to its moof's file offset. That is why simply
 * skipping the field worked, and why refusing the flag outright is not the
 * fix: it would refuse the only files this project has.
 *
 * What was wrong is that a base naming anywhere ELSE was skipped just as
 * quietly and produced slices from the wrong place -- bounds-safe, and wrong.
 * So: the base that resolves still walks, and one that does not is refused
 * rather than assumed away. */
static int t_tfhd_base_data_offset_is_refused(char *note, unsigned n) {
    byte_buf    frag;
    fmp4        m;
    fmp4_err    e;
    size_t      consumed = 0;
    capture_ctx c;

    /* The fragment sits at stream position 0 with no leading box, so a base
     * of 0 IS the moof's position: resolvable, and it must still walk. */
    build_syn_fragment(&frag, 0, 1, 0);
    memset(&m, 0, sizeof m);
    memset(&c, 0, sizeof c);
    e = fmp4_fragment(&m, frag.data, frag.len, on_sample_capture, &c, &consumed);
    bb_free(&frag);

    if (e != FMP4_OK || c.count != 2) {
        snprintf(note, n, "a base_data_offset that IS the moof's own position was refused: %d (%s)", (int)e, m.err);
        return 1;
    }
    if (c.bytes[0] != 0xA0) {
        snprintf(note, n, "the resolvable base produced 0x%02X as the first sample byte, expected 0xA0", c.bytes[0]);
        return 1;
    }

    /* Now a base that names somewhere else entirely. Nothing in the fragment
     * can resolve it, so it must be refused, not silently read as if the moof
     * had been the base. */
    build_syn_fragment(&frag, 0, 1, 0x1000);
    memset(&m, 0, sizeof m);
    memset(&c, 0, sizeof c);
    consumed = 0;
    e        = fmp4_fragment(&m, frag.data, frag.len, on_sample_capture, &c, &consumed);
    bb_free(&frag);

    if (e == FMP4_OK) {
        snprintf(note, n, "a base_data_offset of 0x1000 was accepted, emitting %u samples from an unresolvable base",
                 c.count);
        return 1;
    }
    if (e != FMP4_ERR_UNSUPPORTED) {
        snprintf(note, n, "refused, but as %d rather than UNSUPPORTED (%s)", (int)e, m.err);
        return 1;
    }
    snprintf(note, n, "the moof's own position resolves and walks; a base of 0x1000 is refused: %s", m.err);
    return 0;
}

/* ---- 12: a non-zero on_sample stops the whole fragment ------------------ */

/* media/fmp4.h promises that on_sample returning non-zero "stops the walk
 * early". It used to unwind only as far as the enclosing trun: this fixture's
 * moof holds two trafs, one per track, so a caller that said stop on the
 * first sample still received every sample of the second track. */

typedef struct {
    unsigned count;
    unsigned stop_after;
} stop_ctx;

static int on_sample_stop(const fmp4_sample *s, void *user) {
    stop_ctx *c = (stop_ctx *)user;
    (void)s;
    c->count++;
    return c->count >= c->stop_after;
}

static int t_on_sample_stop_unwinds_the_fragment(char *note, unsigned n) {
    uint8_t *file;
    size_t   len, consumed = 0;
    fmp4     m;
    fmp4_err e;
    stop_ctx ctx;

    file = load_fixture(&len);
    if (!file) {
        snprintf(note, n, "fixtures/probe360.mp4 not found -- skipping");
        return -1;
    }

    e = fmp4_init(&m, file, len);
    if (e != FMP4_OK) {
        snprintf(note, n, "fmp4_init failed: %s", m.err);
        free(file);
        return 1;
    }

    ctx.count      = 0;
    ctx.stop_after = 1;

    e = fmp4_fragment(&m, file + fmp4_init_size(&m), len - fmp4_init_size(&m), on_sample_stop, &ctx, &consumed);
    free(file);

    if (e != FMP4_OK) {
        snprintf(note, n, "stopping early was reported as an error: %s", m.err);
        return 1;
    }
    if (ctx.count != 1) {
        snprintf(note, n, "stopped after the first sample, but %u were delivered -- the walk carried on into the next "
                          "traf",
                 ctx.count);
        return 1;
    }
    if (consumed == 0) {
        snprintf(note, n, "consumed was 0 after an early stop");
        return 1;
    }
    snprintf(note, n, "one sample delivered out of the fragment's 332, %zu bytes still consumed", consumed);
    return 0;
}

void test_fmp4_register(void) {
    test_add("fmp4", "a box smaller than its own header is refused", t_box_smaller_than_header);
    test_add("fmp4", "a box longer than the buffer is truncation, not a read past the end", t_box_longer_than_buffer);
    test_add("fmp4", "an SPS longer than the buffer is refused", t_sps_longer_than_buffer);
    test_add("fmp4", "the fixture's parameter sets are found", t_fixture_parameter_sets);
    test_add("fmp4", "the fixture's audio config is found", t_fixture_audio_config);
    test_add("fmp4", "the first fragment yields samples in order", t_first_fragment_in_order);
    test_add("fmp4", "walking the whole file consumes it exactly", t_whole_file_consumed_exactly);
    test_add("fmp4", "mutated headers are refused, never crashed on", t_mutated_headers_never_crash);
    test_add("fmp4", "mutated fragments are refused, never spun on", t_mutated_fragments_never_spun_on);
    test_add("fmp4", "a largesize that wraps the bounds check is refused", t_a_largesize_that_wraps_is_refused);
    test_add("fmp4", "a wrapping largesize in a fragment is refused", t_a_wrapping_largesize_in_a_fragment_is_refused);
    test_add("fmp4", "a leading box before the moof does not shift the samples",
             t_leading_box_does_not_shift_samples);
    test_add("fmp4", "an unresolvable tfhd base_data_offset is refused", t_tfhd_base_data_offset_is_refused);
    test_add("fmp4", "a non-zero on_sample stops the whole fragment", t_on_sample_stop_unwinds_the_fragment);
}
