/* The Media Engine implementation of media/video_decoder.h.
 *
 * This is the file the project turns on. It reaches sceMpegGetAvcNalAu, an
 * export PSPSDK neither declares nor stubs, through the hand-written import
 * table in src/platform/psp_mpeg_import.S -- and that is what lets the Media
 * Engine take a Jellyfin stream at all, because every other entry point in
 * sceMpeg wants a PSMF container the stream does not have.
 *
 * A WARNING THAT OUTLIVES THIS TASK: development runs on an emulator, and an
 * emulator pass proves that this code RUNS, not that it is right. The
 * emulator does not implement sceMpegGetAvcNalAu at all, so every call below that matters
 * is exercised only on a console. The first hardware run (2026-09-23) switched
 * the PSP off inside that call, because its signature here was a guess; the
 * call sequence below now follows the one working hardware players use (see
 * platform/psp_mpeg_nal.h for where it comes from). Every decode result is
 * still "UNVERIFIED -- REQUIRES REAL PSP TEST" until a console has run it.
 *
 * Only compiled for the console. The host check build (scripts/test.sh)
 * compiles every .c file under src/media, so the guard below keeps this one
 * out of a build that has no PSP headers; the parts of this path that a host
 * CAN test live in media/h264_au.c for exactly that reason. */
#if defined(__PSP__)

#include "media/video_decoder.h"

#include "media/h264_au.h"
#include "media/h264_mmco.h"
#include "platform/av_modules.h"
#include "platform/psp_mpeg_nal.h"
#include "platform/trace.h"
#include "ui/render.h"

#include <pspkernel.h>
#include <pspmodulemgr.h>
#include <pspmpeg.h>
#include <pspsysmem.h>
#include <psputility.h>
#include <psputils.h>
#include <string.h>

/* ---------------------------------------------------------------- limits */

/* One claim, 4 MB, on a 4 MB boundary -- design section 3.3 and
 * docs/PSP1000_BASELINE.md. It is handed whole to sceMpegCreate as the
 * decoder's "ddrtop" working area, and the access-unit buffer sceMpegInitAu
 * binds lives inside it. Nothing else of ours goes there: how the firmware
 * uses the rest of it is not documented anywhere. */
#define VIDEO_BLOCK_SIZE  (4u * 1024u * 1024u)
#define VIDEO_BLOCK_ALIGN (4u * 1024u * 1024u)

/* Where in the block sceMpegInitAu's buffer goes. 1 MB in, from hardware:
 * at 64 kB in, a keyframe overran it into memory the decoder was also
 * using. */
#define VIDEO_AU_OFFSET (1u * 1024u * 1024u)

/* The Media Engine reads and writes these buffers by DMA. 64 bytes is the
 * cache line the writeback calls operate on, so aligning to it means a
 * writeback over one buffer can never share a line with another. */
#define VIDEO_ME_ALIGN 64u

/* sceMpegCreate's and sceMpegQueryMemSize's mode. 4 is what decodes on a
 * PSP-1000 with 6.61 (see VIDEO_NULL_DESTINATION); what the modes select is
 * not documented. */
#ifndef VIDEO_MPEG_MODE
#define VIDEO_MPEG_MODE 4
#endif

/* Decode with NO destination and no sceMpegAvcDecodeMode, then find the YUV
 * planes with sceMpegAvcDecodeDetail2 and convert them with
 * sceMpegBaseCscAvc -- the shape that works on 6.61, together with
 * platform/me_runtime.c and sceMpegCreate mode 4.
 * This is what decoded on hardware (2026-09-24): every picture of a 480x272
 * Baseline fixture, converted with no error. 0 is the 8888-destination,
 * mode-1 path, which refused every frame on this console. */
#ifndef VIDEO_NULL_DESTINATION
#define VIDEO_NULL_DESTINATION 1
#endif

/* sceMpegQueryMemSize's answer is a number from the firmware, and we refuse
 * an absurd one rather than claiming whatever it says. The real figure is
 * tens of kilobytes. */
#define VIDEO_WORKBUF_MAX (256u * 1024u)

/* An access unit is one frame of Constrained Baseline at 480x272. Half a
 * megabyte is far past any real keyframe; it is here so a corrupt length off
 * the network cannot ask us to stage an arbitrary amount of data. */
#define VIDEO_AU_MAX (512u * 1024u)

/* SPS and PPS side by side. Real ones are a few dozen bytes. */
#define VIDEO_PARAMS_MAX 256u

