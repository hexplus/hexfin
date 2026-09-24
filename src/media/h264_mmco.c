/* See media/h264_mmco.h.
 *
 * Everything here works on the escaped NAL bytes directly: the reader drops
 * emulation prevention bytes as it goes and the writer puts them back, so no
 * RBSP copy of a slice is ever held and nothing needs a scratch buffer. */
#include "media/h264_mmco.h"

#include <string.h>

/* ----------------------------------------------------------- bit reading */

typedef struct {
    const uint8_t *p;
    uint32_t       n;     /* escaped bytes available */
    uint32_t       i;     /* next escaped byte */
    unsigned       zeros; /* consecutive 0x00 bytes just read, for 00 00 03 */
    uint32_t       cur;
    unsigned       left;  /* unread bits in cur */
    uint32_t       bits;  /* RBSP bits consumed so far */
    int            err;
} bitreader;

static void br_init(bitreader *r, const uint8_t *p, uint32_t n) {
    memset(r, 0, sizeof *r);
    r->p = p;
    r->n = n;
}

static uint32_t br_byte(bitreader *r) {
    uint32_t b;

    if (r->i >= r->n) {
        r->err = 1;
        return 0;
    }
    b = r->p[r->i++];
    if (r->zeros >= 2 && b == 0x03) {
        /* An emulation prevention byte: not part of the RBSP. */
        r->zeros = 0;
        if (r->i >= r->n) {
            r->err = 1;
            return 0;
        }
        b = r->p[r->i++];
    }
    r->zeros = (b == 0) ? r->zeros + 1 : 0;
    return b;
}

static uint32_t br_bit(bitreader *r) {
    if (r->left == 0) {
        r->cur  = br_byte(r);
        r->left = 8;
    }
    r->left--;
    r->bits++;
    return (r->cur >> r->left) & 1u;
}

static uint32_t br_u(bitreader *r, unsigned n) {
    uint32_t v = 0;
    while (n--) v = (v << 1) | br_bit(r);
    return v;
}

/* Exp-Golomb. More than 31 leading zeros is not a value any field here can
 * hold, and is refused rather than shifted into nonsense. */
static uint32_t br_ue(bitreader *r) {
    unsigned lz = 0;

    while (!r->err && br_bit(r) == 0) {
        if (++lz > 31) {
            r->err = 1;
            return 0;
        }
    }
    return lz ? ((1u << lz) - 1u) + br_u(r, lz) : 0;
}

static int32_t br_se(bitreader *r) {
    uint32_t k = br_ue(r);
    return (k & 1u) ? (int32_t)((k + 1u) / 2u) : -(int32_t)(k / 2u);
}

/* ----------------------------------------------------------- bit writing */

typedef struct {
    uint8_t *p;
    uint32_t cap;
    uint32_t n;
    unsigned zeros;
    uint32_t acc;
    unsigned nacc;
    int      err;
} bitwriter;

static void bw_emit(bitwriter *w, uint8_t b) {
    /* Re-inserting emulation prevention: 00 00 followed by 00..03 must not
     * appear in a NAL payload. */
    if (w->zeros >= 2 && b <= 3) {
        if (w->n >= w->cap) {
            w->err = 1;
            return;
        }
        w->p[w->n++] = 0x03;
        w->zeros     = 0;
    }
    if (w->n >= w->cap) {
        w->err = 1;
        return;
    }
    w->p[w->n++] = b;
    w->zeros     = (b == 0) ? w->zeros + 1 : 0;
}

static void bw_bit(bitwriter *w, uint32_t bit) {
    w->acc = (w->acc << 1) | (bit & 1u);
    if (++w->nacc == 8) {
        bw_emit(w, (uint8_t)w->acc);
        w->acc  = 0;
        w->nacc = 0;
    }
}

/* ----------------------------------------------------- parameter sets */

static int is_high_profile(uint32_t profile_idc) {
    switch (profile_idc) {
        case 100: case 110: case 122: case 244: case 44: case 83:
        case 86:  case 118: case 128: case 138: case 139: case 134: case 135:
            return 1;
        default:
            return 0;
    }
}

