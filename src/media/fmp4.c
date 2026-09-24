/* See media/fmp4.h.
 *
 * Every box is walked the same way: read a 32-bit size and a 4-byte type,
 * check the size against what is actually left in the buffer, then hand the
 * box's *content* down as a fresh (pointer, length) pair. A child never sees
 * more bytes than its parent already proved exist, so nothing below the
 * top-level walk needs to re-check against the original buffer -- it only
 * ever looks at the slice it was given.
 *
 * Offsets used below (avcC's field layout, avc1's width/height at 24/26, the
 * esds descriptor chain, mp4a's channel/rate fields) were checked against
 * fixtures/probe360.mp4 byte-for-byte while this file was written, not
 * quoted from memory of the spec -- see docs/FIXTURES.md. */

#include "media/fmp4.h"

#include <string.h>

#define FOURCC(a, b, c, d) \
    (((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(uint8_t)(b) << 16) | ((uint32_t)(uint8_t)(c) << 8) | (uint32_t)(uint8_t)(d))

enum {
    T_FTYP = FOURCC('f', 't', 'y', 'p'),
    T_MOOV = FOURCC('m', 'o', 'o', 'v'),
    T_TRAK = FOURCC('t', 'r', 'a', 'k'),
    T_TKHD = FOURCC('t', 'k', 'h', 'd'),
    T_MDIA = FOURCC('m', 'd', 'i', 'a'),
    T_MDHD = FOURCC('m', 'd', 'h', 'd'),
    T_MINF = FOURCC('m', 'i', 'n', 'f'),
    T_STBL = FOURCC('s', 't', 'b', 'l'),
    T_STSD = FOURCC('s', 't', 's', 'd'),
    T_AVC1 = FOURCC('a', 'v', 'c', '1'),
    T_AVCC = FOURCC('a', 'v', 'c', 'C'),
    T_MP4A = FOURCC('m', 'p', '4', 'a'),
    T_ESDS = FOURCC('e', 's', 'd', 's'),
    T_MOOF = FOURCC('m', 'o', 'o', 'f'),
    T_TRAF = FOURCC('t', 'r', 'a', 'f'),
    T_TFHD = FOURCC('t', 'f', 'h', 'd'),
    T_TFDT = FOURCC('t', 'f', 'd', 't'),
    T_TRUN = FOURCC('t', 'r', 'u', 'n'),
    T_MDAT = FOURCC('m', 'd', 'a', 't')
};

/* trun's sample_flags: clear means sync (a keyframe). Verified against the
 * fixture's first_sample_flags (0x02000000, sync) versus tfhd's
 * default_sample_flags (0x01010000, not sync). */
#define SAMPLE_IS_NON_SYNC 0x00010000u

typedef struct {
    uint32_t       type;
    const uint8_t *content;
    uint64_t       content_size;
    uint64_t       total_size; /* header + content: how far to advance past this box */
} box_t;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(((uint32_t)p[0] << 8) | p[1]); }

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t rd64(const uint8_t *p) { return ((uint64_t)rd32(p) << 32) | rd32(p + 4); }

static void set_err(fmp4 *m, const char *msg) {
    size_t i = 0;
    while (msg[i] && i + 1 < FMP4_ERR_MAX) {
        m->err[i] = msg[i];
        i++;
    }
    m->err[i] = 0;
}

/* Reads one box at `buf[off]`. Never advances less than the header it read,
 * so a caller looping on the returned total_size cannot spin in place --
 * every error return happens before any advance is even suggested. */
static fmp4_err next_box(fmp4 *m, const uint8_t *buf, size_t len, size_t off, box_t *box) {
    uint64_t size;
    size_t   hdr = 8;

    /* Enforced rather than assumed: every bounds test below subtracts from
     * `len`, which is only meaningful while off <= len. */
    if (off > len) {
        set_err(m, "box offset is past the end of the buffer");
        return FMP4_ERR_TRUNCATED;
    }
    if (len - off < 8) {
        set_err(m, "box header runs past the end of the buffer");
        return FMP4_ERR_TRUNCATED;
    }

    size = rd32(buf + off);

    if (size == 1) {
        if (len - off < 16) {
            set_err(m, "largesize header runs past the end of the buffer");
            return FMP4_ERR_TRUNCATED;
        }
        hdr  = 16;
        size = rd64(buf + off + 8);
    } else if (size == 0) {
        size = len - off;
    }

    if (size < hdr) {
        set_err(m, "a box is smaller than its own header");
        return FMP4_ERR_MALFORMED;
    }
    /* Subtracting, never adding. `size` is a 64-bit field straight off the
     * network: written as `off + size > len` the sum is computed modulo 2^64,
     * so a largesize near 2^64 wraps to a small number, passes the check, and
     * hands a near-2^64 content_size to a child parser that then walks off the
     * end of the allocation. Twenty-eight bytes of input were enough to reach
     * it. `off <= len` is guaranteed by the header check above and by off only
     * ever advancing by an already-validated total_size. */
    if (size > (uint64_t)(len - off)) {
        set_err(m, "a box claims more bytes than the buffer holds");
        return FMP4_ERR_TRUNCATED;
    }

    box->type         = rd32(buf + off + 4);
    box->content      = buf + off + hdr;
    box->content_size = size - hdr;
    box->total_size   = size;
    return FMP4_OK;
}

