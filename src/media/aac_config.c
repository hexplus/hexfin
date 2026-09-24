/* See media/aac_config.h. */

#include "media/aac_config.h"

/* ---- AudioSpecificConfig --------------------------------------------- */

/* ISO/IEC 14496-3 table 1.18. Index 4 is 44100 Hz, the only rate this
 * project's fixture and server are known to send; the others are here
 * because a channel-agnostic table is one fewer place for the two rates
 * (this one and whatever Jellyfin might someday transcode to) to disagree. */
static const uint32_t k_sample_rates[13] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350,
};

uint32_t aac_sample_rate_for_index(uint8_t index) {
    if (index >= (uint8_t)(sizeof(k_sample_rates) / sizeof(k_sample_rates[0]))) return 0;
    return k_sample_rates[index];
}

aac_config_err aac_parse_asc(const uint8_t *asc, uint16_t asc_len, aac_asc_info *out) {
    uint8_t object_type, freq_idx, channel_config;

    if (!asc || !out || asc_len < 2) return AAC_CONFIG_ERR_ARG;

    /* Bit layout (MSB first, spanning the first two bytes):
     *   byte0: object_type[4:0] | freq_idx[3:1]
     *   byte1: freq_idx[0] | channel_config[3:0] | 3 bits this project ignores */
    object_type    = (uint8_t)(asc[0] >> 3);
    freq_idx       = (uint8_t)(((asc[0] & 0x07) << 1) | (asc[1] >> 7));
    channel_config = (uint8_t)((asc[1] >> 3) & 0x0F);

    if (object_type != AAC_OBJECT_TYPE_AAC_LC) return AAC_CONFIG_ERR_UNSUPPORTED;

    out->object_type         = object_type;
    out->sampling_freq_index = freq_idx;
    out->channel_config      = channel_config;
    out->sample_rate         = aac_sample_rate_for_index(freq_idx);

    /* A reserved or explicit-rate index parses cleanly but is not usable:
     * aac_sample_rate_for_index already refused to guess at one, and 0 is
     * not a real sample rate either. */
    if (out->sample_rate == 0) return AAC_CONFIG_ERR_UNSUPPORTED;

    return AAC_CONFIG_OK;
}

/* ---- ADTS synthesis ---------------------------------------------------- */

/* The 13-bit aac_frame_length field counts the header itself, so this is the
 * largest value that field can hold at all -- independent of how much of
 * that room AAC_ADTS_HEADER_LEN then takes back. */
#define ADTS_FRAME_LENGTH_MAX 0x1FFF

/* adts_buffer_fullness, 11 bits: the conventional "VBR, do not know" value,
 * all ones. The Media Engine reads this field (if it reads it at all) the
 * same way every ADTS decoder does: as advisory, not load-bearing. */
#define ADTS_BUFFER_FULLNESS_VBR 0x7FF