h264_err h264_slice_params_parse(const uint8_t *sps, uint16_t sps_len, const uint8_t *pps, uint16_t pps_len,
                                 h264_slice_params *out) {
    bitreader r;
    uint32_t  profile_idc, v;

    if (!sps || !pps || !out || sps_len < 2 || pps_len < 2) return H264_ERR_ARG;
    if ((sps[0] & 0x1Fu) != H264_NAL_SPS || (pps[0] & 0x1Fu) != H264_NAL_PPS) return H264_ERR_MALFORMED;
    memset(out, 0, sizeof *out);

    br_init(&r, sps + 1, sps_len - 1u);
    profile_idc = br_u(&r, 8);
    (void)br_u(&r, 8); /* constraint flags */
    (void)br_u(&r, 8); /* level_idc */
    (void)br_ue(&r);   /* seq_parameter_set_id */
    if (is_high_profile(profile_idc)) {
        uint32_t chroma_format_idc = br_ue(&r);
        /* separate_colour_plane puts colour_plane_id in every slice header,
         * and scaling matrices would have to be walked: neither occurs in
         * what this project's server sends, so neither is supported. */
        if (chroma_format_idc == 3 && br_u(&r, 1)) return H264_ERR_UNSUPPORTED;
        (void)br_ue(&r); /* bit_depth_luma_minus8 */
        (void)br_ue(&r); /* bit_depth_chroma_minus8 */
        (void)br_u(&r, 1); /* qpprime_y_zero_transform_bypass_flag */
        if (br_u(&r, 1)) return H264_ERR_UNSUPPORTED; /* seq_scaling_matrix_present_flag */
    }
    v = br_ue(&r);
    if (v > 12) return H264_ERR_MALFORMED;
    out->log2_max_frame_num = (uint8_t)(v + 4);
    v = br_ue(&r);
    if (v > 2) return H264_ERR_MALFORMED;
    out->poc_type = (uint8_t)v;
    if (out->poc_type == 0) {
        v = br_ue(&r);
        if (v > 12) return H264_ERR_MALFORMED;
        out->log2_max_poc_lsb = (uint8_t)(v + 4);
    } else if (out->poc_type == 1) {
        uint32_t cycle, k;
        out->delta_pic_order_always_zero = (uint8_t)br_u(&r, 1);
        (void)br_se(&r); /* offset_for_non_ref_pic */
        (void)br_se(&r); /* offset_for_top_to_bottom_field */
        cycle = br_ue(&r);
        if (cycle > 255) return H264_ERR_MALFORMED;
        for (k = 0; k < cycle && !r.err; k++) (void)br_se(&r);
    }
    v = br_ue(&r);
    if (v > 16) return H264_ERR_MALFORMED;
    out->max_num_ref_frames = (uint8_t)v;
    (void)br_u(&r, 1); /* gaps_in_frame_num_value_allowed_flag */
    (void)br_ue(&r);   /* pic_width_in_mbs_minus1 */
    (void)br_ue(&r);   /* pic_height_in_map_units_minus1 */
    out->frame_mbs_only = (uint8_t)br_u(&r, 1);
    if (r.err) return H264_ERR_MALFORMED;

    br_init(&r, pps + 1, pps_len - 1u);
    (void)br_ue(&r); /* pic_parameter_set_id */
    (void)br_ue(&r); /* seq_parameter_set_id */
    out->cabac                          = (uint8_t)br_u(&r, 1);
    out->bottom_field_pic_order_present = (uint8_t)br_u(&r, 1);
    /* Slice groups (FMO) carry a map whose syntax this does not walk; the
     * fields needed below come after it. Baseline allows FMO, Constrained
     * Baseline -- what the server is asked for -- does not. */
    if (br_ue(&r) != 0) return r.err ? H264_ERR_MALFORMED : H264_ERR_UNSUPPORTED;
    (void)br_ue(&r); /* num_ref_idx_l0_default_active_minus1 */
    (void)br_ue(&r); /* num_ref_idx_l1_default_active_minus1 */
    out->weighted_pred = (uint8_t)br_u(&r, 1);
    (void)br_u(&r, 2); /* weighted_bipred_idc */
    (void)br_se(&r);   /* pic_init_qp_minus26 */
    (void)br_se(&r);   /* pic_init_qs_minus26 */
    (void)br_se(&r);   /* chroma_qp_index_offset */
    (void)br_u(&r, 1); /* deblocking_filter_control_present_flag */
    (void)br_u(&r, 1); /* constrained_intra_pred_flag */
    out->redundant_pic_cnt_present = (uint8_t)br_u(&r, 1);
    if (r.err) return H264_ERR_MALFORMED;
    return H264_OK;
}

