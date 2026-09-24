/* See media/aac_config.h.
 *
 * Written before src/media/aac_config.c exists, so the first run of this file
 * is expected to fail to LINK -- see Step 4 of the task, and tests/test_fmp4.c
 * for the same convention. That failure is itself evidence these checks call
 * real aac_* entry points rather than nothing.
 *
 * The ASC-parsing checks against fixtures/probe360.mp4 skip (not pass) when
 * the fixture cannot be opened, per tests/test.h's rule that a skip still
 * costs the run its green exit code -- the synthetic checks alongside them
 * need no fixture and always run. */

#include "media/aac_config.h"
#include "media/fmp4.h"

#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- fixture loading, identical to tests/test_fmp4.c's -------------------
 *
 * Duplicated rather than shared: this project has no shared test-support
 * file yet, and a two-line helper is cheaper to repeat than to invent one for. */
static uint8_t *load_fixture(size_t *out_len) {
    static const char *candidates[2] = {"fixtures/probe360.mp4", "../../fixtures/probe360.mp4"};
    unsigned           i;

    for (i = 0; i < 2; i++) {
        FILE    *f = fopen(candidates[i], "rb");
        long     sz;
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

/* ---- 1: the sampling-frequency-index table -------------------------------- */

static int t_sample_rate_table_entries(char *note, unsigned n) {
    if (aac_sample_rate_for_index(4) != 44100) {
        snprintf(note, n, "index 4 gave %u, expected 44100", aac_sample_rate_for_index(4));
        return 1;
    }
    if (aac_sample_rate_for_index(0) != 96000) {
        snprintf(note, n, "index 0 gave %u, expected 96000", aac_sample_rate_for_index(0));
        return 1;
    }
    if (aac_sample_rate_for_index(11) != 8000) {
        snprintf(note, n, "index 11 gave %u, expected 8000", aac_sample_rate_for_index(11));
        return 1;
    }
    snprintf(note, n, "44100/96000/8000 Hz at indices 4/0/11");
    return 0;
}

static int t_sample_rate_table_rejects_out_of_range(char *note, unsigned n) {
    /* 13 and 14 are reserved, 15 is "explicit rate" (not implemented), and
     * anything past that cannot come from a real 4-bit field at all -- this
     * last one is the case that would read off the end of a 13-entry table
     * with no check in front of it. */
    uint8_t bad[] = {13, 14, 15, 255};
    unsigned i;

    for (i = 0; i < sizeof(bad); i++) {
        if (aac_sample_rate_for_index(bad[i]) != 0) {
            snprintf(note, n, "index %u gave a nonzero rate", bad[i]);
            return 1;
        }
    }
    snprintf(note, n, "indices 13, 14, 15 and 255 all report 0 rather than indexing off the table");
    return 0;
}

/* ---- 2: parsing a synthetic ASC -------------------------------------------
 *
 * Needs no fixture: the three fields this parses are exactly 13 bits, easy
 * to hand-assemble, and a synthetic check here can also exercise shapes the
 * one fixture does not have (a wrong object type, a short buffer). */

/* Packs object_type (5 bits), sampling_freq_index (4 bits) and
 * channel_config (4 bits) into the first two bytes of an ASC, the same
 * layout aac_parse_asc is documented to read. The trailing bits (GASpecific-
 * Config's frameLengthFlag etc.) are left zero, matching what this project's
 * fixture actually carries there. */
static void pack_asc(uint8_t *out, uint8_t object_type, uint8_t freq_idx, uint8_t channel_config) {
    out[0] = (uint8_t)((object_type << 3) | (freq_idx >> 1));
    out[1] = (uint8_t)((freq_idx << 7) | (channel_config << 3));
}

static int t_parse_asc_aac_lc_44100_stereo(char *note, unsigned n) {
    uint8_t      asc[2];
    aac_asc_info info;

    pack_asc(asc, AAC_OBJECT_TYPE_AAC_LC, 4, 2);

    if (aac_parse_asc(asc, sizeof(asc), &info) != AAC_CONFIG_OK) {
        snprintf(note, n, "a well-formed AAC-LC/44100/stereo ASC was refused");
        return 1;
    }
    if (info.object_type != AAC_OBJECT_TYPE_AAC_LC || info.sampling_freq_index != 4 || info.channel_config != 2 ||
        info.sample_rate != 44100) {
        snprintf(note, n, "object_type=%u freq_idx=%u channel_config=%u sample_rate=%u", info.object_type,
                 info.sampling_freq_index, info.channel_config, info.sample_rate);
        return 1;
    }
    snprintf(note, n, "AAC-LC, 44100 Hz, 2 channels");
    return 0;
}

static int t_parse_asc_rejects_wrong_object_type(char *note, unsigned n) {
    uint8_t      asc[2];
    aac_asc_info info;

    /* Object type 5 is SBR -- a real MPEG-4 value, just not the one this
     * project's decoder was built for. */
    pack_asc(asc, 5, 4, 2);

    if (aac_parse_asc(asc, sizeof(asc), &info) != AAC_CONFIG_ERR_UNSUPPORTED) {
        snprintf(note, n, "object type 5 (SBR) was accepted as usable");
        return 1;
    }
    return 0;
}

static int t_parse_asc_rejects_reserved_freq_index(char *note, unsigned n) {
    uint8_t      asc[2];
    aac_asc_info info;

    pack_asc(asc, AAC_OBJECT_TYPE_AAC_LC, 14, 2); /* reserved index */

    if (aac_parse_asc(asc, sizeof(asc), &info) != AAC_CONFIG_ERR_UNSUPPORTED) {
        snprintf(note, n, "a reserved sampling frequency index was accepted");
        return 1;
    }
    return 0;
}

static int t_parse_asc_rejects_short_buffer(char *note, unsigned n) {
    uint8_t      asc[1] = {0};
    aac_asc_info info;

    if (aac_parse_asc(asc, 1, &info) != AAC_CONFIG_ERR_ARG) {
        snprintf(note, n, "a 1-byte ASC was accepted");
        return 1;
    }
    if (aac_parse_asc(NULL, 5, &info) != AAC_CONFIG_ERR_ARG) {
        snprintf(note, n, "a null ASC pointer was accepted");
        return 1;
    }
    return 0;
}

/* ---- 3: parsing the fixture's actual ASC ---------------------------------- */

static int t_fixture_asc_parses_as_aac_lc(char *note, unsigned n) {
    uint8_t     *file;
    size_t       len;
    fmp4         m;
    fmp4_err     e;
    aac_asc_info info;

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

    if (aac_parse_asc(m.audio.asc, m.audio.asc_len, &info) != AAC_CONFIG_OK) {
        snprintf(note, n, "the fixture's own ASC (asc_len=%u) was refused", m.audio.asc_len);
        return 1;
    }
    if (info.object_type != AAC_OBJECT_TYPE_AAC_LC) {
        snprintf(note, n, "object_type=%u, expected AAC-LC (2)", info.object_type);
        return 1;
    }
    if (info.sample_rate != m.audio.sample_rate) {
        snprintf(note, n, "ASC says %u Hz, mp4a box says %u Hz", info.sample_rate, m.audio.sample_rate);
        return 1;
    }
    if (info.sample_rate != 44100) {
        snprintf(note, n, "sample_rate=%u, expected 44100", info.sample_rate);
        return 1;
    }
    snprintf(note, n, "AAC-LC, %u Hz, ASC channel_config=%u, mp4a channels=%u", info.sample_rate,
             info.channel_config, m.audio.channels);
    return 0;
}

/* ---- 4: ADTS header synthesis ---------------------------------------------- */

static int t_adts_header_round_trips(char *note, unsigned n) {
    uint8_t  hdr[AAC_ADTS_HEADER_LEN];
    uint32_t payload_len = 200;
    uint32_t frame_len;

    memset(hdr, 0xAA, sizeof(hdr));
    if (aac_write_adts_header(hdr, AAC_OBJECT_TYPE_AAC_LC, 4, 2, payload_len) != AAC_CONFIG_OK) {
        snprintf(note, n, "a plausible AAC-LC/44100/stereo header was refused");
        return 1;
    }

    /* Syncword: 0xFFF, the top 12 bits of the header. */
    if (hdr[0] != 0xFF || (hdr[1] >> 4) != 0x0F) {
        snprintf(note, n, "syncword is 0x%02X%X, expected 0xFFF", hdr[0], hdr[1] >> 4);
        return 1;
    }
    /* MPEG-4 (ID=0), layer always 00, protection_absent=1 (no CRC). */
    if (((hdr[1] >> 3) & 1) != 0 || ((hdr[1] >> 1) & 3) != 0 || (hdr[1] & 1) != 1) {
        snprintf(note, n, "header[1]=0x%02X does not encode MPEG-4/layer-0/no-CRC", hdr[1]);
        return 1;
    }

    /* profile = object_type - 1 (ADTS's 2-bit field), sampling_freq_index and
     * channel_config must read back exactly what was written in. */
    {
        uint8_t profile          = (uint8_t)(hdr[2] >> 6);
        uint8_t freq_idx         = (uint8_t)((hdr[2] >> 2) & 0x0F);
        uint8_t channel_config   = (uint8_t)(((hdr[2] & 0x01) << 2) | (hdr[3] >> 6));

        if (profile != AAC_OBJECT_TYPE_AAC_LC - 1) {
            snprintf(note, n, "profile=%u, expected %u", profile, AAC_OBJECT_TYPE_AAC_LC - 1);
            return 1;
        }
        if (freq_idx != 4) {
            snprintf(note, n, "sampling_freq_index=%u, expected 4", freq_idx);
            return 1;
        }
        if (channel_config != 2) {
            snprintf(note, n, "channel_config=%u, expected 2", channel_config);
            return 1;
        }
    }

    /* aac_frame_length: 13 bits, split across bytes 3-5, must equal the
     * header plus the payload. */
    frame_len = ((uint32_t)(hdr[3] & 0x03) << 11) | ((uint32_t)hdr[4] << 3) | ((uint32_t)hdr[5] >> 5);
    if (frame_len != AAC_ADTS_HEADER_LEN + payload_len) {
        snprintf(note, n, "aac_frame_length=%u, expected %u", frame_len, AAC_ADTS_HEADER_LEN + payload_len);
        return 1;
    }

    snprintf(note, n, "syncword, profile, freq index, channel config and length all round-trip");
    return 0;
}

static int t_adts_header_rejects_oversized_payload(char *note, unsigned n) {
    uint8_t hdr[AAC_ADTS_HEADER_LEN];

    memset(hdr, 0xAA, sizeof(hdr));
    /* aac_frame_length is 13 bits; payload_len + 7 must not exceed 0x1FFF. */
    if (aac_write_adts_header(hdr, AAC_OBJECT_TYPE_AAC_LC, 4, 2, 0x1FFF) != AAC_CONFIG_ERR_UNSUPPORTED) {
        snprintf(note, n, "a payload that overflows the 13-bit length field was accepted");
        return 1;
    }
    if (hdr[0] != 0xAA) {
        snprintf(note, n, "a refused header still wrote into the buffer");
        return 1;
    }
    return 0;
}

static int t_adts_header_rejects_non_lc_object_type(char *note, unsigned n) {
    uint8_t hdr[AAC_ADTS_HEADER_LEN];

    if (aac_write_adts_header(hdr, 5 /* SBR */, 4, 2, 100) != AAC_CONFIG_ERR_UNSUPPORTED) {
        snprintf(note, n, "a non-AAC-LC object type was accepted");
        return 1;
    }
    return 0;
}

/* ---- 5: the PCM ring ------------------------------------------------------- */

static int t_ring_fill_and_drain(char *note, unsigned n) {
    int16_t  storage[16 * 2]; /* 16 frames, stereo */
    pcm_ring r;
    int16_t  in[4 * 2], out[4 * 2];
    unsigned i;
    uint32_t wrote, read;

    for (i = 0; i < 4 * 2; i++) in[i] = (int16_t)(1000 + i);

    if (pcm_ring_init(&r, storage, 16, 2) != AAC_CONFIG_OK) {
        snprintf(note, n, "a valid ring init was refused");
        return 1;
    }
    if (pcm_ring_available(&r) != 0) {
        snprintf(note, n, "a freshly initialised ring reports %u frames available", pcm_ring_available(&r));
        return 1;
    }

    wrote = pcm_ring_write(&r, in, 4);
    if (wrote != 4) {
        snprintf(note, n, "writing 4 frames into an empty 16-frame ring wrote %u", wrote);
        return 1;
    }
    if (pcm_ring_available(&r) != 4) {
        snprintf(note, n, "available=%u after writing 4", pcm_ring_available(&r));
        return 1;
    }

    read = pcm_ring_read(&r, out, 4);
    if (read != 4 || memcmp(in, out, sizeof(in)) != 0) {
        snprintf(note, n, "read back %u frames, or contents did not match what was written", read);
        return 1;
    }
    if (pcm_ring_available(&r) != 0) {
        snprintf(note, n, "ring reports %u frames left after draining everything", pcm_ring_available(&r));
        return 1;
    }

    snprintf(note, n, "wrote 4, read 4, matched, drained to empty");
    return 0;
}

static int t_ring_never_reports_more_than_it_holds(char *note, unsigned n) {
    int16_t  storage[8 * 2]; /* 8 frames, stereo */
    pcm_ring r;
    int16_t  chunk[3 * 2];
    /* Sized for the whole ring, not for `chunk`'s 3 frames: pcm_ring_read
     * clips the *count* to what is held, not to anything it knows about the
     * destination, so asking for 100 from a full 8-frame ring writes 8 frames
     * out. Reading into `chunk` here overran it by 10 samples -- the check
     * still passed, because the overrun landed in the same stack frame. */
    int16_t  out[8 * 2];
    unsigned i;
    uint32_t wrote, avail, read;

    memset(chunk, 0, sizeof(chunk));
    if (pcm_ring_init(&r, storage, 8, 2) != AAC_CONFIG_OK) {
        snprintf(note, n, "ring init failed");
        return 1;
    }

    /* Overfill: three writes of 3 frames each into an 8-frame ring. The
     * third should be clipped to whatever room is left, never wrapping over
     * data that has not been read yet. */
    wrote = 0;
    for (i = 0; i < 3; i++) wrote += pcm_ring_write(&r, chunk, 3);

    if (wrote != 8) {
        snprintf(note, n, "three writes of 3 into an 8-frame ring totalled %u, expected 8 (clipped)", wrote);
        return 1;
    }
    avail = pcm_ring_available(&r);
    if (avail != 8) {
        snprintf(note, n, "available=%u, expected 8 (the ring's full capacity)", avail);
        return 1;
    }

    /* Asking to read more than is held must not report more than was held. */
    read = pcm_ring_read(&r, out, 100);
    if (read != 8) {
        snprintf(note, n, "reading with a request of 100 from a ring holding 8 returned %u", read);
        return 1;
    }
    if (pcm_ring_available(&r) != 0) {
        snprintf(note, n, "ring reports %u available after everything was read out", pcm_ring_available(&r));
        return 1;
    }

    snprintf(note, n, "overfill clipped at capacity (8), over-read clipped at what was held (8)");
    return 0;
}

static int t_ring_wraps_correctly(char *note, unsigned n) {
    int16_t  storage[4 * 2]; /* 4 frames, stereo -- small, to force wraparound quickly */
    pcm_ring r;
    int16_t  a[3 * 2], b[3 * 2], out[4 * 2];
    unsigned i;

    for (i = 0; i < 3 * 2; i++) a[i] = (int16_t)(100 + i);
    for (i = 0; i < 3 * 2; i++) b[i] = (int16_t)(200 + i);

    if (pcm_ring_init(&r, storage, 4, 2) != AAC_CONFIG_OK) {
        snprintf(note, n, "ring init failed");
        return 1;
    }

    /* Fill 3 of 4, drain 3, then write 3 more -- the second write's frames
     * must wrap around the backing array's end, since head is not back at
     * index 0. */
    if (pcm_ring_write(&r, a, 3) != 3) {
        snprintf(note, n, "first write of 3 into an empty 4-frame ring did not take all 3");
        return 1;
    }
    if (pcm_ring_read(&r, out, 3) != 3 || memcmp(out, a, sizeof(a)) != 0) {
        snprintf(note, n, "draining the first write did not return what was written");
        return 1;
    }
    if (pcm_ring_write(&r, b, 3) != 3) {
        snprintf(note, n, "second write of 3 (which must wrap) did not take all 3");
        return 1;
    }
    if (pcm_ring_available(&r) != 3) {
        snprintf(note, n, "available=%u after the wrapping write, expected 3", pcm_ring_available(&r));
        return 1;
    }
    if (pcm_ring_read(&r, out, 3) != 3 || memcmp(out, b, sizeof(b)) != 0) {
        snprintf(note, n, "draining the wrapped write did not return what was written, in order");
        return 1;
    }

    snprintf(note, n, "a write that wraps the backing array still reads back in order");
    return 0;
}

static int t_ring_rejects_zero_channels(char *note, unsigned n) {
    int16_t  storage[4];
    pcm_ring r;

    if (pcm_ring_init(&r, storage, 4, 0) != AAC_CONFIG_ERR_ARG) {
        snprintf(note, n, "a ring with 0 channels was accepted");
        return 1;
    }
    return 0;
}

void test_aac_config_register(void) {
    test_add("aac", "sampling-frequency-index table entries 4/0/11", t_sample_rate_table_entries);
    test_add("aac", "sampling-frequency-index table rejects reserved/explicit/out-of-range indices",
              t_sample_rate_table_rejects_out_of_range);
    test_add("aac", "a synthetic AAC-LC/44100/stereo ASC parses correctly", t_parse_asc_aac_lc_44100_stereo);
    test_add("aac", "an ASC naming a non-AAC-LC object type is refused", t_parse_asc_rejects_wrong_object_type);
    test_add("aac", "an ASC naming a reserved sampling frequency index is refused",
              t_parse_asc_rejects_reserved_freq_index);
    test_add("aac", "an ASC shorter than 2 bytes, or a null one, is refused", t_parse_asc_rejects_short_buffer);
    test_add("aac", "the fixture's own ASC parses as AAC-LC and agrees with the mp4a box",
              t_fixture_asc_parses_as_aac_lc);
    test_add("aac", "a synthesised ADTS header round-trips its profile/rate/channels/length",
              t_adts_header_round_trips);
    test_add("aac", "an ADTS payload too large for the 13-bit length field is refused",
              t_adts_header_rejects_oversized_payload);
    test_add("aac", "an ADTS header for a non-AAC-LC object type is refused", t_adts_header_rejects_non_lc_object_type);
    test_add("aac", "the PCM ring fills and drains exactly what was written", t_ring_fill_and_drain);
    test_add("aac", "the PCM ring never reports more available or read than it holds",
              t_ring_never_reports_more_than_it_holds);
    test_add("aac", "the PCM ring wraps around its backing array correctly", t_ring_wraps_correctly);
    test_add("aac", "a PCM ring with 0 channels is refused", t_ring_rejects_zero_channels);
}
