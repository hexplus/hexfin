/* The pure transformations that sit between fmp4 and the Media Engine's AAC
 * codec, exactly the way media/h264_au.h sits between fmp4 and the video
 * decoder. Everything here is arithmetic on bytes the caller already has: no
 * platform headers, no allocation, no I/O, so the host tests it.
 *
 *   - aac_parse_asc reads the AudioSpecificConfig fmp4 copied out of esds,
 *     because sceAudiocodec's AAC path (see audio_psp.c) needs the object
 *     type, sampling frequency index and channel configuration individually,
 *     not the config blob as a whole.
 *
 *   - aac_write_adts_header synthesises the 7-byte ADTS header the fmp4-fed
 *     raw AAC frames do not carry. Whether sceAudiocodec actually needs it is
 *     argued in audio_psp.c's header comment, not here -- this file only
 *     builds the bytes if asked.
 *
 *   - pcm_ring is the small buffer between "the codec just produced 1024
 *     samples" and "the channel wants some multiple of 64 of them right
 *     now". It is pure because it is just index arithmetic over a
 *     caller-owned array.
 *
 * All three exist here, in one file, rather than three, because the
 * constraints on this task name this file specifically -- but each is
 * self-contained and could be split later without the others noticing. */
#ifndef MEDIA_AAC_CONFIG_H
#define MEDIA_AAC_CONFIG_H

#include <stdint.h>

typedef enum {
    AAC_CONFIG_OK = 0,
    AAC_CONFIG_ERR_ARG,        /* a null pointer, or a buffer too short to hold what it claims */
    AAC_CONFIG_ERR_UNSUPPORTED /* parsed fine, but not a shape this project's decoder can take */
} aac_config_err;

/* ISO/IEC 14496-3 table 1.16's audioObjectType 2: "AAC LC". The only object
 * type this project's decoder is asked to take -- see aac_parse_asc. */
#define AAC_OBJECT_TYPE_AAC_LC 2

/* ---- AudioSpecificConfig -------------------------------------------------
 *
 * ISO/IEC 14496-3 section 1.6.2.1, GetAudioObjectType() plus the two fields
 * that follow it: 5 bits audio object type, 4 bits sampling frequency index,
 * 4 bits channel configuration. Everything past those 13 bits
 * (GASpecificConfig and beyond) is untouched -- our fixture's ASC is 5 bytes
 * and this reads only the first 2. */

typedef struct {
    uint8_t  object_type;         /* 5 bits */
    uint8_t  sampling_freq_index; /* 4 bits -- see aac_sample_rate_for_index */
    uint8_t  channel_config;      /* 4 bits, 0 = "defined in the bitstream", 1 = mono, 2 = stereo, ... */
    uint32_t sample_rate;         /* aac_sample_rate_for_index(sampling_freq_index); 0 if that was reserved/explicit */
} aac_asc_info;

/* The 13 defined sampling-frequency-index entries (ISO/IEC 14496-3 table
 * 1.18). Index 4 is 44100 Hz, this project's only fixture rate. Indices 13
 * and 14 are reserved and 15 means "the rate is written out explicitly
 * elsewhere in the ASC", which this decoder does not implement; all three
 * return 0, and so does anything past 15 -- the index arrived over a network
 * from a server this project does not control (PROMPT.md section 35), and a
 * 4-bit field off the wire is exactly the kind of value that should never be
 * used to index an array without this check in front of it. */
uint32_t aac_sample_rate_for_index(uint8_t index);

/* Parses `asc` into `out`. Requires `asc_len >= 2`: the three fields this
 * reads fit entirely in the first two bytes, so a shorter buffer cannot hold
 * them regardless of what FMP4_MAX_PARAM_SET allows elsewhere.
 *
 * Returns AAC_CONFIG_ERR_ARG for a null `asc`/`out` or `asc_len < 2`.
 * Returns AAC_CONFIG_ERR_UNSUPPORTED when the bytes parse but describe
 * something this project's decoder cannot take: any object type but AAC-LC,
 * or a sampling frequency index whose rate aac_sample_rate_for_index reports
 * as 0. A channel_config of 0 ("defined in the bitstream") is left to the
 * caller to reject or accept -- audio_decoder_open has fmp4's own channel
 * count to cross-check it against, and is a better place for that decision
 * than a parser that does not have that number. */
