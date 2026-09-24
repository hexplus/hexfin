/* See media/h264_mmco.h.
 *
 * The synthetic checks build slices bit by bit, with the same writer rules a
 * real encoder follows (Exp-Golomb, rbsp trailing bits, emulation
 * prevention), and compare the rewrite against the slice an encoder would
 * have written with the flag at 0 in the first place: byte for byte. That is
 * the property that makes the rewrite safe -- the Media Engine sees exactly
 * what a sliding-window encoder would have sent.
 *
 * The fixture check runs the server's own h264_vaapi output through it and
 * skips, not fails, when fixtures/probe360.mp4 is absent. */

#include "media/fmp4.h"
#include "media/h264_mmco.h"

#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- a bit builder, escaping as it goes -------------------------------- */

typedef struct {
    uint8_t  raw[512]; /* RBSP */
    uint32_t bits;
} bits_t;

static void b_bit(bits_t *b, unsigned v) {
    if (v) b->raw[b->bits / 8] |= (uint8_t)(0x80u >> (b->bits % 8));
    b->bits++;
}

static void b_u(bits_t *b, unsigned n, uint32_t v) {
    while (n--) b_bit(b, (v >> n) & 1u);
}

static void b_ue(bits_t *b, uint32_t v) {
    uint32_t x = v + 1, n = 0, t = x;
    while (t > 1) {
        t >>= 1;
        n++;
    }
    b_u(b, n, 0);
    b_u(b, n + 1, x);
}

static void b_se(bits_t *b, int32_t v) { b_ue(b, v > 0 ? (uint32_t)(2 * v - 1) : (uint32_t)(-2 * v)); }

/* rbsp_trailing_bits, then the NAL: header byte plus the escaped RBSP. */
static uint32_t b_finish(bits_t *b, uint8_t header, uint8_t *nal) {
    uint32_t i, n = 0, zeros = 0, len;

    b_bit(b, 1);
    while (b->bits % 8) b_bit(b, 0);
    len    = b->bits / 8;
    nal[n++] = header;
    for (i = 0; i < len; i++) {
        uint8_t c = b->raw[i];
        if (zeros >= 2 && c <= 3) {
            nal[n++] = 3;
            zeros    = 0;
        }
        nal[n++] = c;
        zeros    = (c == 0) ? zeros + 1 : 0;
    }
    return n;
}

/* Constrained Baseline, poc type 2, max_num_ref_frames as given. */
static uint32_t make_sps(uint8_t *nal, uint32_t max_refs) {
    bits_t b;
    memset(&b, 0, sizeof b);
    b_u(&b, 8, 66);
    b_u(&b, 8, 0x40);
    b_u(&b, 8, 21);
    b_ue(&b, 0);         /* sps id */
    b_ue(&b, 4);         /* log2_max_frame_num_minus4 -> 8 bits */
    b_ue(&b, 2);         /* poc type */
    b_ue(&b, max_refs);
    b_u(&b, 1, 0);       /* gaps */
    b_ue(&b, 22);        /* width in mbs - 1 */
    b_ue(&b, 12);        /* height in map units - 1 */
    b_u(&b, 1, 1);       /* frame_mbs_only */
    b_u(&b, 1, 1);       /* direct_8x8_inference */
    b_u(&b, 1, 0);       /* cropping */
    b_u(&b, 1, 0);       /* vui */
    return b_finish(&b, 0x67, nal);
}

static uint32_t make_pps(uint8_t *nal) {
    bits_t b;
    memset(&b, 0, sizeof b);
    b_ue(&b, 0);
    b_ue(&b, 0);
    b_u(&b, 1, 0); /* CAVLC */
    b_u(&b, 1, 0); /* bottom_field_pic_order_in_frame_present */
    b_ue(&b, 0);   /* slice groups */
    b_ue(&b, 0);
    b_ue(&b, 0);
    b_u(&b, 1, 0); /* weighted_pred */
    b_u(&b, 2, 0);
    b_se(&b, 0);
    b_se(&b, 0);
    b_se(&b, 0);
    b_u(&b, 1, 0); /* deblocking control */
    b_u(&b, 1, 0);
    b_u(&b, 1, 0); /* redundant_pic_cnt_present */
    return b_finish(&b, 0x68, nal);
}

/* A P slice. `mmco` < 0 writes the flag as 0; otherwise the flag is 1 and the
 * list is [mmco with difference `diff`], end. The slice data that follows is
 * deliberately full of zero runs, so emulation prevention bytes fall in
 * different places before and after the rewrite. */