/* 8888 output, 512-pixel rows. The Media Engine is told the same 512 through
 * sceMpegCreate and sceMpegAvcDecode. */
#define VIDEO_STRIDE 512u
#define VIDEO_MAX_W  480u
#define VIDEO_MAX_H  272u

/* The private destination, used only when there is no display to decode
 * towards (render_init skipped): 288 rows for the same macroblock reason as
 * ui/render.c's DECODE_ROWS. */
#define VIDEO_FRAME_ROWS 288u
#define VIDEO_FRAME_SZ   (VIDEO_STRIDE * VIDEO_FRAME_ROWS * 4u)

/* sceMpegAvcDecode writes one picture per slot of an ARRAY of destinations,
 * and an IDR yields several. Handed a single pointer -- as this file used to
 * do -- the firmware takes whatever words follow it as further destinations.
 * Every slot points at the same buffer: only the last picture of an access
 * unit is shown. */
#define VIDEO_MAX_PICTURES 4

/* Wide enough for the longest message here plus " (firmware returned
 * 0xXXXXXXXX)" with room to spare. */
#define VIDEO_ERRLEN 256

/* How many pushes trace every Media Engine call of; see platform/trace.h. */
#define VIDEO_TRACE_PUSHES 6u

/* --------------------------------------------------------------- reasons */

/* Formatted by hand rather than with snprintf. Pulling newlib's printf into
 * this module for the sake of eight hex digits costs code size and can reach
 * the heap on an error path, and the heap is 1 MB and not ours to spend. */
static char g_err[VIDEO_ERRLEN];

static unsigned err_put(unsigned n, const char *s) {
    while (*s && n + 1u < (unsigned)VIDEO_ERRLEN) g_err[n++] = *s++;
    g_err[n] = 0;
    return n;
}

static unsigned err_put_hex(unsigned n, unsigned v) {
    static const char digits[] = "0123456789ABCDEF";
    char              tmp[11];
    int               i;

    tmp[0] = '0';
    tmp[1] = 'x';
    for (i = 0; i < 8; i++) tmp[2 + i] = digits[(v >> (28 - 4 * i)) & 0xFu];
    tmp[10] = 0;
    return err_put(n, tmp);
}

static int fail(int code, const char *msg) {
    err_put(0, msg);
    return code;
}

static int fail_rc(int code, const char *msg, int rc) {
    unsigned n = err_put(0, msg);
    n          = err_put(n, " (firmware returned ");
    n          = err_put_hex(n, (unsigned)rc);
    err_put(n, ")");
    return code;
}

/* ----------------------------------------------------------------- state */

/* Everything is a file static, deliberately. There is one Media Engine and
 * one decoder; making this a handle would imply a second instance is possible
 * and invite an allocation to create it. */

static int    g_open;
static SceUID g_block = -1; /* the 4 MB ddrtop block */
static SceUID g_stage = -1; /* work memory, the access-unit copy and the fallback frame */

static uint8_t *g_ddrtop;   /* the block, 4 MB aligned */
static uint8_t *g_work;     /* sceMpegCreate's working memory */
static uint8_t *g_au_stage; /* the access unit currently being decoded */
static uint8_t *g_frame;    /* the fallback destination, cached RAM */
static uint32_t g_work_size;

/* What video_decoder_next_frame() hands back: where the last picture was
 * copied to on the panel, or the fallback frame. */
static void    *g_out_pixels;
static uint16_t g_out_stride;

static SceMpeg           g_mpeg __attribute__((aligned(VIDEO_ME_ALIGN)));
static SceMpegRingbuffer g_ringbuffer __attribute__((aligned(VIDEO_ME_ALIGN)));

/* The AU descriptor, backed by a buffer well past SceMpegAu's size: the
 * firmware reads and writes fields PSPSDK's reconstruction of the struct does
 * not name. A union rather than a cast so the descriptor is accessed as its
 * own type. */
static union {
    SceMpegAu au;
    uint8_t   raw[128];
} g_au_store __attribute__((aligned(VIDEO_ME_ALIGN)));

/* SPS immediately followed by PPS, written once at open. */
static uint8_t  g_params[VIDEO_PARAMS_MAX] __attribute__((aligned(VIDEO_ME_ALIGN)));
static uint16_t g_sps_len, g_pps_len;

static uint16_t g_width, g_height;
static uint8_t  g_nal_length_size;

/* The slice-header layout of this stream, and whether its redundant MMCO is
 * rewritten on the way in -- media/h264_mmco.h, and docs/RESEARCH.md section
 * 10 for why: the Media Engine refused every P-frame that carried it. */