/* ---- moov: parameter sets and timing ----------------------------------- */

typedef struct {
    uint32_t track_id;
    int      have_track_id;
    uint32_t timescale;
    int      have_timescale;
    int      kind; /* 0 unknown, 1 video, 2 audio */
    fmp4_video video;
    fmp4_audio audio;
} track_info;

enum { KIND_UNKNOWN = 0, KIND_VIDEO = 1, KIND_AUDIO = 2 };

static fmp4_err parse_tkhd(fmp4 *m, const uint8_t *buf, size_t len, track_info *ti) {
    uint8_t version;

    if (len < 4) {
        set_err(m, "tkhd shorter than its version/flags");
        return FMP4_ERR_TRUNCATED;
    }
    version = buf[0];

    if (version == 1) {
        if (len < 24) {
            set_err(m, "tkhd (v1) too short for track_id");
            return FMP4_ERR_TRUNCATED;
        }
        ti->track_id = rd32(buf + 20);
    } else {
        if (len < 16) {
            set_err(m, "tkhd (v0) too short for track_id");
            return FMP4_ERR_TRUNCATED;
        }
        ti->track_id = rd32(buf + 12);
    }
    ti->have_track_id = 1;
    return FMP4_OK;
}

static fmp4_err parse_mdhd(fmp4 *m, const uint8_t *buf, size_t len, track_info *ti) {
    uint8_t version;

    if (len < 4) {
        set_err(m, "mdhd shorter than its version/flags");
        return FMP4_ERR_TRUNCATED;
    }
    version = buf[0];

    if (version == 1) {
        if (len < 28) {
            set_err(m, "mdhd (v1) too short for timescale");
            return FMP4_ERR_TRUNCATED;
        }
        ti->timescale = rd32(buf + 20);
    } else {
        if (len < 16) {
            set_err(m, "mdhd (v0) too short for timescale");
            return FMP4_ERR_TRUNCATED;
        }
        ti->timescale = rd32(buf + 12);
    }
    ti->have_timescale = 1;
    return FMP4_OK;
}

/* avcC: configurationVersion, profile, compat, level, then a byte whose low
 * two bits plus one give nal_length_size, then a byte whose low 5 bits count
 * SPS entries (we keep the first), then a PPS count the same way. */
static fmp4_err parse_avcc(fmp4 *m, const uint8_t *buf, size_t len, track_info *ti) {
    size_t   off;
    uint8_t  num_sps, num_pps;
    uint16_t sps_len, pps_len;
    unsigned i;

    if (len < 6) {
        set_err(m, "avcC shorter than its fixed header");
        return FMP4_ERR_TRUNCATED;
    }

    ti->video.nal_length_size = (uint8_t)((buf[4] & 0x3) + 1);
    /* lengthSizeMinusOne == 2, i.e. a three-byte length prefix, is the one
     * value those two bits can hold that ISO/IEC 14496-15 does not allow:
     * only 1, 2 and 4 are legal widths. Refused where it is read, rather
     * than carried down to media/h264_au.c to be refused there -- this is
     * the box that states it. */
    if (ti->video.nal_length_size == 3) {
        set_err(m, "avcC declares a 3-byte NAL length prefix, which is not a width the format allows");
        return FMP4_ERR_MALFORMED;
    }
    num_sps                   = (uint8_t)(buf[5] & 0x1F);
    off                       = 6;

    if (num_sps == 0) {
        set_err(m, "avcC has no SPS");
        return FMP4_ERR_MALFORMED;
    }
    for (i = 0; i < num_sps; i++) {
        if (off + 2 > len) {
            set_err(m, "avcC SPS length runs past the box");
            return FMP4_ERR_TRUNCATED;
        }
        sps_len = rd16(buf + off);
        off += 2;
        if (i == 0) {
            if (sps_len > FMP4_MAX_PARAM_SET) {
                set_err(m, "SPS longer than this reader's fixed capacity");
                return FMP4_ERR_TOOBIG;
            }
            if (off + sps_len > len) {
                set_err(m, "SPS length runs past the avcC box");
                return FMP4_ERR_TRUNCATED;
            }
            memcpy(ti->video.sps, buf + off, sps_len);
            ti->video.sps_len = sps_len;
        } else {
            if (off + sps_len > len) {
                set_err(m, "SPS length runs past the avcC box");
                return FMP4_ERR_TRUNCATED;
            }
        }
        off += sps_len;
    }

    if (off >= len) {
        set_err(m, "avcC has no room for a PPS count");
        return FMP4_ERR_TRUNCATED;
    }
    num_pps = buf[off];
    off += 1;

    if (num_pps == 0) {
        set_err(m, "avcC has no PPS");
        return FMP4_ERR_MALFORMED;
    }
    for (i = 0; i < num_pps; i++) {
        if (off + 2 > len) {
            set_err(m, "avcC PPS length runs past the box");
            return FMP4_ERR_TRUNCATED;
        }
        pps_len = rd16(buf + off);
        off += 2;
        if (i == 0) {
            if (pps_len > FMP4_MAX_PARAM_SET) {
                set_err(m, "PPS longer than this reader's fixed capacity");
                return FMP4_ERR_TOOBIG;
            }
            if (off + pps_len > len) {
                set_err(m, "PPS length runs past the avcC box");
                return FMP4_ERR_TRUNCATED;
            }
            memcpy(ti->video.pps, buf + off, pps_len);
            ti->video.pps_len = pps_len;
        } else {
            if (off + pps_len > len) {
                set_err(m, "PPS length runs past the avcC box");
                return FMP4_ERR_TRUNCATED;
            }
        }
        off += pps_len;
    }

    return FMP4_OK;
}