int h264_mmco_rewrite_applies(const h264_slice_params *p) {
    return p && p->max_num_ref_frames == 1 && !p->cabac && p->frame_mbs_only;
}

/* ------------------------------------------------------------- slices */

#define SLICE_P 0
#define SLICE_I 2

/* Walks one slice NAL's header (payload after the NAL header byte) as far as
 * dec_ref_pic_marking. Returns 1, with the RBSP bit positions of the adaptive
 * flag and of the first bit after the command list, when the header carries
 * exactly the redundant command; 0 when it does not, or when anything about
 * it is outside what h264_mmco.h promises to handle. */
static int find_redundant_mmco(const h264_slice_params *p, const uint8_t *payload, uint32_t len, uint32_t *flag_at,
                               uint32_t *after) {
    bitreader r;
    uint32_t  slice_type, k;

    br_init(&r, payload, len);
    (void)br_ue(&r); /* first_mb_in_slice */
    slice_type = br_ue(&r) % 5u;
    if (slice_type != SLICE_P && slice_type != SLICE_I) return 0;
    (void)br_ue(&r); /* pic_parameter_set_id */
    (void)br_u(&r, p->log2_max_frame_num); /* frame_num */
    /* frame_mbs_only is required by h264_mmco_rewrite_applies, so there is
     * no field_pic_flag here; and this is never an IDR, so no idr_pic_id. */
    if (p->poc_type == 0) {
        (void)br_u(&r, p->log2_max_poc_lsb);
        if (p->bottom_field_pic_order_present) (void)br_se(&r);
    } else if (p->poc_type == 1 && !p->delta_pic_order_always_zero) {
        (void)br_se(&r);
        if (p->bottom_field_pic_order_present) (void)br_se(&r);
    }
    if (p->redundant_pic_cnt_present) (void)br_ue(&r);
    if (slice_type == SLICE_P) {
        if (br_u(&r, 1)) (void)br_ue(&r); /* num_ref_idx_active_override_flag */
        if (br_u(&r, 1)) {                /* ref_pic_list_modification_flag_l0 */
            for (k = 0;; k++) {
                uint32_t idc = br_ue(&r);
                if (r.err || k > 32) return 0;
                if (idc == 3) break;
                if (idc > 2) return 0;
                (void)br_ue(&r);
            }
        }
        /* pred_weight_table sits here, and is not walked. */
        if (p->weighted_pred) return 0;
    }

    /* dec_ref_pic_marking, non-IDR. */
    *flag_at = r.bits;
    if (br_u(&r, 1) != 1) return 0;
    if (br_ue(&r) != 1) return 0; /* memory_management_control_operation */
    if (br_ue(&r) != 0) return 0; /* difference_of_pic_nums_minus1 */
    if (br_ue(&r) != 0) return 0; /* end of the list */
    *after = r.bits;
    return !r.err;
}

/* The number of RBSP bits before the rbsp_stop_one_bit, or 0 if the payload
 * does not end the way a CAVLC slice must (its last byte holds the stop bit
 * and is therefore never zero). */
