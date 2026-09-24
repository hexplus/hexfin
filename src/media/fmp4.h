/* A reader for exactly the fragmented MP4 Jellyfin produces, and nothing more.
 *
 * Jellyfin's transcode arrives as ftyp, then moov carrying the sample
 * descriptions, then alternating moof/mdat fragments -- see docs/RESEARCH.md
 * section 5. That shape is far kinder than MPEG-TS: the parameter sets arrive
 * once, structured, in the avcC box, which is exactly the form the Media
 * Engine wants; and the NAL units inside mdat are already length-prefixed, so
 * there is no start-code scan and no emulation-prevention handling here.
 *
 * Deliberately not a general MP4 reader. Unknown boxes are skipped, not
 * interpreted. Everything it does understand, it validates: this input comes
 * off a network from a server we do not control (PROMPT.md section 35).
 *
 * No platform headers: the host tests this without a PSP. */
#ifndef MEDIA_FMP4_H
#define MEDIA_FMP4_H

#include <stddef.h>
#include <stdint.h>

#define FMP4_MAX_PARAM_SET 64  /* SPS and PPS are tens of bytes at this profile */
#define FMP4_ERR_MAX       96

typedef enum {
    FMP4_OK = 0,
    FMP4_ERR_TRUNCATED,   /* a box claims more bytes than exist */
    FMP4_ERR_MALFORMED,   /* a box's contents do not parse */
    FMP4_ERR_UNSUPPORTED, /* valid MP4, but not a shape we handle */
    FMP4_ERR_TOOBIG       /* a field exceeds a fixed capacity here */
} fmp4_err;

typedef struct {
    uint8_t  sps[FMP4_MAX_PARAM_SET];
    uint16_t sps_len;
    uint8_t  pps[FMP4_MAX_PARAM_SET];
    uint16_t pps_len;
    uint8_t  nal_length_size; /* from avcC; 4 for Jellyfin's output */
    uint16_t width;
    uint16_t height;
    uint32_t timescale;       /* ticks per second for this track */
    uint32_t track_id;
} fmp4_video;

typedef struct {
    uint8_t  asc[FMP4_MAX_PARAM_SET]; /* AudioSpecificConfig from esds */
    uint16_t asc_len;
    uint32_t sample_rate;
    uint8_t  channels;
    uint32_t timescale;
    uint32_t track_id;
} fmp4_audio;

typedef struct {
    fmp4_video video;
    fmp4_audio audio;
    int        have_video;
    int        have_audio;
    char       err[FMP4_ERR_MAX];

    /* internal: byte offset where fmp4_init stopped, i.e. the size of
     * ftyp+moov, so fmp4_init_size() has something to return. */
    size_t _init_size;

    /* internal: how far into the stream the next fmp4_fragment call's `data`
     * begins, kept so that a tfhd carrying base-data-offset-present -- which
     * is an offset into the whole file, not into the span handed in -- can be
     * checked rather than skipped over. It advances by each call's `consumed`
     * and is therefore only meaningful while the caller feeds spans in order
     * from where fmp4_init stopped, which is what main.c does. A caller that
     * seeks will see such a tfhd refused as unsupported rather than
     * misread. */
    uint64_t _stream_pos;

    /* internal: running decode timestamps, in each track's timescale, kept
     * across fmp4_fragment calls for the fragment that omits tfdt -- see
     * docs/RESEARCH.md section 5. Unused whenever tfdt is present, which is
     * every fragment this project's server has been observed to emit. */
    uint64_t _running_dts_video;
    uint64_t _running_dts_audio;
} fmp4;

/* One sample (one access unit for video, one frame for audio), described as a
 * slice of the caller's buffer rather than a copy. */
typedef struct {
    const uint8_t *data;
    uint32_t       size;
    uint64_t       dts;      /* in the track's timescale */
    uint32_t       duration;
    uint32_t       track_id;
    int            is_sync;  /* a keyframe, per trun's sample flags */
} fmp4_sample;

/* Parses the initialisation part: everything up to and including moov.
 * `data` must hold at least that much. Returns FMP4_OK and fills `m`, or an
 * error with a readable reason in m->err. */
fmp4_err fmp4_init(fmp4 *m, const uint8_t *data, size_t len);

/* How many bytes fmp4_init consumed, so the caller knows where the first moof
 * begins. Valid only after a successful fmp4_init. */
size_t fmp4_init_size(const fmp4 *m);

/* Walks one moof/mdat pair at `data`, calling `on_sample` for each sample in
 * declaration order. Returns the number of bytes consumed through `consumed`,
 * so the caller can advance. `on_sample` returning non-zero stops the walk
 * early and is not an error. */
typedef int (*fmp4_sample_fn)(const fmp4_sample *s, void *user);

fmp4_err fmp4_fragment(fmp4 *m, const uint8_t *data, size_t len, fmp4_sample_fn on_sample, void *user,
                       size_t *consumed);

#endif