/* avc1 sample entry: 6 reserved bytes, 2-byte data_reference_index, 16 more
 * fixed bytes, then width/height as 16-bit fields at offset 24/26, then more
 * fixed fields out to offset 78, where the avcC (and pasp, btrt, ...) boxes
 * begin. */
static fmp4_err parse_avc1(fmp4 *m, const uint8_t *buf, size_t len, track_info *ti) {
    size_t   off;
    box_t    box;
    fmp4_err e;
    int      found_avcc = 0;

    if (len < 28) {
        set_err(m, "avc1 entry too short for width/height");
        return FMP4_ERR_TRUNCATED;
    }
    ti->video.width  = rd16(buf + 24);
    ti->video.height = rd16(buf + 26);

    if (len < 78) {
        set_err(m, "avc1 entry too short to reach its child boxes");
        return FMP4_ERR_TRUNCATED;
    }

    off = 78;
    while (off < len) {
        e = next_box(m, buf, len, off, &box);
        if (e) return e;
        if (box.type == T_AVCC) {
            e = parse_avcc(m, box.content, (size_t)box.content_size, ti);
            if (e) return e;
            found_avcc = 1;
        }
        off += (size_t)box.total_size;
    }

    if (!found_avcc) {
        set_err(m, "avc1 without an avcC box");
        return FMP4_ERR_UNSUPPORTED;
    }
    ti->kind = KIND_VIDEO;
    return FMP4_OK;
}

/* A descriptor's length uses the MPEG-4 7-bit continuation encoding: the top
 * bit of each byte says whether another length byte follows. Capped at four
 * bytes, comfortably more than esds ever needs here. */
static fmp4_err read_desc_len(fmp4 *m, const uint8_t *buf, size_t len, size_t *off, uint32_t *out_len) {
    uint32_t val = 0;
    int      i;

    for (i = 0; i < 4; i++) {
        uint8_t b;
        if (*off >= len) {
            set_err(m, "descriptor length runs past esds");
            return FMP4_ERR_TRUNCATED;
        }
        b = buf[(*off)++];
        val = (val << 7) | (uint32_t)(b & 0x7f);
        if (!(b & 0x80)) {
            *out_len = val;
            return FMP4_OK;
        }
    }
    set_err(m, "descriptor length continuation too long");
    return FMP4_ERR_MALFORMED;
}

#define DESC_ES              0x03
#define DESC_DECODER_CONFIG  0x04
#define DESC_DECODER_SPECIFIC 0x05

/* esds: an ES_Descriptor holding a DecoderConfigDescriptor holding the
 * DecoderSpecificInfo whose payload is the AudioSpecificConfig. */