static h264_slice_params g_slice;
static int               g_rewrite_mmco;
static int      g_first; /* no picture yet: sceMpegGetAvcNalAu wants its "first" mode */
static int      g_frame_ready;
static uint64_t g_frame_dts;
static unsigned g_trace_pushes;

static int g_mpeg_inited, g_mpeg_created;

/* ------------------------------------------------------------- the claims */

static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1u) & ~(a - 1u); }

/* Claims the 4 MB block on a 4 MB boundary.
 *
 * sceKernelAllocPartitionMemory has no aligned mode in this SDK -- the block
 * types are Low, High and Addr, and none of them takes an alignment -- so the
 * alignment has to be produced rather than requested.
 *
 * PSP_SMEM_Low places the block at the lowest free address, which makes the
 * placement deterministic and lets us do this cheaply: claim 4 MB, look at
 * where it landed, and if it is already aligned (it usually is, early in a
 * fresh partition) keep it. Otherwise free it, work out the padding to the
 * next boundary, and claim 4 MB plus exactly that much -- the same lowest
 * free address, so base + pad is the boundary we wanted, and the waste is the
 * padding rather than a whole extra 4 MB.
 *
 * That relies on nothing else allocating between the two calls, which holds
 * because open() runs before any worker thread exists. It is checked anyway,
 * and a last attempt over-allocates by a full alignment so it cannot matter
 * where the block lands. */
static int claim_block(uint8_t **out_base) {
    SceUID   uid;
    uint8_t *head;
    uint32_t pad;

    uid = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER, "hexfin_video", PSP_SMEM_Low, VIDEO_BLOCK_SIZE,
                                        NULL);
    if (uid < 0)
        return fail_rc(VIDEO_ERR_MEMORY, "the 4 MB decoder block could not be claimed from the partition", uid);

    head = (uint8_t *)sceKernelGetBlockHeadAddr(uid);
    if (!head) {
        sceKernelFreePartitionMemory(uid);
        return fail(VIDEO_ERR_MEMORY, "the decoder block has no address");
    }

    if (((uintptr_t)head & (VIDEO_BLOCK_ALIGN - 1u)) == 0) {
        g_block   = uid;
        *out_base = head;
        return VIDEO_OK;
    }

    pad = (uint32_t)((VIDEO_BLOCK_ALIGN - ((uintptr_t)head & (VIDEO_BLOCK_ALIGN - 1u))) & (VIDEO_BLOCK_ALIGN - 1u));
    sceKernelFreePartitionMemory(uid);

    uid = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER, "hexfin_video", PSP_SMEM_Low,
                                        VIDEO_BLOCK_SIZE + pad, NULL);
    if (uid >= 0) {
        head = (uint8_t *)sceKernelGetBlockHeadAddr(uid);
        if (head && ((uintptr_t)(head + pad) & (VIDEO_BLOCK_ALIGN - 1u)) == 0) {
            g_block   = uid;
            *out_base = head + pad;
            return VIDEO_OK;
        }
        sceKernelFreePartitionMemory(uid);
    }

    /* Something moved underneath us. Pay for a whole alignment's worth of
     * slack and align inside it, which cannot fail for placement reasons. */
    uid = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER, "hexfin_video", PSP_SMEM_Low,
                                        VIDEO_BLOCK_SIZE + VIDEO_BLOCK_ALIGN, NULL);
    if (uid < 0)
        return fail_rc(VIDEO_ERR_MEMORY, "no 4 MB-aligned run is free in the partition for the decoder block", uid);

    head = (uint8_t *)sceKernelGetBlockHeadAddr(uid);
    if (!head) {
        sceKernelFreePartitionMemory(uid);
        return fail(VIDEO_ERR_MEMORY, "the decoder block has no address");
    }

    g_block   = uid;
    *out_base = (uint8_t *)(((uintptr_t)head + (VIDEO_BLOCK_ALIGN - 1u)) & ~(uintptr_t)(VIDEO_BLOCK_ALIGN - 1u));
    return VIDEO_OK;
}

/* The second, smaller claim: sceMpegCreate's work memory, the staging copy of
 * the access unit and the fallback frame, one after another on cache lines.
 * Separate from the 4 MB block because that block is the firmware's. */
