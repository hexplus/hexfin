/* The seam between "bytes that came off a network" and "a picture".
 *
 * Design section 3.1: this interface exists so the parser and the sync logic
 * can be exercised without a PSP. The real implementation is video_psp.c,
 * which decodes on the Media Engine and exists on the console and nowhere
 * else; a software decoder could implement the same five calls later without
 * a caller changing a line.
 *
 * Nothing here mentions sceMpeg, SceMpegAu or pixel layout beyond the pixel
 * format itself, on purpose: the moment a caller knows what is behind this
 * header, swapping the implementation stops being possible.
 *
 * No platform headers, so the host can at least compile a caller.
 *
 * Errors return a code and leave a readable reason behind
 * (PROMPT.md section 47). The code is what the caller branches on; the string
 * is what a person reads on the screen, and it is where a firmware status
 * word gets translated into plain words rather than printed as hex. */
#ifndef MEDIA_VIDEO_DECODER_H
#define MEDIA_VIDEO_DECODER_H

#include <stdint.h>

typedef enum {
    VIDEO_OK = 0,
    VIDEO_ERR_STATE,       /* called out of order: open twice, push before open */
    VIDEO_ERR_ARG,         /* the caller's arguments cannot be right */
    VIDEO_ERR_MEMORY,      /* the partition could not give us what we need */
    VIDEO_ERR_DRIVER,      /* a firmware call refused; the reason carries its code */
    VIDEO_ERR_BAD_STREAM,  /* the bytes handed in do not frame as H.264 */
    VIDEO_ERR_UNSUPPORTED, /* valid H.264, but not a shape this decoder can take */
    VIDEO_ERR_AGAIN        /* no frame ready yet. NOT a failure -- see below */
} video_err;

/* One decoded picture, described as a borrowed pointer rather than a copy:
 * the decoder owns these pixels and may overwrite them on the next push, so a
 * caller that wants to keep a frame must copy it.
 *
 * `stride` is in PIXELS, not bytes, because that is the unit sceGu and the
 * Media Engine both take, and a stride in bytes here would be converted at
 * every use. The Media Engine writes 512-pixel rows when the output is
 * framebuffer-shaped, so stride is normally 512 while width is 360 or 480. */
typedef struct {
    const void *pixels; /* 8888, one uint32_t per pixel, 0xAABBGGRR */
    uint16_t    stride; /* pixels per row, >= width */
    uint16_t    width;
    uint16_t    height;
    uint64_t    dts;    /* the DTS of the access unit this came from, in the track's timescale */
} video_frame;

/* Claims everything the decoder will ever need and hands it the parameter
 * sets. Called once, before playback, because design section 3.3 requires the
 * large blocks to be claimed in a fixed order before anything has had a
 * chance to fragment the partition.
 *
 * `sps` and `pps` are the parameter sets exactly as fmp4 extracted them from
 * avcC: raw NAL units, no start codes, no length prefix.
 * `nal_length_size` is avcC's, and describes the access units that will
 * arrive at video_decoder_push -- 4 for Jellyfin's output.
 *
 * Returns VIDEO_OK, or a code with the reason in video_decoder_error(). */
int video_decoder_open(const uint8_t *sps, uint16_t sps_len, const uint8_t *pps, uint16_t pps_len,
                       uint8_t nal_length_size, int width, int height);

/* Submits one access unit -- one frame's worth of length-prefixed NAL units,
 * as fmp4 emits them. `au` need not stay valid after the call returns.
 *
 * `dts` rides along so the frame that comes out can be matched to the audio
 * clock; this call does no timing of its own.
 *
 * `show` 0 decodes without producing a picture: for a frame that is already
 * too late to display. It must still be decoded -- later frames are
 * predicted from it -- but the colour conversion and the copy to the panel,
 * the expensive part, are skipped, and no frame comes out.
 *
 * Returns VIDEO_OK if the access unit was accepted. A decoder that reorders
 * may accept several before any frame is available, which is why "accepted"
 * and "a frame exists" are two different questions. */
int video_decoder_push(const uint8_t *au, uint32_t size, uint64_t dts, int show);

/* Fills `out` with the next frame in display order.
 *
 * Returns VIDEO_OK with `out` filled, or VIDEO_ERR_AGAIN when nothing is
 * ready yet -- which a caller must treat as "push more", not as a failure.
 * Keeping that distinction in the return code rather than in an out-parameter
 * is what lets a reordering software decoder implement this interface
 * unchanged. */
int video_decoder_next_frame(video_frame *out);

/* Releases everything video_decoder_open claimed. Safe to call when open
 * failed or was never called: the teardown path in design section 3.4 runs
 * after failures too, and a close that only works after a successful open is
 * how a half-initialised decoder survives into the next run. */
void video_decoder_close(void);

/* The reason for the last failure, in words a person can act on, or "" if
 * nothing has failed. Valid until the next call into this module. */
const char *video_decoder_error(void);

#endif