static fmp4_err parse_esds(fmp4 *m, const uint8_t *buf, size_t len, track_info *ti) {
    size_t   off;
    uint8_t  tag;
    uint32_t declen;
    fmp4_err e;
    size_t   es_end;
    size_t   dcd_end;
    uint8_t  es_flags;

    if (len < 4) {
        set_err(m, "esds shorter than its version/flags");
        return FMP4_ERR_TRUNCATED;
    }
    off = 4; /* version + flags */

    if (off >= len) {
        set_err(m, "esds has no ES_Descriptor");
        return FMP4_ERR_TRUNCATED;
    }
    tag = buf[off++];
    if (tag != DESC_ES) {
        set_err(m, "esds does not start with an ES_Descriptor");
        return FMP4_ERR_UNSUPPORTED;
    }
    e = read_desc_len(m, buf, len, &off, &declen);
    if (e) return e;
    if (off + declen > len) {
        set_err(m, "ES_Descriptor length runs past esds");
        return FMP4_ERR_TRUNCATED;
    }
    es_end = off + declen;

    if (off + 3 > es_end) {
        set_err(m, "ES_Descriptor too short for ES_ID/flags");
        return FMP4_ERR_TRUNCATED;
    }
    off += 2; /* ES_ID */
    es_flags = buf[off++];
    if (es_flags & 0x80) { /* streamDependenceFlag */
        if (off + 2 > es_end) {
            set_err(m, "ES_Descriptor dependsOn_ES_ID runs past its box");
            return FMP4_ERR_TRUNCATED;
        }
        off += 2;
    }
    if (es_flags & 0x40) { /* URL_Flag */
        uint8_t urllen;
        if (off + 1 > es_end) {
            set_err(m, "ES_Descriptor URL length runs past its box");
            return FMP4_ERR_TRUNCATED;
        }
        urllen = buf[off++];
        if (off + urllen > es_end) {
            set_err(m, "ES_Descriptor URL runs past its box");
            return FMP4_ERR_TRUNCATED;
        }
        off += urllen;
    }
    if (es_flags & 0x20) { /* OCRstreamFlag */
        if (off + 2 > es_end) {
            set_err(m, "ES_Descriptor OCR_ES_Id runs past its box");
            return FMP4_ERR_TRUNCATED;
        }
        off += 2;
    }

    if (off >= es_end) {
        set_err(m, "ES_Descriptor has no DecoderConfigDescriptor");
        return FMP4_ERR_MALFORMED;
    }
    tag = buf[off++];
    if (tag != DESC_DECODER_CONFIG) {
        set_err(m, "ES_Descriptor's first child is not a DecoderConfigDescriptor");
        return FMP4_ERR_UNSUPPORTED;
    }
    e = read_desc_len(m, buf, len, &off, &declen);
    if (e) return e;
    if (off + declen > es_end) {
        set_err(m, "DecoderConfigDescriptor length runs past its parent");
        return FMP4_ERR_TRUNCATED;
    }
    dcd_end = off + declen;

    /* objectTypeIndication(1) + streamType/upstream/reserved(1) +
     * bufferSizeDB(3) + maxBitrate(4) + avgBitrate(4) */
    if (off + 13 > dcd_end) {
        set_err(m, "DecoderConfigDescriptor too short for its fixed fields");
        return FMP4_ERR_TRUNCATED;
    }
    off += 13;

    if (off >= dcd_end) {
        set_err(m, "DecoderConfigDescriptor has no DecoderSpecificInfo");
        return FMP4_ERR_MALFORMED;
    }
    tag = buf[off++];
    if (tag != DESC_DECODER_SPECIFIC) {
        set_err(m, "DecoderConfigDescriptor's child is not a DecoderSpecificInfo");
        return FMP4_ERR_UNSUPPORTED;
    }
    e = read_desc_len(m, buf, len, &off, &declen);
    if (e) return e;
    if (declen > FMP4_MAX_PARAM_SET) {
        set_err(m, "AudioSpecificConfig longer than this reader's fixed capacity");
        return FMP4_ERR_TOOBIG;
    }
    if (off + declen > dcd_end || off + declen > len) {
        set_err(m, "DecoderSpecificInfo payload runs past esds");
        return FMP4_ERR_TRUNCATED;
    }
    memcpy(ti->audio.asc, buf + off, declen);
    ti->audio.asc_len = (uint16_t)declen;
    return FMP4_OK;
}

/* mp4a sample entry: 6 reserved + 2-byte data_reference_index, 8 more
 * reserved bytes, channelcount at 16, samplerate (16.16 fixed point, high
 * half only) at 24, then child boxes (esds) from 28. */
static fmp4_err parse_mp4a(fmp4 *m, const uint8_t *buf, size_t len, track_info *ti) {
    size_t   off;
    box_t    box;
    fmp4_err e;
    int      found_esds = 0;

    if (len < 28) {
        set_err(m, "mp4a entry too short for its fixed fields");
        return FMP4_ERR_TRUNCATED;
    }
    ti->audio.channels    = (uint8_t)rd16(buf + 16);
    ti->audio.sample_rate = rd32(buf + 24) >> 16;

    off = 28;
    while (off < len) {
        e = next_box(m, buf, len, off, &box);
        if (e) return e;
        if (box.type == T_ESDS) {
            e = parse_esds(m, box.content, (size_t)box.content_size, ti);
            if (e) return e;
            found_esds = 1;
        }
        off += (size_t)box.total_size;
    }

    if (!found_esds) {
        set_err(m, "mp4a without an esds box");
        return FMP4_ERR_UNSUPPORTED;
    }
    ti->kind = KIND_AUDIO;
    return FMP4_OK;
}

static fmp4_err parse_stsd(fmp4 *m, const uint8_t *buf, size_t len, track_info *ti) {
    box_t    box;
    fmp4_err e;

    if (len < 8) {
        set_err(m, "stsd shorter than its version/flags/entry_count");
        return FMP4_ERR_TRUNCATED;
    }
    /* Only the first sample entry is used -- this reader does not handle
     * multiple sample descriptions per track. */
    e = next_box(m, buf, len, 8, &box);
    if (e) return e;

    if (box.type == T_AVC1) return parse_avc1(m, box.content, (size_t)box.content_size, ti);
    if (box.type == T_MP4A) return parse_mp4a(m, box.content, (size_t)box.content_size, ti);

    set_err(m, "stsd's sample entry is neither avc1 nor mp4a");
    return FMP4_ERR_UNSUPPORTED;
}

static fmp4_err find_and_parse(fmp4 *m, const uint8_t *buf, size_t len, uint32_t want,
                                fmp4_err (*fn)(fmp4 *, const uint8_t *, size_t, track_info *), track_info *ti,
                                int *found) {
    size_t   off = 0;
    box_t    box;
    fmp4_err e;

    *found = 0;
    while (off < len) {
        e = next_box(m, buf, len, off, &box);
        if (e) return e;
        if (box.type == want) {
            e = fn(m, box.content, (size_t)box.content_size, ti);
            if (e) return e;
            *found = 1;
        }
        off += (size_t)box.total_size;
    }
    return FMP4_OK;
}

