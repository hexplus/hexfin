/* See media/h264_au.h.
 *
 * The decoder itself cannot run here -- it is the Media Engine, and there
 * isn't one -- but everything that happens to the bytes on the way to it can
 * be checked on the host, and that is the only reason those transformations
 * live in a file of their own rather than inside video_psp.c.
 *
 * The first check is the one that matters most. An access unit arrives from a
 * server we do not control and is handed to the Media Engine, which reads it
 * by DMA with no bounds check of any kind: a NAL length prefix one byte too
 * large is a wild read that would only ever appear on hardware, where it
 * looks like a decoder fault rather than a parser one. */

#include "media/h264_au.h"

#include "test.h"

#include <stdio.h>
#include <string.h>

/* ---- helpers --------------------------------------------------------- */

/* Writes a 4-byte big-endian NAL length prefix. */
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* A plausible Constrained Baseline SPS and PPS. Only the first four bytes of
 * the SPS and the NAL type bytes are actually inspected by the code under
 * test; the rest is filler so the lengths are realistic. */
static const uint8_t k_sps[] = {0x67, 0x42, 0xC0, 0x0D, 0x96, 0x52, 0x02, 0xD0, 0x2D, 0xC8};
static const uint8_t k_pps[] = {0x68, 0xCE, 0x3C, 0x80};

/* ---- the framing walk ------------------------------------------------ */

static int t_overrunning_prefix_refused(char *note, unsigned n) {
    uint8_t au[16];

    memset(au, 0, sizeof(au));
    /* 16 bytes of buffer, 4 of prefix, and a claim of 100 bytes of payload. */
    put32(au, 100);
    au[4] = 0x65;

    if (h264_au_check(au, sizeof(au), 4, NULL) != H264_ERR_TRUNCATED) {
        snprintf(note, n, "a NAL claiming 100 bytes inside a 16-byte access unit was not refused");
        return 1;
    }
    return 0;
}

static int t_prefix_overflow_refused(char *note, unsigned n) {
    uint8_t au[32];

    memset(au, 0, sizeof(au));
    /* The wrap case: with a 4-byte prefix, adding this to the read position
     * overflows 32 bits and lands back inside the buffer, so a bounds check
     * written as `pos + len <= size` passes it. */
    put32(au, 0xFFFFFFFFu);

    if (h264_au_check(au, sizeof(au), 4, NULL) != H264_ERR_TRUNCATED) {
        snprintf(note, n, "a 0xFFFFFFFF length prefix was not refused -- the bounds check wraps");
        return 1;
    }
    return 0;
}

static int t_wellformed_walks_to_the_end(char *note, unsigned n) {
    uint8_t  au[4 + 6 + 4 + 3];
    uint32_t count = 0;

    memset(au, 0, sizeof(au));
    put32(au, 6);
    au[4] = 0x65; /* an IDR slice */
    put32(au + 4 + 6, 3);
    au[4 + 6 + 4] = 0x41; /* a non-IDR slice */

    if (h264_au_check(au, sizeof(au), 4, &count) != H264_OK) {
        snprintf(note, n, "a well-formed two-NAL access unit was refused");
        return 1;
    }
    if (count != 2) {
        snprintf(note, n, "expected 2 NAL units, walked %u", count);
        return 1;
    }
    snprintf(note, n, "%u NAL units in %u bytes", count, (unsigned)sizeof(au));
    return 0;
}

static int t_short_tail_refused(char *note, unsigned n) {
    uint8_t au[4 + 6 + 2];

    memset(au, 0, sizeof(au));
    put32(au, 6);
    /* Two bytes left over: too few to be another length prefix, which means
     * the walk has been out of step for some unknown distance. */
    if (h264_au_check(au, sizeof(au), 4, NULL) != H264_ERR_MALFORMED) {
        snprintf(note, n, "a 2-byte tail that cannot hold a length prefix was accepted");
        return 1;
    }
    return 0;
}

static int t_empty_nal_refused(char *note, unsigned n) {
    uint8_t au[8];

    memset(au, 0, sizeof(au));
    put32(au, 0);
    put32(au + 4, 0);

    if (h264_au_check(au, sizeof(au), 4, NULL) != H264_ERR_MALFORMED) {
        snprintf(note, n, "a zero-length NAL was accepted");
        return 1;
    }
    if (h264_au_check(au, 0, 4, NULL) != H264_ERR_MALFORMED) {
        snprintf(note, n, "a zero-length access unit was accepted");
        return 1;
    }
    return 0;
}

static int t_bad_prefix_width_refused(char *note, unsigned n) {
    uint8_t au[8];
    uint8_t widths[] = {0, 3, 5, 8};
    unsigned i;

    memset(au, 0, sizeof(au));
    /* One 1-byte prefix saying "one byte of payload", then that byte. */
    au[0] = 1;
    au[1] = 0x65;

    /* ISO/IEC 14496-15 allows 1, 2 and 4 only. A 3 here means avcC was
     * misparsed or the server sent something strange; guessing at it would
     * hand the Media Engine a misframed access unit. */
    /* Element count, not byte count. `sizeof(widths)` gave the right answer
     * only because the array is uint8_t, and would have silently stopped
     * checking three quarters of a uint32_t one. */
    for (i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
        if (h264_au_check(au, sizeof(au), widths[i], NULL) != H264_ERR_ARG) {
            snprintf(note, n, "nal_length_size %u was accepted", widths[i]);
            return 1;
        }
    }

    /* And the legal widths are genuinely accepted, so the check above is not
     * passing by refusing everything. */
    if (h264_au_check(au, 2, 1, NULL) != H264_OK) {
        snprintf(note, n, "a 1-byte prefix width was refused");
        return 1;
    }
    return 0;
}