static uint32_t rbsp_data_bits(const uint8_t *payload, uint32_t len) {
    uint32_t i, zeros = 0, epb = 0;
    uint8_t  last;
    unsigned tz = 0;

    if (len == 0 || payload[len - 1] == 0) return 0;
    for (i = 0; i < len; i++) {
        if (zeros >= 2 && payload[i] == 0x03) {
            epb++;
            zeros = 0;
            continue;
        }
        zeros = (payload[i] == 0) ? zeros + 1 : 0;
    }
    last = payload[len - 1];
    while (!(last & 1u)) {
        last >>= 1;
        tz++;
    }
    /* Everything up to the last set bit, minus that bit itself. */
    return (len - epb) * 8u - tz - 1u;
}

/* Writes the rewritten payload of one slice NAL into `w`. */
static void rewrite_slice(const uint8_t *payload, uint32_t len, uint32_t flag_at, uint32_t after, uint32_t data_bits,
                          bitwriter *w) {
    bitreader r;

    br_init(&r, payload, len);
    while (r.bits < flag_at) bw_bit(w, br_bit(&r));
    bw_bit(w, 0); /* adaptive_ref_pic_marking_mode_flag: sliding window */
    while (r.bits < after) (void)br_bit(&r);
    while (r.bits < data_bits && !r.err) bw_bit(w, br_bit(&r));
    bw_bit(w, 1); /* rbsp_stop_one_bit */
    while (w->nacc != 0) bw_bit(w, 0);
    if (r.err) w->err = 1;
}

static void put_prefix(uint8_t *p, uint8_t width, uint32_t v) {
    uint8_t k;
    for (k = 0; k < width; k++) p[k] = (uint8_t)(v >> (8u * (width - 1u - k)));
}

h264_err h264_au_drop_redundant_mmco(const h264_slice_params *p, const uint8_t *au, uint32_t size,
                                     uint8_t nal_length_size, uint8_t *out, uint32_t out_cap, uint32_t *out_len,
                                     uint32_t *rewritten) {
    uint32_t in = 0, o = 0, count = 0;

    if (!p || !au || !out || !out_len) return H264_ERR_ARG;
    if (nal_length_size != 1 && nal_length_size != 2 && nal_length_size != 4) return H264_ERR_ARG;
    *out_len = 0;
    if (rewritten) *rewritten = 0;

    while (in + nal_length_size <= size) {
        uint32_t       len = 0, k, flag_at = 0, after = 0, data_bits;
        const uint8_t *nal;
        int            done = 0;

        for (k = 0; k < nal_length_size; k++) len = (len << 8) | au[in + k];
        if (len == 0 || len > size - in - nal_length_size) return H264_ERR_MALFORMED;
        nal = au + in + nal_length_size;

        /* Only a non-IDR slice (type 1) that is itself a reference
         * (nal_ref_idc != 0) has dec_ref_pic_marking to rewrite. */
        if ((nal[0] & 0x1Fu) == 1 && (nal[0] & 0x60u) != 0 && h264_mmco_rewrite_applies(p) &&
            find_redundant_mmco(p, nal + 1, len - 1u, &flag_at, &after) &&
            (data_bits = rbsp_data_bits(nal + 1, len - 1u)) > after) {
            bitwriter w;

            if (o + nal_length_size + 1u > out_cap) return H264_ERR_TOOBIG;
            memset(&w, 0, sizeof w);
            w.p   = out + o + nal_length_size + 1u;
            w.cap = out_cap - o - nal_length_size - 1u;
            rewrite_slice(nal + 1, len - 1u, flag_at, after, data_bits, &w);
            if (w.err) return H264_ERR_TOOBIG;
            out[o + nal_length_size] = nal[0];
            put_prefix(out + o, nal_length_size, w.n + 1u);
            o += nal_length_size + 1u + w.n;
            count++;
            done = 1;
        }
        if (!done) {
            if (o + nal_length_size + len > out_cap) return H264_ERR_TOOBIG;
            memcpy(out + o, au + in, nal_length_size + len);
            o += nal_length_size + len;
        }
        in += nal_length_size + len;
    }
    if (in != size) return H264_ERR_MALFORMED;

    *out_len = o;
    if (rewritten) *rewritten = count;
    return H264_OK;
}