static fmp4_err parse_stbl(fmp4 *m, const uint8_t *buf, size_t len, track_info *ti) {
    int      found;
    return find_and_parse(m, buf, len, T_STSD, parse_stsd, ti, &found);
    /* An stbl without an stsd falls through with ti->kind left unknown,
     * which parse_trak treats as "not a track this reader understands". */
}

static fmp4_err parse_minf(fmp4 *m, const uint8_t *buf, size_t len, track_info *ti) {
    int found;
    return find_and_parse(m, buf, len, T_STBL, parse_stbl, ti, &found);
}

static fmp4_err parse_mdia(fmp4 *m, const uint8_t *buf, size_t len, track_info *ti) {
    size_t   off = 0;
    box_t    box;
    fmp4_err e;

    while (off < len) {
        e = next_box(m, buf, len, off, &box);
        if (e) return e;
        if (box.type == T_MDHD) {
            e = parse_mdhd(m, box.content, (size_t)box.content_size, ti);
            if (e) return e;
        } else if (box.type == T_MINF) {
            e = parse_minf(m, box.content, (size_t)box.content_size, ti);
            if (e) return e;
        }
        off += (size_t)box.total_size;
    }
    return FMP4_OK;
}

static fmp4_err parse_trak(fmp4 *m, const uint8_t *buf, size_t len) {
    size_t     off = 0;
    box_t      box;
    fmp4_err   e;
    track_info ti;

    memset(&ti, 0, sizeof ti);

    while (off < len) {
        e = next_box(m, buf, len, off, &box);
        if (e) return e;
        if (box.type == T_TKHD) {
            e = parse_tkhd(m, box.content, (size_t)box.content_size, &ti);
            if (e) return e;
        } else if (box.type == T_MDIA) {
            e = parse_mdia(m, box.content, (size_t)box.content_size, &ti);
            if (e) return e;
        }
        off += (size_t)box.total_size;
    }

    /* A trak this reader does not recognise (no avc1/mp4a sample entry) is
     * simply not one of the two tracks it cares about -- not an error. */
    if (ti.kind == KIND_UNKNOWN) return FMP4_OK;

    if (!ti.have_track_id) {
        set_err(m, "trak has a recognised sample entry but no tkhd");
        return FMP4_ERR_MALFORMED;
    }
    if (!ti.have_timescale) {
        set_err(m, "trak has a recognised sample entry but no mdhd");
        return FMP4_ERR_MALFORMED;
    }

    if (ti.kind == KIND_VIDEO) {
        m->video             = ti.video;
        m->video.track_id    = ti.track_id;
        m->video.timescale   = ti.timescale;
        m->have_video        = 1;
    } else {
        m->audio             = ti.audio;
        m->audio.track_id    = ti.track_id;
        m->audio.timescale   = ti.timescale;
        m->have_audio        = 1;
    }
    return FMP4_OK;
}

static fmp4_err parse_moov(fmp4 *m, const uint8_t *buf, size_t len) {
    size_t   off = 0;
    box_t    box;
    fmp4_err e;

    while (off < len) {
        e = next_box(m, buf, len, off, &box);
        if (e) return e;
        if (box.type == T_TRAK) {
            e = parse_trak(m, box.content, (size_t)box.content_size);
            if (e) return e;
        }
        off += (size_t)box.total_size;
    }
    return FMP4_OK;
}

fmp4_err fmp4_init(fmp4 *m, const uint8_t *data, size_t len) {
    size_t   off = 0;
    box_t    box;
    fmp4_err e;
    int      found_moov = 0;

    memset(m, 0, sizeof *m);

    while (off < len) {
        e = next_box(m, data, len, off, &box);
        if (e) return e;
        off += (size_t)box.total_size;
        if (box.type == T_MOOV) {
            e = parse_moov(m, box.content, (size_t)box.content_size);
            if (e) return e;
            found_moov = 1;
            break;
        }
    }

    if (!found_moov) {
        set_err(m, "no moov box found");
        return FMP4_ERR_MALFORMED;
    }
    if (!m->have_video && !m->have_audio) {
        set_err(m, "moov has neither a recognised video nor audio track");
        return FMP4_ERR_UNSUPPORTED;
    }

    m->_init_size  = off;
    m->_stream_pos = off; /* the first fragment begins exactly here */
    return FMP4_OK;
}

size_t fmp4_init_size(const fmp4 *m) { return m->_init_size; }

/* ---- moof/mdat: samples ------------------------------------------------- */

