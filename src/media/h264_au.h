/* The two pure transformations that sit between fmp4 and the Media Engine.
 *
 * The decoder itself cannot run on the development host, but everything that
 * happens to the bytes on the way to it can be tested there, so it lives here
 * rather than inside video_psp.c:
 *
 *   - h264_au_check walks an access unit's length-prefixed NAL units and
 *     refuses one whose prefix runs past the end. This is not paranoia about
 *     our own parser. The access unit arrives from a Jellyfin server over a
 *     network (PROMPT.md section 35), and the buffer it describes is handed
 *     straight to the Media Engine, which reads it by DMA with no bounds
 *     check of any kind. A length field one byte too large is a wild read on
 *     hardware, and hardware is the only place it would ever show up.
 *
 *   - h264_build_avcc reassembles the AVCDecoderConfigurationRecord from the
 *     SPS and PPS that fmp4 extracted. The decoder wants the parameter sets
 *     in that structured, self-describing form; fmp4 deliberately hands back
 *     the parsed pieces instead, because everything above it wants those.
 *
 * No platform headers, no allocation, no I/O: give it a buffer and it fills
 * it or refuses. */
#ifndef MEDIA_H264_AU_H
#define MEDIA_H264_AU_H

#include <stdint.h>

/* 11 fixed bytes, plus one SPS and one PPS at fmp4's 64-byte cap each. The
 * record is tens of bytes at Constrained Baseline; this is generous. */
#define H264_AVCC_MAX 160

typedef enum {
    H264_OK = 0,
    H264_ERR_ARG,        /* an argument cannot be right -- a null, or a nal_length_size that is not 1, 2 or 4 */
    H264_ERR_TRUNCATED,  /* a NAL length prefix claims more bytes than the access unit holds */
    H264_ERR_MALFORMED,  /* an empty NAL, or a tail too short to hold another prefix */
    H264_ERR_TOOBIG,     /* the result would not fit the caller's buffer */
    H264_ERR_UNSUPPORTED /* well-formed, but a shape the caller does not handle (see h264_mmco.h) */
} h264_err;

/* NAL unit types, for the parameter-set check. */
#define H264_NAL_SPS 7
#define H264_NAL_PPS 8

/* Walks `au` as `nal_length_size`-prefixed NAL units and confirms every unit
 * lies wholly inside it. `nal_count`, if given, receives how many were found.
 *
 * An access unit of zero length is H264_ERR_MALFORMED, not success: a frame
 * with no NAL units in it is a fault upstream, and passing it on would have
 * the decoder fail with something far less specific.
 *
 * ISO/IEC 14496-15 allows only 1, 2 and 4 for the prefix width, so a 3 here
 * means avcC was misparsed or the server sent something strange, and is
 * refused rather than guessed at. */
h264_err h264_au_check(const uint8_t *au, uint32_t size, uint8_t nal_length_size, uint32_t *nal_count);

/* Builds an AVCDecoderConfigurationRecord into `out`, from one SPS and one
 * PPS as fmp4 produced them: raw NAL units, no start code, no length prefix.
 *
 * `out_len` receives the record's length. Refuses parameter sets whose NAL
 * type is not SPS/PPS, and an SPS shorter than the four bytes the record's
 * profile/compatibility/level fields are copied out of -- both are cheap
 * checks against untrusted input that would otherwise reach the decoder as
 * nonsense. */
h264_err h264_build_avcc(const uint8_t *sps, uint16_t sps_len, const uint8_t *pps, uint16_t pps_len,
                         uint8_t nal_length_size, uint8_t *out, uint32_t out_cap, uint32_t *out_len);

#endif