static uint32_t make_p_slice(uint8_t *nal, uint8_t header, int mmco, uint32_t diff) {
    bits_t   b;
    unsigned k;

    memset(&b, 0, sizeof b);
    b_ue(&b, 0);    /* first_mb_in_slice */
    b_ue(&b, 5);    /* P, all slices */
    b_ue(&b, 0);    /* pps id */
    b_u(&b, 8, 1);  /* frame_num */
    b_u(&b, 1, 0);  /* num_ref_idx_active_override_flag */
    b_u(&b, 1, 0);  /* ref_pic_list_modification_flag_l0 */
    if (mmco < 0) {
        b_u(&b, 1, 0);
    } else {
        b_u(&b, 1, 1);
        b_ue(&b, (uint32_t)mmco);
        b_ue(&b, diff);
        b_ue(&b, 0);
    }
    b_se(&b, -11); /* slice_qp_delta */
    for (k = 0; k < 40; k++) {
        b_u(&b, 8, (k % 5 == 0) ? 0x5Au : 0x00u);
        b_u(&b, 3, k & 7u);
    }
    return b_finish(&b, header, nal);
}

/* Wraps NAL units into a 4-byte length-prefixed access unit. */
static uint32_t au_of(uint8_t *au, const uint8_t *nal, uint32_t len) {
    au[0] = (uint8_t)(len >> 24);
    au[1] = (uint8_t)(len >> 16);
    au[2] = (uint8_t)(len >> 8);
    au[3] = (uint8_t)len;
    memcpy(au + 4, nal, len);
    return len + 4;
}

static int params(h264_slice_params *p, uint32_t max_refs) {
    uint8_t  sps[64], pps[64];
    uint32_t sl = make_sps(sps, max_refs), pl = make_pps(pps);
    return h264_slice_params_parse(sps, (uint16_t)sl, pps, (uint16_t)pl, p) == H264_OK;
}

/* ---- the checks --------------------------------------------------------- */

static int t_rewrite_equals_sliding_window(char *note, unsigned n) {
    h264_slice_params p;
    uint8_t           nal[1024], want_nal[1024], au[1100], want[1100], out[1100];
    uint32_t          len, au_len, want_len, out_len, count;

    if (!params(&p, 1) || !h264_mmco_rewrite_applies(&p)) {
        snprintf(note, n, "the synthetic SPS/PPS did not parse as rewritable");
        return 1;
    }
    len      = make_p_slice(nal, 0x21, 1, 0);
    au_len   = au_of(au, nal, len);
    len      = make_p_slice(want_nal, 0x21, -1, 0);
    want_len = au_of(want, want_nal, len);

    if (h264_au_drop_redundant_mmco(&p, au, au_len, 4, out, sizeof out, &out_len, &count) != H264_OK) {
        snprintf(note, n, "the rewrite refused a well-formed access unit");
        return 1;
    }
    if (count != 1) {
        snprintf(note, n, "%u slices rewritten, expected 1", count);
        return 1;
    }
    if (out_len != want_len || memcmp(out, want, want_len) != 0) {
        snprintf(note, n, "the rewrite (%u bytes) differs from the sliding-window slice (%u bytes)", out_len, want_len);
        return 1;
    }
    snprintf(note, n, "%u -> %u bytes, identical to the slice written without the command", au_len, out_len);
    return 0;
}

static int t_other_patterns_untouched(char *note, unsigned n) {
    h264_slice_params p1, p2;
    uint8_t           nal[1024], au[1100], out[1100];
    uint32_t          len, au_len, out_len, count;

    if (!params(&p1, 1) || !params(&p2, 2)) {
        snprintf(note, n, "the synthetic SPS/PPS did not parse");
        return 1;
    }

    /* MMCO 1 with a different difference: not the previous frame. */
    len    = make_p_slice(nal, 0x21, 1, 1);
    au_len = au_of(au, nal, len);
    if (h264_au_drop_redundant_mmco(&p1, au, au_len, 4, out, sizeof out, &out_len, &count) != H264_OK || count != 0 ||
        out_len != au_len || memcmp(out, au, au_len) != 0) {
        snprintf(note, n, "an MMCO 1 with difference 1 was changed");
        return 1;
    }
    /* The redundant command, but two references allowed: not equivalent. */
    len    = make_p_slice(nal, 0x21, 1, 0);
    au_len = au_of(au, nal, len);
    if (h264_au_drop_redundant_mmco(&p2, au, au_len, 4, out, sizeof out, &out_len, &count) != H264_OK || count != 0 ||
        memcmp(out, au, au_len) != 0) {
        snprintf(note, n, "a stream with max_num_ref_frames 2 was changed");
        return 1;
    }
    /* A non-reference slice (nal_ref_idc 0) has no marking at all. */
    len    = make_p_slice(nal, 0x01, -1, 0);
    au_len = au_of(au, nal, len);
    if (h264_au_drop_redundant_mmco(&p1, au, au_len, 4, out, sizeof out, &out_len, &count) != H264_OK || count != 0 ||
        memcmp(out, au, au_len) != 0) {
        snprintf(note, n, "a non-reference slice was changed");
        return 1;
    }
    return 0;
}