static fmp4_err parse_tfhd(fmp4 *m, const uint8_t *buf, size_t len, uint32_t *track_id, uint32_t *default_duration,
                            int *have_dd, uint32_t *default_size, int *have_ds, uint32_t *default_flags,
                            int *have_df, uint64_t *base_data_offset, int *have_bdo) {
    uint32_t flags;
    size_t   off;

    if (len < 8) {
        set_err(m, "tfhd too short for track_id");
        return FMP4_ERR_TRUNCATED;
    }
    flags      = rd32(buf) & 0x00FFFFFFu;
    *track_id  = rd32(buf + 4);
    off        = 8;

    if (flags & 0x000001) { /* base-data-offset-present */
        /* Read and carried out, not skipped. This field is an offset into
         * the whole file, while everything below addresses samples as
         * moof_start + data_offset -- the two agree only when the base IS
         * the moof's own position. ffmpeg, and so Jellyfin, writes exactly
         * that (checked against fixtures/probe360.mp4: every tfhd in it
         * carries flags 0x39 with base_data_offset equal to its moof's file
         * offset), which is why skipping these eight bytes worked. It worked
         * by coincidence: any other base produced slices from the wrong
         * place, bounds-safe and silently wrong. parse_trun_and_emit checks
         * it against the moof's real position and refuses what it cannot
         * resolve. */
        if (off + 8 > len) {
            set_err(m, "tfhd base_data_offset runs past the box");
            return FMP4_ERR_TRUNCATED;
        }
        *base_data_offset = rd64(buf + off);
        *have_bdo         = 1;
        off += 8;
    }
    if (flags & 0x000002) { /* sample-description-index-present */
        if (off + 4 > len) {
            set_err(m, "tfhd sample_description_index runs past the box");
            return FMP4_ERR_TRUNCATED;
        }
        off += 4;
    }
    if (flags & 0x000008) {
        if (off + 4 > len) {
            set_err(m, "tfhd default_sample_duration runs past the box");
            return FMP4_ERR_TRUNCATED;
        }
        *default_duration = rd32(buf + off);
        *have_dd          = 1;
        off += 4;
    }
    if (flags & 0x000010) {
        if (off + 4 > len) {
            set_err(m, "tfhd default_sample_size runs past the box");
            return FMP4_ERR_TRUNCATED;
        }
        *default_size = rd32(buf + off);
        *have_ds      = 1;
        off += 4;
    }
    if (flags & 0x000020) {
        if (off + 4 > len) {
            set_err(m, "tfhd default_sample_flags runs past the box");
            return FMP4_ERR_TRUNCATED;
        }
        *default_flags = rd32(buf + off);
        *have_df       = 1;
        off += 4;
    }
    return FMP4_OK;
}

static fmp4_err parse_tfdt(fmp4 *m, const uint8_t *buf, size_t len, uint64_t *dts) {
    uint8_t version;

    if (len < 1) {
        set_err(m, "tfdt has no version byte");
        return FMP4_ERR_TRUNCATED;
    }
    version = buf[0];
    if (version == 1) {
        if (len < 12) {
            set_err(m, "tfdt (v1) too short for baseMediaDecodeTime");
            return FMP4_ERR_TRUNCATED;
        }
        *dts = rd64(buf + 4);
    } else {
        if (len < 8) {
            set_err(m, "tfdt (v0) too short for baseMediaDecodeTime");
            return FMP4_ERR_TRUNCATED;
        }
        *dts = rd32(buf + 4);
    }
    return FMP4_OK;
}

/* trun: a sample count, an optional data offset (relative to the moof this
 * fragment began with -- see docs/RESEARCH.md section 5), an optional
 * first-sample override, then per-sample fields chosen by the flags word. */