static int claim_stage(void) {
    uint32_t size = align_up(g_work_size, VIDEO_ME_ALIGN) + VIDEO_AU_MAX + VIDEO_FRAME_SZ + VIDEO_ME_ALIGN;
    uint8_t *head;
    SceUID   uid;

    uid = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER, "hexfin_vstage", PSP_SMEM_Low, size, NULL);
    if (uid < 0) return fail_rc(VIDEO_ERR_MEMORY, "the decoder's working memory could not be claimed", uid);
    g_stage = uid;

    head = (uint8_t *)sceKernelGetBlockHeadAddr(uid);
    if (!head) return fail(VIDEO_ERR_MEMORY, "the decoder's working memory has no address");

    head       = (uint8_t *)(((uintptr_t)head + VIDEO_ME_ALIGN - 1u) & ~(uintptr_t)(VIDEO_ME_ALIGN - 1u));
    g_work     = head;
    g_au_stage = g_work + align_up(g_work_size, VIDEO_ME_ALIGN);
    g_frame    = g_au_stage + VIDEO_AU_MAX;
    return VIDEO_OK;
}

/* ------------------------------------------------------------------ open */

int video_decoder_open(const uint8_t *sps, uint16_t sps_len, const uint8_t *pps, uint16_t pps_len,
                       uint8_t nal_length_size, int width, int height) {
    int            rc;
    SceMpegAvcMode mode;

    g_err[0] = 0;

    if (g_open) return fail(VIDEO_ERR_STATE, "the decoder is already open");
    if (!sps || !pps || sps_len == 0 || pps_len == 0)
        return fail(VIDEO_ERR_ARG, "the stream arrived without parameter sets");
    if (width <= 0 || height <= 0 || (uint32_t)width > VIDEO_MAX_W || (uint32_t)height > VIDEO_MAX_H)
        return fail(VIDEO_ERR_UNSUPPORTED, "the picture is larger than the PSP's display");
    if (nal_length_size != 1 && nal_length_size != 2 && nal_length_size != 4)
        return fail(VIDEO_ERR_UNSUPPORTED, "the stream's NAL length prefix is not 1, 2 or 4 bytes wide");
    if ((uint32_t)sps_len + pps_len > VIDEO_PARAMS_MAX)
        return fail(VIDEO_ERR_BAD_STREAM, "the stream's SPS/PPS are larger than any real parameter set");

    /* The Media Engine reads these directly and does NOT snoop the CPU cache.
     * They are written once, so one writeback covers the whole run. */
    memcpy(g_params, sps, sps_len);
    memcpy(g_params + sps_len, pps, pps_len);
    g_sps_len = sps_len;
    g_pps_len = pps_len;
    sceKernelDcacheWritebackRange(g_params, align_up((uint32_t)sps_len + pps_len, VIDEO_ME_ALIGN));
    trace("video: sps %u bytes %02X %02X %02X %02X, pps %u bytes %02X, nal prefix %u", (unsigned)sps_len, sps[0],
          sps_len > 1 ? sps[1] : 0, sps_len > 2 ? sps[2] : 0, sps_len > 3 ? sps[3] : 0, (unsigned)pps_len, pps[0],
          (unsigned)nal_length_size);

    /* AVCODEC before MPEGBASE. The order is not a style choice: MPEGBASE
     * depends on AVCODEC and loading it first fails.
     *
     * Through av_module_acquire rather than sceUtilityLoadAvModule directly,
     * and with no local "have I loaded it" flag: AVCODEC is shared with
     * audio_psp.c. See platform/av_modules.h. */
    rc = av_module_acquire(PSP_AV_MODULE_AVCODEC);
    if (rc < 0) return fail_rc(VIDEO_ERR_DRIVER, "the video codec module would not load", rc);

    /* Not fatal: with platform/me_runtime.c's mpeg_vsh already providing
     * sceMpeg, this module's own copy of that library cannot register again
     * and the load may say so -- while sceMpegbase, which the colour
     * conversion needs, is there regardless. sceMpegInit below is the real
     * test either way. */
    rc = av_module_acquire(PSP_AV_MODULE_MPEGBASE);
    if (rc < 0) trace("video: MPEGBASE module load -> 0x%08X, carrying on", (unsigned)rc);

    /* Once more after a Finish if it refuses: a previous run that exited
     * without closing leaves sceMpegInit failing until it is finished. */
    trace("video: modules loaded, sceMpegInit");
    rc = sceMpegInit();
    if (rc < 0) {
        sceMpegFinish();
        rc = sceMpegInit();
    }
    trace("video: sceMpegInit -> 0x%08X", (unsigned)rc);
    if (rc < 0) {
        video_decoder_close();
        return fail_rc(VIDEO_ERR_DRIVER, "the Media Engine would not initialise", rc);
    }
    g_mpeg_inited = 1;

    rc = sceMpegQueryMemSize(VIDEO_MPEG_MODE);
    if (rc <= 0) {
        video_decoder_close();
        return fail_rc(VIDEO_ERR_DRIVER, "the Media Engine would not say how much memory it needs", rc);
    }
    if ((uint32_t)rc > VIDEO_WORKBUF_MAX) {
        video_decoder_close();
        return fail_rc(VIDEO_ERR_MEMORY, "the Media Engine asked for more working memory than we will give it", rc);
    }
    g_work_size = (uint32_t)rc;

    /* The 4 MB block first: design section 3.3 claims the largest block
     * before anything smaller can fragment the partition under it. */
    if (claim_block(&g_ddrtop) != VIDEO_OK || claim_stage() != VIDEO_OK) {
        /* The claim already wrote the reason; close() must not clear it. */
        video_decoder_close();
        return VIDEO_ERR_MEMORY;
    }
    memset(g_frame, 0, VIDEO_FRAME_SZ);
    sceKernelDcacheWritebackInvalidateRange(g_frame, VIDEO_FRAME_SZ);

    /* No ringbuffer is constructed -- it is the PSMF demuxer's input, and the
     * NAL path has no PSMF. Passed zeroed rather than NULL, as the hardware
     * players do. */
    memset(&g_ringbuffer, 0, sizeof(g_ringbuffer));

    trace("video: sceMpegCreate work=%p size=%u ddrtop=%p", (void *)g_work, (unsigned)g_work_size, (void *)g_ddrtop);
    rc = sceMpegCreate(&g_mpeg, g_work, (SceInt32)g_work_size, &g_ringbuffer, (SceInt32)VIDEO_STRIDE,
                       VIDEO_MPEG_MODE, (SceInt32)(uintptr_t)g_ddrtop);
    trace("video: sceMpegCreate -> 0x%08X", (unsigned)rc);
    if (rc < 0) {
        video_decoder_close();
        return fail_rc(VIDEO_ERR_DRIVER, "the Media Engine refused to create a decoder", rc);
    }
    g_mpeg_created = 1;

    /* 0xFF rather than 0 before binding, as the hardware players do; the
     * firmware fills in only the ES buffer fields. The descriptor has two
     * writers, us and the firmware, so it is written back AND invalidated on
     * both sides of every call that touches it -- a plain writeback leaves
     * our stale lines resident to be written back over the firmware's. */
    memset(&g_au_store, 0xFF, sizeof(g_au_store));
    sceKernelDcacheWritebackInvalidateRange(&g_au_store, sizeof(g_au_store));
    rc = sceMpegInitAu(&g_mpeg, g_ddrtop + (VIDEO_NULL_DESTINATION ? 0x10000u : VIDEO_AU_OFFSET), &g_au_store.au);
    sceKernelDcacheWritebackInvalidateRange(&g_au_store, sizeof(g_au_store));
    trace("video: sceMpegInitAu -> 0x%08X", (unsigned)rc);
    if (rc < 0) {
        video_decoder_close();
        return fail_rc(VIDEO_ERR_DRIVER, "the Media Engine would not accept an access unit buffer", rc);
    }

    /* 8888 out, the format proven on hardware. RGB565 -- design section
     * 3.2's hope -- would halve the copy to the panel, and is worth trying
     * once this path works at all. iUnk0 is documented only as "set to -1". */
    mode.iUnk0        = -1;
    mode.iPixelFormat = SCE_MPEG_AVC_FORMAT_8888;
    rc                = VIDEO_NULL_DESTINATION ? 0 : sceMpegAvcDecodeMode(&g_mpeg, &mode);
    trace("video: sceMpegAvcDecodeMode -> 0x%08X%s", (unsigned)rc, VIDEO_NULL_DESTINATION ? " (skipped)" : "");
    if (rc < 0) {
        video_decoder_close();
        return fail_rc(VIDEO_ERR_DRIVER, "the Media Engine would not output 8888 pixels", rc);
    }

    g_width           = (uint16_t)width;
    g_height          = (uint16_t)height;
    g_nal_length_size = nal_length_size;
    {
        h264_err pe    = h264_slice_params_parse(sps, sps_len, pps, pps_len, &g_slice);
        g_rewrite_mmco = (pe == H264_OK && h264_mmco_rewrite_applies(&g_slice));
        trace("video: slice params -> %d, max_num_ref_frames %u, MMCO rewrite %s", (int)pe,
              (unsigned)g_slice.max_num_ref_frames, g_rewrite_mmco ? "on" : "off");
    }
    g_first           = 1;
    g_frame_ready     = 0;
    g_frame_dts       = 0;
    g_out_pixels      = g_frame;
    g_out_stride      = (uint16_t)VIDEO_STRIDE;
    g_open            = 1;

    return VIDEO_OK;
}

