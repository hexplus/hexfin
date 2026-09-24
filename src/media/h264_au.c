/* See media/h264_au.h. */

#include "media/h264_au.h"

h264_err h264_au_check(const uint8_t *au, uint32_t size, uint8_t nal_length_size, uint32_t *nal_count) {
    uint32_t pos   = 0;
    uint32_t count = 0;

    if (nal_count) *nal_count = 0;
    if (!au) return H264_ERR_ARG;
    if (nal_length_size != 1 && nal_length_size != 2 && nal_length_size != 4) return H264_ERR_ARG;
    if (size == 0) return H264_ERR_MALFORMED;

    while (pos < size) {
        uint32_t len = 0;
        uint8_t  i;

        /* The tail case first. A remainder too short to hold another prefix
         * is not "nearly right": it means the previous length was wrong, and
         * the walk has been out of step for some unknown distance. */
        if (size - pos < nal_length_size) return H264_ERR_MALFORMED;

        for (i = 0; i < nal_length_size; i++) len = (len << 8) | au[pos + i];
        pos += nal_length_size;

        if (len == 0) return H264_ERR_MALFORMED;

        /* Compared against what is LEFT rather than pos + len against size:
         * with a 4-byte prefix, len reaches 0xFFFFFFFF and the addition would
         * wrap to something comfortably inside the buffer. That wrap is
         * precisely the bug this function exists to stop. */
        if (len > size - pos) return H264_ERR_TRUNCATED;

        pos += len;
        count++;
    }

    if (nal_count) *nal_count = count;
    return H264_OK;
}

h264_err h264_build_avcc(const uint8_t *sps, uint16_t sps_len, const uint8_t *pps, uint16_t pps_len,
                         uint8_t nal_length_size, uint8_t *out, uint32_t out_cap, uint32_t *out_len) {
    uint32_t need;
    uint32_t n = 0;
    uint16_t i;

    if (out_len) *out_len = 0;
    if (!sps || !pps || !out) return H264_ERR_ARG;
    if (nal_length_size != 1 && nal_length_size != 2 && nal_length_size != 4) return H264_ERR_ARG;

    /* Four bytes because the record copies profile_idc, the constraint flags
     * and level_idc straight out of the SPS's first four bytes. */
    if (sps_len < 4 || pps_len < 1) return H264_ERR_MALFORMED;
    if ((sps[0] & 0x1F) != H264_NAL_SPS) return H264_ERR_MALFORMED;
    if ((pps[0] & 0x1F) != H264_NAL_PPS) return H264_ERR_MALFORMED;

    need = 11u + (uint32_t)sps_len + (uint32_t)pps_len;
    if (need > out_cap) return H264_ERR_TOOBIG;

    out[n++] = 1;          /* configurationVersion */
    out[n++] = sps[1];     /* AVCProfileIndication */
    out[n++] = sps[2];     /* profile_compatibility */
    out[n++] = sps[3];     /* AVCLevelIndication */

    /* The top six bits are reserved and specified as ones; a decoder that
     * checks them rejects a record that leaves them clear. */
    out[n++] = (uint8_t)(0xFC | (nal_length_size - 1));
    out[n++] = (uint8_t)(0xE0 | 1); /* three reserved ones, then one SPS */

    out[n++] = (uint8_t)(sps_len >> 8);
    out[n++] = (uint8_t)(sps_len & 0xFF);
    for (i = 0; i < sps_len; i++) out[n++] = sps[i];

    out[n++] = 1; /* one PPS */
    out[n++] = (uint8_t)(pps_len >> 8);
    out[n++] = (uint8_t)(pps_len & 0xFF);
    for (i = 0; i < pps_len; i++) out[n++] = pps[i];

    if (out_len) *out_len = n;
    return H264_OK;
}
