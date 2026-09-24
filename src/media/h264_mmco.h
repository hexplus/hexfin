/* Rewriting away the one H.264 construct the PSP-1000's Media Engine was
 * seen to refuse.
 *
 * The Jellyfin server this project runs against encodes with h264_vaapi, and
 * every P-frame it writes carries an explicit memory-management command in
 * its slice header: adaptive_ref_pic_marking_mode_flag = 1, followed by
 * MMCO 1 with difference_of_pic_nums_minus1 = 0 -- "the previous frame is no
 * longer a reference". On hardware (2026-09-24, docs/RESEARCH.md section 10)
 * the Media Engine decoded that stream's IDR frames and refused every one of
 * its P-frames with 0x80628002, while a libx264 stream, whose P-frames leave
 * the flag at 0, decoded throughout.
 *
 * With max_num_ref_frames = 1 that command is redundant: the default
 * "sliding window" marking drops the oldest short-term reference as soon as
 * the reference list is full, and with room for one reference the oldest is
 * exactly the previous frame. So the header is rewritten to flag = 0 and the
 * command removed, which changes nothing a decoder reconstructs -- the rest of
 * the slice is copied bit for bit, shifted to close the gap, with emulation
 * prevention bytes removed and re-inserted on the fly.
 *
 * Deliberately narrow. Only non-IDR P and I slices, CAVLC, frame pictures,
 * no weighted prediction, and only the exact command list [MMCO 1 with
 * difference 0] under max_num_ref_frames == 1 are rewritten. Anything else --
 * including every other MMCO pattern -- is copied untouched, because there
 * the rewrite would no longer be provably equivalent.
 *
 * No platform headers, no allocation, no I/O. */
#ifndef MEDIA_H264_MMCO_H
#define MEDIA_H264_MMCO_H

#include "media/h264_au.h"

#include <stdint.h>

/* The handful of SPS/PPS fields a slice header's layout depends on, as far as
 * dec_ref_pic_marking. */
typedef struct {
    uint8_t log2_max_frame_num;
    uint8_t poc_type;
    uint8_t log2_max_poc_lsb; /* poc_type 0 only */
    uint8_t delta_pic_order_always_zero; /* poc_type 1 only */
    uint8_t max_num_ref_frames;
    uint8_t frame_mbs_only;
    uint8_t bottom_field_pic_order_present;
    uint8_t redundant_pic_cnt_present;
    uint8_t weighted_pred;
    uint8_t cabac;
} h264_slice_params;

/* Reads what h264_slice_params holds out of one SPS and one PPS, as fmp4
 * extracted them (raw NAL units, header byte included, emulation prevention
 * still in place).
 *
 * H264_OK when both parse. H264_ERR_UNSUPPORTED when they parse but describe
 * a stream this module does not rewrite (a High-profile SPS with scaling
 * matrices, which it does not walk). H264_ERR_MALFORMED when they do not
 * parse at all. */
h264_err h264_slice_params_parse(const uint8_t *sps, uint16_t sps_len, const uint8_t *pps, uint16_t pps_len,
                                 h264_slice_params *out);

/* 1 when the stream is one whose redundant MMCO can be rewritten at all --
 * max_num_ref_frames == 1, CAVLC, frame pictures only. Checked once, at open,
 * so the per-frame path can skip streams it would never change. */
int h264_mmco_rewrite_applies(const h264_slice_params *p);

/* Copies the length-prefixed access unit `au` into `out`, rewriting every
 * slice whose header carries exactly the redundant command described above.
 * Every other NAL unit is copied byte for byte. `rewritten`, if given,
 * receives how many slices were changed.
 *
 * `out` must not overlap `au`. H264_ERR_TOOBIG if the result does not fit
 * `out_cap` (the rewrite shortens a slice by at least five bits, but
 * re-inserted emulation prevention can, in principle, lengthen it); the
 * caller then sends the original. The framing is assumed already checked by
 * h264_au_check. */
h264_err h264_au_drop_redundant_mmco(const h264_slice_params *p, const uint8_t *au, uint32_t size,
                                     uint8_t nal_length_size, uint8_t *out, uint32_t out_cap, uint32_t *out_len,
                                     uint32_t *rewritten);

#endif