/* ------------------------------------------------------------------ push */

/* The picture the engine just wrote, centred onto the panel's back buffer.
 * Both sides are uncached VRAM, so there is no cache to manage. Returns where
 * it went, or NULL when there is no panel to put it on. */
static void *copy_to_panel(const uint32_t *src, int src_stride) {
    void     *dst_v = NULL;
    int       dst_stride;
    uint32_t *dst;
    int       row;

    if (render_target((int)g_width, (int)g_height, &dst_v, &dst_stride) != 0) return NULL;
    dst = (uint32_t *)dst_v;
    for (row = 0; row < (int)g_height; row++)
        memcpy(dst + (uint32_t)row * (uint32_t)dst_stride, src + (uint32_t)row * (uint32_t)src_stride,
               (size_t)g_width * 4u);
    g_out_stride = (uint16_t)dst_stride;
    return dst;
}

int video_decoder_push(const uint8_t *au, uint32_t size, uint64_t dts, int show) {
    /* Static, not on the stack: the Media Engine reads it by DMA, so it must
     * sit on its own cache line and be written back like everything else. */
    static psp_mpeg_avc_nal nal __attribute__((aligned(VIDEO_ME_ALIGN)));
    void                   *dest[VIDEO_MAX_PICTURES];
    void                   *target;
    int                     target_stride, target_rows, in_vram, k, rc;
    SceInt32                pictures = 0;
    h264_err                he;

    g_err[0] = 0;

    if (!g_open) return fail(VIDEO_ERR_STATE, "the decoder is not open");
    if (!au || size == 0) return fail(VIDEO_ERR_ARG, "an empty access unit was pushed");
    if (size > VIDEO_AU_MAX) return fail(VIDEO_ERR_BAD_STREAM, "an access unit claims an implausible size");

    /* Framing checked before the bytes go anywhere near the Media Engine.
     * The ME reads this buffer by DMA with no bounds check of its own, so a
     * length prefix that overruns is a wild read on hardware -- and this
     * access unit came off a network from a server we do not control
     * (PROMPT.md section 35). */
    he = h264_au_check(au, size, g_nal_length_size, NULL);
    if (he == H264_ERR_TRUNCATED)
        return fail(VIDEO_ERR_BAD_STREAM, "an access unit's NAL length runs past the end of the frame");
    if (he != H264_OK) return fail(VIDEO_ERR_BAD_STREAM, "an access unit does not frame as length-prefixed NALs");

    /* Copied into the staging buffer rather than handed over where it lies:
     * the caller's slice points into a streaming read buffer whose alignment
     * is whatever the byte offset happened to be, and whose contents may be
     * overwritten the moment this returns. Rewritten on the way when the
     * stream carries the MMCO the engine refuses; if the rewrite cannot be
     * done for any reason, the original goes instead -- it will fail as it
     * always did, which is no worse. Then written back, because the ME does
     * not snoop the cache. */
    {
        uint32_t staged = 0, rewritten = 0;

        if (g_rewrite_mmco &&
            h264_au_drop_redundant_mmco(&g_slice, au, size, g_nal_length_size, g_au_stage, VIDEO_AU_MAX, &staged,
                                        &rewritten) == H264_OK) {
            size = staged;
        } else {
            memcpy(g_au_stage, au, size);
        }
        if (g_trace_pushes < VIDEO_TRACE_PUSHES) trace("video: %u slice(s) had their MMCO rewritten", (unsigned)rewritten);
    }
    sceKernelDcacheWritebackRange(g_au_stage, align_up(size, VIDEO_ME_ALIGN));

    nal.sps             = g_params;
    nal.sps_size        = g_sps_len;
    nal.pps             = g_params + g_sps_len;
    nal.pps_size        = g_pps_len;
    nal.nal_prefix_size = g_nal_length_size;
    nal.nal             = g_au_stage;
    nal.nal_size        = (SceInt32)size;
    nal.mode            = g_first ? PSP_MPEG_NAL_MODE_FIRST : PSP_MPEG_NAL_MODE_NEXT;
    sceKernelDcacheWritebackRange(&nal, sizeof(nal));

    /* Timestamps unknown to the engine, as the hardware players pass them;
     * the DTS this probe cares about is kept on our side. */
    g_au_store.au.iPtsMSB = 0xFFFFFFFFu;
    g_au_store.au.iPts    = 0xFFFFFFFFu;
    g_au_store.au.iDtsMSB = 0xFFFFFFFFu;
    g_au_store.au.iDts    = 0xFFFFFFFFu;
    sceKernelDcacheWritebackInvalidateRange(&g_au_store, sizeof(g_au_store));

    if (g_trace_pushes < VIDEO_TRACE_PUSHES)
        trace("video: push %u size %u mode %d, sceMpegGetAvcNalAu", g_trace_pushes, (unsigned)size, (int)nal.mode);
    rc = sceMpegGetAvcNalAu(&g_mpeg, &nal, &g_au_store.au);
    if (g_trace_pushes < VIDEO_TRACE_PUSHES) trace("video: sceMpegGetAvcNalAu -> 0x%08X", (unsigned)rc);
    sceKernelDcacheWritebackInvalidateRange(&g_au_store, sizeof(g_au_store));
    if (rc < 0) return fail_rc(VIDEO_ERR_DRIVER, "the Media Engine rejected the access unit", rc);

    /* The engine's destination: its own VRAM buffer when there is a display,
     * the private frame when there is not. */
    in_vram = (render_decode_target(&target, &target_stride, &target_rows) == 0);
    if (!in_vram) {
        target        = g_frame;
        target_stride = (int)VIDEO_STRIDE;
        sceKernelDcacheWritebackInvalidateRange(g_frame, VIDEO_FRAME_SZ);
    }
    for (k = 0; k < VIDEO_MAX_PICTURES; k++) dest[k] = target;

    if (g_trace_pushes < VIDEO_TRACE_PUSHES)
        trace("video: sceMpegAvcDecode into %s %p stride %d", in_vram ? "VRAM" : "RAM", target, target_stride);
    rc = sceMpegAvcDecode(&g_mpeg, &g_au_store.au, (SceInt32)target_stride, VIDEO_NULL_DESTINATION ? NULL : dest,
                          &pictures);
    if (g_trace_pushes < VIDEO_TRACE_PUSHES)
        trace("video: sceMpegAvcDecode -> 0x%08X pictures %d", (unsigned)rc, (int)pictures);
    g_trace_pushes++;
    sceKernelDcacheWritebackInvalidateRange(&g_au_store, sizeof(g_au_store));
    if (rc < 0) {
        /* The one error worth naming, but named carefully. B-slices are the
         * likeliest cause and the one the user can act on -- the Media Engine
         * cannot decode them. It is not the only cause, so the message
         * offers it as the first thing to check and carries the raw code for
         * everything else. */
        if ((unsigned)rc == SCE_MPEG_ERROR_AVC_DECODE_FATAL)
            return fail_rc(VIDEO_ERR_UNSUPPORTED,
                           "the decoder could not produce a picture; the usual cause is B-frames, "
                           "which the PSP cannot decode -- the server must send Baseline or "
                           "Constrained Baseline",
                           rc);
        return fail_rc(VIDEO_ERR_DRIVER, "the Media Engine failed to decode a frame", rc);
    }

    /* No picture is legitimate: the decoder may hold an access unit. */
    if (pictures <= 0) return VIDEO_OK;
    g_first = 0;
    if (!show) return VIDEO_OK; /* decoded for reference only */

    if (VIDEO_NULL_DESTINATION) {
        /* The picture exists only as YUV in the engine's memory. Ask where,
         * and convert it into the destination chosen above. */
        psp_mpeg_avc_detail *detail = NULL;
        psp_mpeg_avc_csc     csc __attribute__((aligned(VIDEO_ME_ALIGN)));

        rc = sceMpegAvcDecodeDetail2(&g_mpeg, &detail);
        if (g_trace_pushes <= VIDEO_TRACE_PUSHES)
            trace("video: sceMpegAvcDecodeDetail2 -> 0x%08X detail %p picture %p yuv %p %dx%d", (unsigned)rc,
                  (void *)detail, detail ? (void *)detail->picture : NULL, detail ? (void *)detail->yuv : NULL,
                  detail && detail->picture ? (int)detail->picture->width : -1,
                  detail && detail->picture ? (int)detail->picture->height : -1);
        if (rc < 0 || !detail || !detail->picture || !detail->yuv)
            return fail_rc(VIDEO_ERR_DRIVER, "the Media Engine decoded a picture but would not say where", rc);

        memset(&csc, 0, sizeof csc);
        csc.height_blocks = (detail->picture->height + 15) >> 4;
        csc.width_blocks  = (detail->picture->width + 15) >> 4;
        memcpy(csc.plane, detail->yuv->plane, sizeof csc.plane);
        if (csc.height_blocks * 16 > (int)VIDEO_FRAME_ROWS || csc.width_blocks * 16 > (int)VIDEO_STRIDE)
            return fail(VIDEO_ERR_UNSUPPORTED, "the decoded picture is larger than the conversion buffer");

        if (!in_vram) sceKernelDcacheWritebackInvalidateRange(g_frame, VIDEO_FRAME_SZ);
        rc = sceMpegBaseCscAvc(target, 0, (SceUInt32)target_stride, &csc);
        if (g_trace_pushes <= VIDEO_TRACE_PUSHES) trace("video: sceMpegBaseCscAvc -> 0x%08X", (unsigned)rc);
        if (rc < 0) return fail_rc(VIDEO_ERR_DRIVER, "the Media Engine would not convert the picture", rc);
    }

    if (in_vram) {
        g_out_pixels = copy_to_panel((const uint32_t *)target, target_stride);
        if (!g_out_pixels) return VIDEO_OK; /* decoded, but there is no panel to show it on */
    } else {
        /* The ME wrote behind the cache's back; invalidate before any read. */
        sceKernelDcacheWritebackInvalidateRange(g_frame, VIDEO_FRAME_SZ);
        g_out_pixels = g_frame;
        g_out_stride = (uint16_t)VIDEO_STRIDE;
    }

    g_frame_dts   = dts;
    g_frame_ready = 1;
    return VIDEO_OK;
}