static int t_too_small_output_refused(char *note, unsigned n) {
    h264_slice_params p;
    uint8_t           nal[1024], au[1100], out[16];
    uint32_t          len, au_len, out_len = 99;

    params(&p, 1);
    len    = make_p_slice(nal, 0x21, 1, 0);
    au_len = au_of(au, nal, len);
    if (h264_au_drop_redundant_mmco(&p, au, au_len, 4, out, sizeof out, &out_len, NULL) != H264_ERR_TOOBIG) {
        snprintf(note, n, "a 16-byte output buffer was accepted for a %u-byte access unit", au_len);
        return 1;
    }
    return 0;
}

/* ---- the server's own stream ------------------------------------------- */

static uint8_t *load_fixture(size_t *out_len) {
    static const char *candidates[2] = {"fixtures/probe360.mp4", "../../fixtures/probe360.mp4"};
    unsigned           i;

    for (i = 0; i < 2; i++) {
        FILE    *f = fopen(candidates[i], "rb");
        long     sz;
        uint8_t *buf;

        if (!f) continue;
        fseek(f, 0, SEEK_END);
        sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        buf = (uint8_t *)malloc((size_t)sz);
        if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz) {
            fclose(f);
            *out_len = (size_t)sz;
            return buf;
        }
        free(buf);
        fclose(f);
    }
    return NULL;
}

typedef struct {
    const h264_slice_params *p;
    uint32_t                 track, nal_len;
    unsigned                 samples, sync, rewritten, failed, grew;
} fixture_ctx;

static int on_video(const fmp4_sample *s, void *user) {
    fixture_ctx *c = (fixture_ctx *)user;
    static uint8_t out[256 * 1024];
    uint32_t       out_len, count;

    if (s->track_id != c->track) return 0;
    c->samples++;
    if (s->is_sync) c->sync++;
    if (h264_au_drop_redundant_mmco(c->p, s->data, s->size, (uint8_t)c->nal_len, out, sizeof out, &out_len, &count) !=
        H264_OK) {
        c->failed++;
        return 0;
    }
    c->rewritten += count;
    if (out_len > s->size) c->grew++;
    return 0;
}

static int t_fixture_p_frames_rewritten(char *note, unsigned n) {
    uint8_t          *file;
    size_t            len, off, consumed;
    fmp4              m;
    h264_slice_params p;
    fixture_ctx       c;
    h264_err          e;

    file = load_fixture(&len);
    if (!file) {
        snprintf(note, n, "fixtures/probe360.mp4 not found -- skipping");
        return -1;
    }
    if (fmp4_init(&m, file, len) != FMP4_OK) {
        snprintf(note, n, "fmp4_init failed: %s", m.err);
        free(file);
        return 1;
    }
    e = h264_slice_params_parse(m.video.sps, m.video.sps_len, m.video.pps, m.video.pps_len, &p);
    if (e != H264_OK || !h264_mmco_rewrite_applies(&p)) {
        snprintf(note, n, "the fixture's SPS/PPS did not parse as rewritable (%d)", (int)e);
        free(file);
        return 1;
    }

    memset(&c, 0, sizeof c);
    c.p       = &p;
    c.track   = m.video.track_id;
    c.nal_len = m.video.nal_length_size;
    for (off = fmp4_init_size(&m); off < len; off += consumed) {
        if (fmp4_fragment(&m, file + off, len - off, on_video, &c, &consumed) != FMP4_OK || consumed == 0) break;
    }
    free(file);

    /* Every P-frame of this stream carries the command; no IDR does. */
    if (c.failed || c.rewritten != c.samples - c.sync) {
        snprintf(note, n, "%u samples, %u sync, %u rewritten, %u refused", c.samples, c.sync, c.rewritten, c.failed);
        return 1;
    }
    snprintf(note, n, "%u samples: %u P-frames rewritten, %u IDRs untouched, %u grew", c.samples, c.rewritten, c.sync,
             c.grew);
    return 0;
}

void test_h264_mmco_register(void) {
    test_add("h264", "the MMCO rewrite equals the slice written with sliding-window marking",
             t_rewrite_equals_sliding_window);
    test_add("h264", "every other marking pattern is left untouched", t_other_patterns_untouched);
    test_add("h264", "a rewrite that would not fit the output is refused", t_too_small_output_refused);
    test_add("h264", "every P-frame of the server's fixture is rewritten", t_fixture_p_frames_rewritten);
}