aac_config_err aac_parse_asc(const uint8_t *asc, uint16_t asc_len, aac_asc_info *out);

/* ---- ADTS synthesis -------------------------------------------------------
 *
 * See audio_psp.c for why this project believes sceAudiocodec's AAC path
 * wants ADTS framing even though fmp4's frames arrive without it. In short:
 * sceAudiocodecDecode takes no explicit per-frame length, only a fixed
 * generic upper bound on how much of the buffer it is safe to read, and the
 * only other codec this same entry point drives that needs its own frame
 * boundary (MP3) locates it by reading a sync word out of the buffer rather
 * than trusting a size the caller passed in. A raw AAC access unit has no
 * such sync word; ADTS's 7-byte header exists to supply exactly one. */

#define AAC_ADTS_HEADER_LEN 7

/* Writes AAC_ADTS_HEADER_LEN bytes to `out` (which must have at least that
 * much room) describing a frame of `payload_len` raw AAC bytes that
 * immediately follows it. `object_type`/`sampling_freq_index`/`channel_config`
 * are copied from the stream's already-parsed AudioSpecificConfig rather than
 * re-derived here, so the header this writes and the config the decoder was
 * opened with can never disagree with each other.
 *
 * Only AAC_OBJECT_TYPE_AAC_LC is accepted -- ADTS's 2-bit profile field
 * cannot represent every MPEG-4 object type anyway, and this project has no
 * use for the others.
 *
 * ADTS's aac_frame_length field is 13 bits and counts the header itself, so
 * the largest payload this can frame is 0x1FFF - AAC_ADTS_HEADER_LEN. A
 * payload past that returns AAC_CONFIG_ERR_UNSUPPORTED rather than writing a
 * length that has silently wrapped -- an AAC-LC frame this large has never
 * been observed from this project's server and would be a corrupt length
 * from a network we do not control, not a real frame (PROMPT.md section 35). */
aac_config_err aac_write_adts_header(uint8_t *out, uint8_t object_type, uint8_t sampling_freq_index,
                                     uint8_t channel_config, uint32_t payload_len);

/* ---- PCM ring buffer -------------------------------------------------------
 *
 * Holds a fraction of a second of decoded PCM (PROMPT.md section 24: 100-300
 * ms, not seconds -- this machine has roughly 22 MB in total). Interleaved
 * 16-bit samples, fixed capacity, backed by a caller-owned array so nothing
 * here allocates -- the PSP side makes that array a file static, the same way
 * video_psp.c does for every buffer it touches from the decode path.
 *
 * "Frame" below means one sample-frame: one sample per channel, not an AAC
 * access unit -- the same word video_decoder.h avoids for exactly the
 * opposite reason, which is why it is spelled out here instead of assumed. */

typedef struct {
    int16_t *buf;             /* caller-owned, capacity_frames * channels samples, interleaved */
    uint32_t capacity_frames;
    uint8_t  channels;
    uint32_t head;             /* index, in frames, of the oldest unread sample-frame */
    uint32_t count;            /* frames currently held; always <= capacity_frames */
} pcm_ring;

/* Binds `r` to `storage`, which must hold at least
 * capacity_frames * channels samples and outlive every call made with `r`.
 * Starts empty. `channels` of 0 is refused the same way the rest of this
 * file refuses a zero it would otherwise divide or index by. */
aac_config_err pcm_ring_init(pcm_ring *r, int16_t *storage, uint32_t capacity_frames, uint8_t channels);

/* Appends up to `frames` sample-frames from `src`. Returns the number
 * actually written, which is less than `frames` exactly when the ring does
 * not have that much free space left -- it never overwrites a sample-frame a
 * reader has not taken yet, because a decode loop that overran has nowhere
 * else to put the extra output, and dropping it silently would be a worse
 * failure than visibly falling behind. */
uint32_t pcm_ring_write(pcm_ring *r, const int16_t *src, uint32_t frames);

/* Copies up to `frames` sample-frames into `dst` and removes them from the
 * ring. Returns the number actually copied, which is never more than
 * pcm_ring_available(r) reported a moment before -- a caller cannot read out
 * more than the ring holds no matter what it asks for. */
uint32_t pcm_ring_read(pcm_ring *r, int16_t *dst, uint32_t frames);

/* How many sample-frames are queued right now. */
uint32_t pcm_ring_available(const pcm_ring *r);

#endif