aac_config_err aac_write_adts_header(uint8_t *out, uint8_t object_type, uint8_t sampling_freq_index,
                                     uint8_t channel_config, uint32_t payload_len) {
    uint32_t frame_len;
    uint8_t  profile;

    if (!out) return AAC_CONFIG_ERR_ARG;

    /* ADTS's profile field is 2 bits, encoding audioObjectType - 1 for the
     * four MPEG-2-era profiles it can express; AAC-LC (2) is the only one
     * this project ever asks for. */
    if (object_type != AAC_OBJECT_TYPE_AAC_LC) return AAC_CONFIG_ERR_UNSUPPORTED;
    if (sampling_freq_index > 15) return AAC_CONFIG_ERR_UNSUPPORTED; /* the field is 4 bits, full stop */
    if (channel_config > 7) return AAC_CONFIG_ERR_UNSUPPORTED;       /* ADTS's channel_configuration is 3 bits */

    /* Subtracting, never adding. payload_len is a sample size that came off
     * the network: at 0xFFFFFFFF the sum wraps to 6, sails past the bound,
     * and a frame length of 6 is written for a payload of four billion
     * bytes. The bound is therefore put on payload_len itself, before any
     * arithmetic can lose the high bits -- the same rule media/fmp4.c's
     * next_box follows for a largesize. */
    if (payload_len > (uint32_t)(ADTS_FRAME_LENGTH_MAX - AAC_ADTS_HEADER_LEN)) return AAC_CONFIG_ERR_UNSUPPORTED;
    frame_len = (uint32_t)AAC_ADTS_HEADER_LEN + payload_len;

    profile = (uint8_t)(object_type - 1);

    out[0] = 0xFF; /* syncword, high byte */
    out[1] = 0xF1; /* syncword low nibble (1111) | ID=0 (MPEG-4) | layer=00 | protection_absent=1 (no CRC) */
    out[2] = (uint8_t)((profile << 6) | (sampling_freq_index << 2) | /* private_bit=0 */ (channel_config >> 2));
    out[3] = (uint8_t)(((channel_config & 0x03) << 6) | /* original/copy=0, home=0, copyright bits=0 */
                       (uint8_t)(frame_len >> 11));
    out[4] = (uint8_t)((frame_len >> 3) & 0xFF);
    out[5] = (uint8_t)(((frame_len & 0x07) << 5) | (ADTS_BUFFER_FULLNESS_VBR >> 6));
    out[6] = (uint8_t)(((ADTS_BUFFER_FULLNESS_VBR & 0x3F) << 2) /* | num_raw_data_blocks=0 */);

    return AAC_CONFIG_OK;
}

/* ---- PCM ring buffer ---------------------------------------------------- */

aac_config_err pcm_ring_init(pcm_ring *r, int16_t *storage, uint32_t capacity_frames, uint8_t channels) {
    if (!r || !storage || capacity_frames == 0 || channels == 0) return AAC_CONFIG_ERR_ARG;

    r->buf             = storage;
    r->capacity_frames = capacity_frames;
    r->channels        = channels;
    r->head            = 0;
    r->count           = 0;
    return AAC_CONFIG_OK;
}

uint32_t pcm_ring_write(pcm_ring *r, const int16_t *src, uint32_t frames) {
    uint32_t free_frames, tail, first_chunk, i;

    if (!r || !src || frames == 0) return 0;

    /* Never overwrite a frame the reader has not taken yet -- clip to
     * whatever room is actually free rather than wrapping over it. */
    free_frames = r->capacity_frames - r->count;
    if (frames > free_frames) frames = free_frames;
    if (frames == 0) return 0;

    tail        = (r->head + r->count) % r->capacity_frames;
    first_chunk = r->capacity_frames - tail;
    if (first_chunk > frames) first_chunk = frames;

    for (i = 0; i < first_chunk * r->channels; i++) r->buf[tail * r->channels + i] = src[i];
    for (i = 0; i < (frames - first_chunk) * r->channels; i++)
        r->buf[i] = src[first_chunk * r->channels + i];

    r->count += frames;
    return frames;
}

uint32_t pcm_ring_read(pcm_ring *r, int16_t *dst, uint32_t frames) {
    uint32_t first_chunk, i;

    if (!r || !dst || frames == 0) return 0;

    /* Never hand back more than is actually held. */
    if (frames > r->count) frames = r->count;
    if (frames == 0) return 0;

    first_chunk = r->capacity_frames - r->head;
    if (first_chunk > frames) first_chunk = frames;

    for (i = 0; i < first_chunk * r->channels; i++) dst[i] = r->buf[r->head * r->channels + i];
    for (i = 0; i < (frames - first_chunk) * r->channels; i++) dst[first_chunk * r->channels + i] = r->buf[i];

    r->head = (r->head + frames) % r->capacity_frames;
    r->count -= frames;
    return frames;
}

uint32_t pcm_ring_available(const pcm_ring *r) {
    if (!r) return 0;
    return r->count;
}