/* ------------------------------------------------------------ next frame */

int video_decoder_next_frame(video_frame *out) {
    g_err[0] = 0;

    if (!g_open) return fail(VIDEO_ERR_STATE, "the decoder is not open");
    if (!out) return fail(VIDEO_ERR_ARG, "no frame to fill was given");

    /* A push that produced no picture leaves nothing here, which is the
     * AGAIN case: the decoder may hold an access unit before it yields. */
    if (!g_frame_ready) return VIDEO_ERR_AGAIN;

    out->pixels = g_out_pixels;
    out->stride = g_out_stride;
    out->width  = g_width;
    out->height = g_height;
    out->dts    = g_frame_dts;

    g_frame_ready = 0;
    return VIDEO_OK;
}

/* ----------------------------------------------------------------- close */

void video_decoder_close(void) {
    /* Unwound in the reverse of the order open() built it, and every step is
     * guarded by its own flag, because this runs on the failure path too.
     * Design section 3.4: exiting underneath a live sceMpeg instance is a
     * classic cause of the black screen on HOME.
     *
     * No sceMpegAvcDecodeStop. It takes the same array of destinations as
     * sceMpegAvcDecode, this file used to hand it a single pointer, and the
     * hardware players that run for hours do not call it at all.
     *
     * It deliberately does not touch g_err: a caller that closes after a
     * failure still needs the reason that failure left behind. */
    if (g_mpeg_created) {
        sceMpegDelete(&g_mpeg);
        g_mpeg_created = 0;
    }

    if (g_mpeg_inited) {
        sceMpegFinish();
        g_mpeg_inited = 0;
    }

    if (g_stage >= 0) {
        sceKernelFreePartitionMemory(g_stage);
        g_stage = -1;
    }
    if (g_block >= 0) {
        sceKernelFreePartitionMemory(g_block);
        g_block = -1;
    }

    /* The AV modules are left loaded. Unloading them is the documented way
     * to get their memory back, but it is also a step that can fail while
     * the decoder is being torn down after an error, and design section 3.4
     * wants the teardown path to be the one thing that cannot itself break.
     * See platform/av_modules.h. */

    g_ddrtop      = NULL;
    g_work        = NULL;
    g_au_stage    = NULL;
    g_frame       = NULL;
    g_out_pixels  = NULL;
    g_out_stride  = 0;
    g_work_size   = 0;
    g_frame_ready = 0;
    g_first       = 0;
    g_open        = 0;
}

const char *video_decoder_error(void) { return g_err; }

#else

/* The host check build compiles every .c file under src/media. There is no
 * Media Engine here and nothing to stand in for one, so this file contributes
 * nothing -- but an empty translation unit is not strictly legal C, hence the
 * typedef. */
typedef int video_psp_needs_a_psp;

#endif /* __PSP__ */