static fmp4_err parse_trun_and_emit(fmp4 *m, const uint8_t *buf, size_t len, const uint8_t *frag_data,
                                     size_t frag_len, uint32_t track_id, uint32_t default_duration, int have_dd,
                                     uint32_t default_size, int have_ds, uint32_t default_flags, int have_df,
                                     uint64_t base_data_offset, int have_bdo, uint64_t moof_pos, uint64_t *dts,
                                     fmp4_sample_fn on_sample, void *user, int *stop) {
    uint32_t flags;
    uint32_t sample_count;
    size_t   off;
    int32_t  data_offset  = 0;
    int      have_data_offset = 0;
    uint32_t first_sample_flags = 0;
    int      have_fsf     = 0;
    size_t   cursor;
    uint32_t i;

    if (len < 8) {
        set_err(m, "trun too short for its sample_count");
        return FMP4_ERR_TRUNCATED;
    }
    flags        = rd32(buf) & 0x00FFFFFFu;
    sample_count = rd32(buf + 4);
    off          = 8;

    if (flags & 0x000001) {
        if (off + 4 > len) {
            set_err(m, "trun data_offset runs past the box");
            return FMP4_ERR_TRUNCATED;
        }
        data_offset      = (int32_t)rd32(buf + off);
        have_data_offset = 1;
        off += 4;
    }
    if (flags & 0x000004) {
        if (off + 4 > len) {
            set_err(m, "trun first_sample_flags runs past the box");
            return FMP4_ERR_TRUNCATED;
        }
        first_sample_flags = rd32(buf + off);
        have_fsf            = 1;
        off += 4;
    }

    if (!have_data_offset || data_offset < 0) {
        set_err(m, "trun without a usable data_offset is not a shape this reader handles");
        return FMP4_ERR_UNSUPPORTED;
    }
    /* The base this reader can address from is the moof's own first byte. A
     * tfhd that names a different one is a valid file this reader cannot
     * resolve -- it would have to hold the whole stream to do so -- and
     * saying that is the only honest answer. Skipping the field and assuming
     * the moof was the base is what made a mismatch silently wrong. */
    if (have_bdo && base_data_offset != moof_pos) {
        set_err(m, "tfhd base_data_offset is not the moof's own position, which this reader cannot address from");
        return FMP4_ERR_UNSUPPORTED;
    }
    if ((size_t)data_offset > frag_len) {
        set_err(m, "trun data_offset runs past the fragment buffer");
        return FMP4_ERR_TRUNCATED;
    }
    cursor = (size_t)data_offset;

    /* sample_count comes off the network and reaches 2^32-1. Bound it against
     * bytes that actually exist BEFORE looping, because the loop body is not
     * guaranteed to advance anything: with no per-sample flags set and a
     * default_sample_size of zero, neither `off` nor `cursor` moves and no
     * bounds test fires, so a sixteen-byte trun spins four billion times --
     * minutes of frozen console per fragment, with a decoder push on each
     * turn. */
    {
        uint32_t per_sample = ((flags & 0x000100) ? 4u : 0u) + ((flags & 0x000200) ? 4u : 0u) +
                              ((flags & 0x000400) ? 4u : 0u) + ((flags & 0x000800) ? 4u : 0u);

        if (per_sample != 0) {
            if (sample_count > (uint32_t)((len - off) / per_sample)) {
                set_err(m, "trun claims more samples than its own box can describe");
                return FMP4_ERR_TRUNCATED;
            }
        } else {
            /* Every sample is a default. A zero default size means nothing in
             * the fragment constrains the count at all, so it is refused
             * rather than trusted. */
            if (!have_ds || default_size == 0) {
                set_err(m, "trun gives no per-sample sizes and no usable default");
                return FMP4_ERR_UNSUPPORTED;
            }
            if (sample_count > (uint32_t)(frag_len / default_size)) {
                set_err(m, "trun claims more samples than the fragment can hold");
                return FMP4_ERR_TRUNCATED;
            }
        }
    }

    for (i = 0; i < sample_count; i++) {
        uint32_t    s_dur   = have_dd ? default_duration : 0;
        uint32_t    s_size  = have_ds ? default_size : 0;
        uint32_t    s_flags = have_df ? default_flags : 0;
        int         has_per_sample_flags = 0;
        fmp4_sample samp;

        if (flags & 0x000100) {
            if (off + 4 > len) {
                set_err(m, "trun sample_duration runs past the box");
                return FMP4_ERR_TRUNCATED;
            }
            s_dur = rd32(buf + off);
            off += 4;
            have_dd = 1;
        }
        if (flags & 0x000200) {
            if (off + 4 > len) {
                set_err(m, "trun sample_size runs past the box");
                return FMP4_ERR_TRUNCATED;
            }
            s_size = rd32(buf + off);
            off += 4;
            have_ds = 1;
        }
        if (flags & 0x000400) {
            if (off + 4 > len) {
                set_err(m, "trun sample_flags runs past the box");
                return FMP4_ERR_TRUNCATED;
            }
            s_flags = rd32(buf + off);
            off += 4;
            has_per_sample_flags = 1;
        }
        if (flags & 0x000800) {
            if (off + 4 > len) {
                set_err(m, "trun sample_composition_time_offset runs past the box");
                return FMP4_ERR_TRUNCATED;
            }
            off += 4; /* ignored: this profile has no B-frames to reorder */
        }

        if (!have_dd) {
            set_err(m, "a sample has no duration, explicit or default");
            return FMP4_ERR_MALFORMED;
        }
        if (!have_ds) {
            set_err(m, "a sample has no size, explicit or default");
            return FMP4_ERR_MALFORMED;
        }
        if (cursor > frag_len || s_size > frag_len - cursor) {
            set_err(m, "a sample's bytes run past the buffer");
            return FMP4_ERR_TRUNCATED;
        }

        samp.data     = frag_data + cursor;
        samp.size     = s_size;
        samp.dts      = *dts;
        samp.duration = s_dur;
        samp.track_id = track_id;

        if (i == 0 && have_fsf) {
            samp.is_sync = (first_sample_flags & SAMPLE_IS_NON_SYNC) == 0;
        } else if (has_per_sample_flags) {
            samp.is_sync = (s_flags & SAMPLE_IS_NON_SYNC) == 0;
        } else if (have_df) {
            samp.is_sync = (default_flags & SAMPLE_IS_NON_SYNC) == 0;
        } else if (i == 0) {
            samp.is_sync = 1; /* frag_keyframe guarantees the first sample of a fragment is a keyframe */
        } else {
            samp.is_sync = 0;
        }

        cursor += s_size;
        *dts += s_dur;

        /* Stops the WHOLE fragment walk, not just this trun. media/fmp4.h
         * promises "stops the walk early"; unwinding only to the enclosing
         * traf meant the next trun in the same fragment kept emitting
         * samples the caller had already said it did not want. */
        if (on_sample(&samp, user)) {
            *stop = 1;
            return FMP4_OK;
        }
    }
    return FMP4_OK;
}