/* ---- the parameter-set record ---------------------------------------- */

static int t_avcc_carries_the_parameter_sets(char *note, unsigned n) {
    uint8_t  out[H264_AVCC_MAX];
    uint32_t len = 0;

    memset(out, 0xAA, sizeof(out));
    if (h264_build_avcc(k_sps, sizeof(k_sps), k_pps, sizeof(k_pps), 4, out, sizeof(out), &len) != H264_OK) {
        snprintf(note, n, "a valid SPS/PPS pair was refused");
        return 1;
    }
    if (len != 11 + sizeof(k_sps) + sizeof(k_pps)) {
        snprintf(note, n, "record is %u bytes, expected %u", len, (unsigned)(11 + sizeof(k_sps) + sizeof(k_pps)));
        return 1;
    }
    if (out[0] != 1) {
        snprintf(note, n, "configurationVersion is %u, expected 1", out[0]);
        return 1;
    }
    if (out[1] != k_sps[1] || out[2] != k_sps[2] || out[3] != k_sps[3]) {
        snprintf(note, n, "profile/compatibility/level were not copied from the SPS");
        return 1;
    }
    /* The top six bits are specified as ones; a decoder that checks them
     * rejects a record that leaves them clear. */
    if (out[4] != (0xFC | 3)) {
        snprintf(note, n, "lengthSizeMinusOne byte is 0x%02X, expected 0xFF for a 4-byte prefix", out[4]);
        return 1;
    }
    if (out[5] != (0xE0 | 1)) {
        snprintf(note, n, "SPS count byte is 0x%02X, expected 0xE1", out[5]);
        return 1;
    }
    if (((out[6] << 8) | out[7]) != (int)sizeof(k_sps) || memcmp(out + 8, k_sps, sizeof(k_sps)) != 0) {
        snprintf(note, n, "the SPS was not stored with its length");
        return 1;
    }
    if (out[8 + sizeof(k_sps)] != 1) {
        snprintf(note, n, "PPS count is %u, expected 1", out[8 + sizeof(k_sps)]);
        return 1;
    }
    if (memcmp(out + 11 + sizeof(k_sps), k_pps, sizeof(k_pps)) != 0) {
        snprintf(note, n, "the PPS was not stored");
        return 1;
    }

    snprintf(note, n, "%u-byte record from a %u-byte SPS and a %u-byte PPS", len, (unsigned)sizeof(k_sps),
             (unsigned)sizeof(k_pps));
    return 0;
}

static int t_avcc_refuses_wrong_nal_types(char *note, unsigned n) {
    uint8_t  sps[sizeof(k_sps)];
    uint8_t  out[H264_AVCC_MAX];
    uint32_t len = 0;

    memcpy(sps, k_sps, sizeof(k_sps));
    sps[0] = 0x65; /* an IDR slice where an SPS should be */

    if (h264_build_avcc(sps, sizeof(sps), k_pps, sizeof(k_pps), 4, out, sizeof(out), &len) != H264_ERR_MALFORMED) {
        snprintf(note, n, "a slice NAL was accepted as an SPS");
        return 1;
    }
    if (h264_build_avcc(k_sps, sizeof(k_sps), k_sps, sizeof(k_sps), 4, out, sizeof(out), &len) != H264_ERR_MALFORMED) {
        snprintf(note, n, "an SPS was accepted as a PPS");
        return 1;
    }
    /* Too short for the profile/compatibility/level bytes the record copies. */
    if (h264_build_avcc(k_sps, 2, k_pps, sizeof(k_pps), 4, out, sizeof(out), &len) != H264_ERR_MALFORMED) {
        snprintf(note, n, "a 2-byte SPS was accepted");
        return 1;
    }
    return 0;
}

static int t_avcc_refuses_an_undersized_buffer(char *note, unsigned n) {
    uint8_t  out[H264_AVCC_MAX];
    uint32_t len = 99;

    memset(out, 0xAA, sizeof(out));
    if (h264_build_avcc(k_sps, sizeof(k_sps), k_pps, sizeof(k_pps), 4, out, 11 + sizeof(k_sps) + sizeof(k_pps) - 1,
                        &len) != H264_ERR_TOOBIG) {
        snprintf(note, n, "a buffer one byte too small was written into anyway");
        return 1;
    }
    if (len != 0) {
        snprintf(note, n, "a refused build still reported %u bytes written", len);
        return 1;
    }
    if (out[0] != 0xAA) {
        snprintf(note, n, "a refused build wrote into the buffer");
        return 1;
    }
    return 0;
}

void test_h264_au_register(void) {
    test_add("h264", "a NAL length that overruns the access unit is refused", t_overrunning_prefix_refused);
    test_add("h264", "a length prefix that would wrap the bounds check is refused", t_prefix_overflow_refused);
    test_add("h264", "a well-formed access unit walks to its exact end", t_wellformed_walks_to_the_end);
    test_add("h264", "a tail too short to hold a length prefix is refused", t_short_tail_refused);
    test_add("h264", "an empty NAL, and an empty access unit, are refused", t_empty_nal_refused);
    test_add("h264", "a NAL length prefix width other than 1, 2 or 4 is refused", t_bad_prefix_width_refused);
    test_add("h264", "the avcC record carries the parameter sets the decoder needs", t_avcc_carries_the_parameter_sets);
    test_add("h264", "parameter sets that are not an SPS and a PPS are refused", t_avcc_refuses_wrong_nal_types);
    test_add("h264", "a record that would not fit the buffer is refused, not truncated", t_avcc_refuses_an_undersized_buffer);
}