static fmp4_err parse_traf_and_emit(fmp4 *m, const uint8_t *buf, size_t len, const uint8_t *frag_data,
                                     size_t frag_len, uint64_t moof_pos, fmp4_sample_fn on_sample, void *user,
                                     int *stop) {
    size_t   off = 0;
    box_t    box;
    fmp4_err e;

    uint32_t track_id = 0;
    int      have_tfhd = 0;
    uint32_t default_duration = 0, default_size = 0, default_flags = 0;
    int      have_dd = 0, have_ds = 0, have_df = 0;
    uint64_t base_data_offset = 0;
    int      have_bdo = 0;
    uint64_t dts     = 0;
    int      have_dts = 0;

    while (off < len) {
        e = next_box(m, buf, len, off, &box);
        if (e) return e;

        if (box.type == T_TFHD) {
            e = parse_tfhd(m, box.content, (size_t)box.content_size, &track_id, &default_duration, &have_dd,
                            &default_size, &have_ds, &default_flags, &have_df, &base_data_offset, &have_bdo);
            if (e) return e;
            have_tfhd = 1;
        } else if (box.type == T_TFDT) {
            e = parse_tfdt(m, box.content, (size_t)box.content_size, &dts);
            if (e) return e;
            have_dts = 1;
        } else if (box.type == T_TRUN) {
            if (!have_tfhd) {
                set_err(m, "trun appears before tfhd in this traf");
                return FMP4_ERR_UNSUPPORTED;
            }
            if (!have_dts) {
                /* No tfdt: fall back to wherever this track's running clock
                 * last stopped. Every fragment observed from this project's
                 * server carries a tfdt, so this path exists for spec
                 * completeness rather than because it has been exercised. */
                if (m->have_video && track_id == m->video.track_id) dts = m->_running_dts_video;
                else if (m->have_audio && track_id == m->audio.track_id) dts = m->_running_dts_audio;
                else dts = 0;
                have_dts = 1;
            }
            e = parse_trun_and_emit(m, box.content, (size_t)box.content_size, frag_data, frag_len, track_id,
                                     default_duration, have_dd, default_size, have_ds, default_flags, have_df,
                                     base_data_offset, have_bdo, moof_pos, &dts, on_sample, user, stop);
            if (e) return e;

            if (m->have_video && track_id == m->video.track_id) m->_running_dts_video = dts;
            else if (m->have_audio && track_id == m->audio.track_id) m->_running_dts_audio = dts;

            if (*stop) return FMP4_OK;
        }
        off += (size_t)box.total_size;
    }

    if (!have_tfhd) {
        set_err(m, "traf without a tfhd");
        return FMP4_ERR_MALFORMED;
    }
    return FMP4_OK;
}

static fmp4_err parse_moof_and_emit(fmp4 *m, const uint8_t *buf, size_t len, const uint8_t *frag_data,
                                     size_t frag_len, uint64_t moof_pos, fmp4_sample_fn on_sample, void *user,
                                     int *stop) {
    size_t   off = 0;
    box_t    box;
    fmp4_err e;

    while (off < len) {
        e = next_box(m, buf, len, off, &box);
        if (e) return e;
        if (box.type == T_TRAF) {
            e = parse_traf_and_emit(m, box.content, (size_t)box.content_size, frag_data, frag_len, moof_pos, on_sample,
                                     user, stop);
            if (e) return e;
            if (*stop) return FMP4_OK;
        }
        off += (size_t)box.total_size;
    }
    return FMP4_OK;
}

fmp4_err fmp4_fragment(fmp4 *m, const uint8_t *data, size_t len, fmp4_sample_fn on_sample, void *user,
                       size_t *consumed) {
    size_t   off = 0;
    box_t    box;
    box_t    moof_box;
    size_t   moof_off = 0;
    uint64_t moof_pos;
    fmp4_err e;
    int      have_moof = 0, have_mdat = 0;
    int      stop      = 0;

    /* Defined on every return, including the error ones. A caller that logs
     * or advances by it after a failure should not be reading whatever was
     * in its local. */
    *consumed = 0;

    while (off < len) {
        e = next_box(m, data, len, off, &box);
        if (e) return e;

        if (!have_moof && box.type == T_MOOF) {
            moof_box  = box;
            moof_off  = off; /* trun's data_offset is measured from HERE, not from `data` */
            have_moof = 1;
        } else if (have_moof && !have_mdat && box.type == T_MDAT) {
            have_mdat = 1;
            off += (size_t)box.total_size;
            break;
        }
        off += (size_t)box.total_size;
    }

    if (have_moof && !have_mdat) {
        set_err(m, "moof without a following mdat");
        return FMP4_ERR_MALFORMED;
    }

    *consumed = off;
    moof_pos  = m->_stream_pos + moof_off;
    m->_stream_pos += off;

    /* Nothing but boxes this reader does not interpret (e.g. the trailing
     * mfra) -- not an error, just nothing to walk. */
    if (!have_moof) return FMP4_OK;

    /* The span starts at the moof, not at `data`. trun's data_offset is
     * defined relative to the first byte of the enclosing moof (ISO/IEC
     * 14496-12), and the two coincide only when the moof happens to sit at
     * offset 0 -- which the loop above explicitly does not require, since it
     * walks past leading boxes it does not interpret. Passing `data` shifted
     * every sample slice by the size of whatever styp, free, skip, sidx or
     * emsg came first: inside the buffer, so every bounds check still passed,
     * but the wrong bytes, and misaligned NAL length prefixes then went to
     * the Media Engine. Jellyfin puts a styp in front of each segment. */
    return parse_moof_and_emit(m, moof_box.content, (size_t)moof_box.content_size, data + moof_off, len - moof_off,
                               moof_pos, on_sample, user, &stop);
}
