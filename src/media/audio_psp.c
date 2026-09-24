/* The Media Engine implementation of media/audio_decoder.h, decoding AAC-LC
 * through sceAudiocodec the way video_psp.c decodes H.264 through sceMpeg.
 *
 * ============================================================================
 * FRAMING: raw AAC frames, no ADTS -- settled on hardware, 2026-09-23.
 * ============================================================================
 *
 * fmp4 hands back bare AAC access units: raw_data_block payloads with no
 * ADTS syncword, because the configuration travelled once already, in the
 * esds box's AudioSpecificConfig (see media/aac_config.h). sceAudiocodecDecode
 * takes no per-frame length, and its MP3 path finds each frame by its own
 * sync word, so this file first synthesised a 7-byte ADTS header per frame.
 * On a real PSP-1000 that decoded to nothing: rc 0, err 0, no PCM. What
 * decodes is the raw frame, zero-padded to the input stage size, with the
 * sizes set in the control block before the call (see audio_decoder_push).
 *
 * ============================================================================
 * THE CONTROL BLOCK
 * ============================================================================
 *
 * PSPSDK declares no struct for sceAudiocodec's 128-byte control block, only
 * the pointer. The layout below, `audio_ctx`, is common to every codec
 * sceAudiocodec drives. Confidence, field by field:
 *
 *   HIGH   -- magic, err, in_buf, in_size, out_buf, out_size, needed_mem,
 *             edram_addr, alloc_mem: consistent across every codec the
 *             firmware drives this way. 0x1c and 0x24 are documented as
 *             "bytes read" and "bytes written"; on hardware (2026-09-23)
 *             decoding only worked with 0x1c SET to the input size and 0x24
 *             SET to the output capacity before the call.
 *   MEDIUM -- sample_rate at 0x28: observed rather than documented.
 *   LOW    -- the two bytes at 0x2c/0x2d, believed to select which of two
 *             fixed input/output size assumptions the firmware uses. This
 *             file leaves them at 0 -- see AUDIO_INPUT_STAGE_CAP below for
 *             how that choice is made safe if it is wrong.
 *   NONE   -- whether `magic` must be rewritten before every call. Writing
 *             it costs four bytes, so it is written defensively.
 *   NONE   -- whether a nonzero `err` after a call that returned rc >= 0 is
 *             a failure the return code alone would miss. This file checks
 *             both.
 *
 * _Static_assert guards each offset below against a transcription slip. */
#if defined(__PSP__)

#include "media/audio_decoder.h"

#include "media/aac_config.h"
#include "platform/av_modules.h"
#include "platform/trace.h"

#include <pspaudio.h>
#include <pspaudiocodec.h>
#include <pspkernel.h>
#include <psputility.h>
#include <stddef.h>
#include <string.h>

/* ---------------------------------------------------------------- limits */

/* The Media Engine (and, per this file's header comment, possibly the audio
 * hardware's own DMA too) reads and writes these buffers directly. 64 bytes
 * is the cache line the writeback calls operate on. */
#define AUDIO_ME_ALIGN 64u

/* avcodec.prx is documented (see header comment) to assume one of two fixed
 * upper bounds for how much of the input buffer it is safe to read for an
 * AAC frame: 0x600 or 0x609 bytes, chosen by a control-block byte this file
 * leaves at 0. Sizing the actual staging buffer to the LARGER of the two,
 * regardless of which one gets selected, is what makes that byte's
 * uncertain meaning a non-issue: whichever upper bound the firmware
 * actually uses, it is still within a buffer this size, so there is no
 * out-of-bounds read on our side no matter which assumption turns out to be
 * true, or whether this file's understanding of the byte is even right at
 * all. */
#define AUDIO_INPUT_STAGE_CAP 0x609u

/* The matching upper bound for decoded output: unk2d left at 0 (like unk2c
 * above) is documented as selecting the SMALLER of two assumed output
 * sizes, 0x1000 bytes -- which is exactly one AAC-LC frame's worth of
 * stereo 16-bit PCM (1024 samples * 2 channels * 2 bytes = 0x1000), so it is
 * also the natural size regardless of how much trust that byte deserves. */
#define AUDIO_DECODE_OUT_BYTES 0x1000u

/* sceAudiocodecCheckNeedMem's answer, like sceMpegQueryMemSize's in
 * video_psp.c, is a number from the firmware and is bounded before being
 * trusted. The figure expected for AAC is 0x18f20 (~98.5 KB); this ceiling
 * is well past that with room for firmware to disagree without this file
 * handing an unbounded number to anything. */
#define AUDIO_NEEDED_MEM_MAX (256u * 1024u)

/* AAC-LC decodes a fixed 1024 samples per frame, already a multiple of 64 --
 * see PSP_AUDIO_SAMPLE_ALIGN in pspaudio.h -- so the channel is reserved for
 * exactly that many samples per call rather than some other round number.
 * One decoded frame is one sceAudioOutputBlocking call; there is no
 * remainder to reconcile. */
#define AUDIO_OUT_CHUNK_FRAMES 1024u

/* The ring's capacity: PROMPT.md section 24 wants 100-300 ms buffered, not
 * seconds. At 44.1 kHz stereo, 12288 frames is ~278.6 ms and 48 KB -- the top
 * of that window, because the ring is now all that stands between the output
 * thread and silence while the main thread is busy decoding and presenting a
 * video frame (a vblank wait alone is up to 16.7 ms). A whole number of
 * AUDIO_OUT_CHUNK_FRAMES-sized reads (12) with nothing left over. */
#define AUDIO_RING_CAPACITY_FRAMES (12u * AUDIO_OUT_CHUNK_FRAMES)

/* The output thread: above the main thread (0x20), so a decode never keeps
 * the hardware waiting for its next chunk; its stack holds nothing but the
 * loop's locals. */
#define AUDIO_THREAD_PRIO  0x12
#define AUDIO_THREAD_STACK 0x4000

/* When the ring has less than a chunk, the thread waits this long before
 * looking again. A chunk is 23.2 ms of sound. */
#define AUDIO_IDLE_US 2000

/* How long close() waits for the output thread to leave before deleting it
 * anyway. It only ever blocks for one chunk. */
#define AUDIO_THREAD_EXIT_US 500000

/* This decoder only takes stereo. Real firmware's AAC output-size byte
 * (unk2d, above) is only confirmed against a stereo frame's byte count;
 * whether a mono stream halves that, matches it, or something else needs a
 * console to answer, and guessing at ONE MORE unverified number on top of
 * everything already in this file's header comment is where this project
 * chooses to stop rather than compound them. */
#define AUDIO_SUPPORTED_CHANNELS 2u

#define AUDIO_ERRLEN 192

/* --------------------------------------------------------------- reasons */

/* Formatted by hand, not with snprintf -- see video_psp.c's identical
 * helpers and their reasoning; duplicated here rather than shared because
 * this project has no common home for them yet (PROMPT.md section 47 wants
 * the words, not a particular file structure for producing them). */
static char g_err[AUDIO_ERRLEN];

static unsigned err_put(unsigned n, const char *s) {
    while (*s && n + 1u < (unsigned)AUDIO_ERRLEN) g_err[n++] = *s++;
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

/* For a call that SUCCEEDED but wrote a value we will not accept. The value
 * is the whole diagnosis -- "implausible" on its own leaves the reader unable
 * to tell a firmware that asked for 8 MB from one that wrote nothing at all,
 * and those need opposite responses. */
static int fail_val(int code, const char *msg, unsigned value) {
    unsigned n = err_put(0, msg);
    n          = err_put(n, " (it asked for ");
    n          = err_put_hex(n, value);
    err_put(n, " bytes)");
    return code;
}

/* ----------------------------------------------------------- the context */

/* See this file's header comment for provenance and confidence per field.
 * A union over three views of the same 128 bytes: named fields for the ones
 * this file touches, a word array because sceAudiocodec's own prototypes
 * take `unsigned long *`, and a byte array for the memset/sizeof below. */
typedef union {
    struct {
        uint32_t magic;             /* 0x00 -- written before every call, see header comment */
        int32_t  unk4;              /* 0x04 */
        int32_t  err;               /* 0x08 -- the firmware's OWN error signal; checked alongside rc */
        uint32_t edram_addr;        /* 0x0c */
        int32_t  needed_mem;        /* 0x10 -- sceAudiocodecCheckNeedMem's answer */
        int32_t  inited;            /* 0x14 */
        uint32_t in_buf;            /* 0x18 */
        int32_t  in_size;           /* 0x1c -- input bytes, written by us (see push) */
        uint32_t out_buf;           /* 0x20 */
        int32_t  out_size;          /* 0x24 -- output capacity in, bytes written out */
        uint32_t sample_rate;       /* 0x28 -- AAC-specific; see header comment's MEDIUM confidence note */
        uint8_t  reserved_aac[0x68 - 0x2c]; /* 0x2c..0x67: left at 0, see header comment */
        uint32_t alloc_mem;                 /* 0x68 -- our own GetEDRAM's scratch, not read back by us */
        uint8_t  tail[0x80 - 0x6c];         /* 0x6c..0x7f: untouched padding out to 128 bytes */
    } f;
    uint32_t words[32];
    uint8_t  bytes[128];
} audio_ctx;

/* Checked against the byte offsets above: if a compiler for some target
 * ever pads this union differently, this fails the PSP build loudly instead
 * of miscompiling a control block silently. */
_Static_assert(sizeof(audio_ctx) == 128, "sceAudiocodec control block must be exactly 128 bytes");
_Static_assert(offsetof(audio_ctx, f.in_buf) == 0x18, "in_buf offset");
_Static_assert(offsetof(audio_ctx, f.out_buf) == 0x20, "out_buf offset");
_Static_assert(offsetof(audio_ctx, f.sample_rate) == 0x28, "sample_rate offset");
_Static_assert(offsetof(audio_ctx, f.alloc_mem) == 0x68, "alloc_mem offset");

/* The value avcodec.prx is documented to expect in `magic` ahead of a call. */
#define AUDIO_CTX_MAGIC 0x05100601u

/* ----------------------------------------------------------------- state */

/* File statics, deliberately, for the same reason video_psp.c gives: one
 * Media Engine, one audio decoder, and a handle would only invite a second
 * instance nothing here is built to support. */

static int g_open;

static audio_ctx g_ctx __attribute__((aligned(AUDIO_ME_ALIGN)));

/* The copy of whatever audio_decoder_push was handed, zero-padded to the
 * size the firmware reads. Sized per
 * AUDIO_INPUT_STAGE_CAP's reasoning above, not per any particular frame's
 * real length. A file-scope array carrying this attribute is placed at that
 * alignment by the linker, unlike video_psp.c's video buffers, which need
 * their alignment computed at runtime because they are carved out of a
 * dynamically claimed partition block rather than declared like this. */
static uint8_t g_in_stage[AUDIO_INPUT_STAGE_CAP] __attribute__((aligned(AUDIO_ME_ALIGN)));

/* Where sceAudiocodecDecode lands one frame's PCM -- 1024 samples, stereo,
 * 16-bit, exactly AUDIO_DECODE_OUT_BYTES. Copied into the ring immediately
 * (pcm_ring_write), so this buffer is only ever live between one
 * sceAudiocodecDecode call and the copy right after it. */
static uint8_t g_decode_out[AUDIO_DECODE_OUT_BYTES] __attribute__((aligned(AUDIO_ME_ALIGN)));

/* The ring's backing store: AUDIO_RING_CAPACITY_FRAMES sample-frames,
 * stereo. No alignment requirement beyond int16_t's own -- the CPU is the
 * only thing that ever touches this array; the Media Engine writes to
 * g_decode_out, never here. */
static int16_t g_ring_storage[AUDIO_RING_CAPACITY_FRAMES * AUDIO_SUPPORTED_CHANNELS];
static pcm_ring g_ring;

/* What the output thread drains a chunk into before handing it to the
 * audio channel. Same alignment reasoning as g_decode_out: written by the
 * CPU, then possibly read by the audio hardware's own DMA -- see this
 * file's header comment on why that writeback is not skipped just because
 * it is not the Media Engine on this side. */
static int16_t g_out_stage[AUDIO_OUT_CHUNK_FRAMES * AUDIO_SUPPORTED_CHANNELS] __attribute__((aligned(AUDIO_ME_ALIGN)));

static aac_asc_info g_asc;
static uint8_t      g_adts_channel_config;

static int g_codec_inited;
static int g_edram_claimed;
static int g_channel = -1;

/* The output thread and what it shares with the decoding side. The ring is
 * written by audio_decoder_push on the caller's thread and read here, and
 * pcm_ring keeps one shared count, so every ring call is made under g_lock.
 * The counters are written only by the thread and read by the caller: a
 * single aligned word each, so a read is never torn. */
static SceUID            g_lock   = -1;
static SceUID            g_thread = -1;
static volatile int      g_stop;
static volatile int      g_paused;
static volatile uint32_t g_played_frames;
static volatile uint32_t g_underruns;
static volatile int      g_out_rc; /* the first sceAudioOutputBlocking failure, or 0 */

/* Where in the stream each underrun happened, in played frames: the first
 * AUDIO_UNDERRUN_LOG of them, for the log -- a count alone cannot say
 * whether gaps line up with fragment boundaries, keyframes or neither. */
#define AUDIO_UNDERRUN_LOG 32u
static uint32_t g_underrun_at[AUDIO_UNDERRUN_LOG];

static void ring_lock(void) { sceKernelWaitSema(g_lock, 1, NULL); }
static void ring_unlock(void) { sceKernelSignalSema(g_lock, 1); }

/* Takes a chunk off the ring whenever there is one and hands it to the
 * hardware, which blocks until the channel can queue it -- that block is what
 * paces playback. A ring that runs dry after playing has started is an
 * underrun: a gap the listener hears. */
static int output_thread(SceSize args, void *argp) {
    int playing = 0;

    (void)args;
    (void)argp;
    while (!g_stop) {
        uint32_t got = 0;

        /* Paused is not starved: nothing is taken, and the gap that
         * follows is not an underrun. */
        if (g_paused) {
            playing = 0;
            sceKernelDelayThread(AUDIO_IDLE_US);
            continue;
        }

        ring_lock();
        if (pcm_ring_available(&g_ring) >= AUDIO_OUT_CHUNK_FRAMES)
            got = pcm_ring_read(&g_ring, g_out_stage, AUDIO_OUT_CHUNK_FRAMES);
        ring_unlock();

        if (got == 0) {
            if (playing) {
                if (g_underruns < AUDIO_UNDERRUN_LOG) g_underrun_at[g_underruns] = g_played_frames;
                g_underruns++;
                playing = 0;
            }
            sceKernelDelayThread(AUDIO_IDLE_US);
            continue;
        }

        /* See the writeback note on g_out_stage above. */
        sceKernelDcacheWritebackRange(g_out_stage, got * AUDIO_SUPPORTED_CHANNELS * sizeof(int16_t));
        {
            int rc = sceAudioOutputBlocking(g_channel, PSP_AUDIO_VOLUME_MAX, g_out_stage);
            if (rc < 0) {
                if (!g_out_rc) g_out_rc = rc;
                sceKernelDelayThread(AUDIO_IDLE_US);
                continue;
            }
        }
        g_played_frames += got;
        playing = 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ open */

/* How many pushes and outputs trace their firmware calls; see
 * platform/trace.h. */
#define AUDIO_TRACE_PUSHES 4u
static unsigned g_trace_pushes;

int audio_decoder_open(const uint8_t *asc, uint16_t asc_len, uint32_t sample_rate, uint8_t channels) {
    aac_config_err ace;
    int            rc;

    g_err[0] = 0;

    if (g_open) return fail(AUDIO_ERR_STATE, "the decoder is already open");
    if (!asc || asc_len == 0) return fail(AUDIO_ERR_ARG, "the stream arrived without an AudioSpecificConfig");
    if (sample_rate == 0) return fail(AUDIO_ERR_ARG, "a zero sample rate was given");
    if (channels != AUDIO_SUPPORTED_CHANNELS)
        return fail(AUDIO_ERR_UNSUPPORTED,
                     "only stereo AAC is supported -- see audio_psp.c for why mono is not yet implemented");

    /* The stream's own config first, because it is pure arithmetic and a
     * failure here should not leave a firmware module loaded or EDRAM
     * claimed behind it -- the same ordering video_decoder_open uses for
     * h264_build_avcc. */
    ace = aac_parse_asc(asc, asc_len, &g_asc);
    if (ace == AAC_CONFIG_ERR_ARG) return fail(AUDIO_ERR_ARG, "the AudioSpecificConfig is malformed");
    if (ace != AAC_CONFIG_OK)
        return fail(AUDIO_ERR_UNSUPPORTED, "the AudioSpecificConfig is not AAC-LC at a supported sample rate");

    if (g_asc.sample_rate != sample_rate)
        return fail(AUDIO_ERR_BAD_STREAM,
                     "the AudioSpecificConfig's sample rate disagrees with the container's own mp4a box");
    /* channel_config 0 means "defined elsewhere in the bitstream" -- fmp4's
     * own channel count is the only number available for that case, so it
     * is trusted rather than refused. Any OTHER disagreement is a
     * contradiction this project's server should never produce and is
     * refused rather than picked between. */
    if (g_asc.channel_config != 0 && g_asc.channel_config != channels)
        return fail(AUDIO_ERR_BAD_STREAM,
                     "the AudioSpecificConfig's channel configuration disagrees with the container's channel count");
    g_adts_channel_config = (g_asc.channel_config != 0) ? g_asc.channel_config : channels;

    /* AVCODEC before AAC -- MPEGBASE-style dependency.
     *
     * Through av_module_acquire, not sceUtilityLoadAvModule: AVCODEC is the
     * same module video_psp.c needs, and when each file tracked its own load
     * separately this call returned 0x80110F02 for a module video had already
     * loaded successfully. See platform/av_modules.h. */
    rc = av_module_acquire(PSP_AV_MODULE_AVCODEC);
    if (rc < 0) return fail_rc(AUDIO_ERR_DRIVER, "the audio codec module would not load", rc);

    /* Not fatal. The first hardware run (2026-09-23) had this refused with
     * 0x80110F01 -- "bad module id" -- so that console's firmware does not
     * know AAC as a separate AV module. Whether its sceAudiocodec can decode
     * AAC regardless is exactly what sceAudiocodecInit below answers, with
     * its own code; refusing here would only hide that answer. */
    rc = av_module_acquire(PSP_AV_MODULE_AAC);
    if (rc < 0) trace("audio: AAC module load -> 0x%08X, carrying on", (unsigned)rc);

    /* Everything below is a plain memset/memset-adjacent claim, not an
     * allocator call -- there is no partition block to size here the way
     * video_psp.c's 4 MB claim needs one; the only scarce resource is the
     * Media Engine's own EDRAM, claimed below through the firmware itself. */
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.f.sample_rate = sample_rate;

    /* Every sceAudiocodec call below follows the same rule as the decode in
     * audio_decoder_push: the control block is written by both sides, so the
     * cache lines are invalidated, not merely written back, on either side of
     * the call. needed_mem and edram_addr are firmware writes into this very
     * block and are read back immediately. */
    g_ctx.f.magic = AUDIO_CTX_MAGIC;
    sceKernelDcacheWritebackInvalidateRange(&g_ctx, sizeof(g_ctx));
    rc            = sceAudiocodecCheckNeedMem((unsigned long *)&g_ctx, PSP_CODEC_AAC);
    sceKernelDcacheWritebackInvalidateRange(&g_ctx, sizeof(g_ctx));
    if (rc < 0 || g_ctx.f.err != 0) {
        audio_decoder_close();
        return fail_rc(AUDIO_ERR_DRIVER, "the Media Engine would not say how much memory AAC decode needs", rc);
    }
    if ((uint32_t)g_ctx.f.needed_mem > AUDIO_NEEDED_MEM_MAX || g_ctx.f.needed_mem <= 0) {
        audio_decoder_close();
        return fail_val(AUDIO_ERR_MEMORY, "the Media Engine asked for an implausible amount of EDRAM for AAC",
                        (unsigned)g_ctx.f.needed_mem);
    }

    g_ctx.f.magic = AUDIO_CTX_MAGIC;
    sceKernelDcacheWritebackInvalidateRange(&g_ctx, sizeof(g_ctx));
    rc            = sceAudiocodecGetEDRAM((unsigned long *)&g_ctx, PSP_CODEC_AAC);
    sceKernelDcacheWritebackInvalidateRange(&g_ctx, sizeof(g_ctx));
    /* g_ctx.f.err as well as rc, like every other call here. This one was
     * checked on rc alone, which would let a claim that failed only through
     * `err` set g_edram_claimed and carry on into an Init against EDRAM that
     * was never handed over. */
    if (rc < 0 || g_ctx.f.err != 0) {
        audio_decoder_close();
        return fail_rc(AUDIO_ERR_MEMORY, "the Media Engine's EDRAM could not be claimed for AAC decode", rc);
    }
    g_edram_claimed = 1;

    g_ctx.f.magic = AUDIO_CTX_MAGIC;
    sceKernelDcacheWritebackInvalidateRange(&g_ctx, sizeof(g_ctx));
    rc            = sceAudiocodecInit((unsigned long *)&g_ctx, PSP_CODEC_AAC);
    trace("audio: sceAudiocodecInit -> 0x%08X", (unsigned)rc);
    sceKernelDcacheWritebackInvalidateRange(&g_ctx, sizeof(g_ctx));
    if (rc < 0 || g_ctx.f.err != 0) {
        audio_decoder_close();
        return fail_rc(AUDIO_ERR_DRIVER, "the Media Engine would not initialise an AAC decoder", rc);
    }
    g_codec_inited = 1;

    /* One hardware channel, reserved once and released in close() -- the
     * same "claim it up front, in a fixed order" rule design section 3.3
     * applies to the 4 MB video block applies to this, even though it is a
     * far smaller resource. */
    g_channel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, (int)AUDIO_OUT_CHUNK_FRAMES, PSP_AUDIO_FORMAT_STEREO);
    trace("audio: sceAudioChReserve -> 0x%08X", (unsigned)g_channel);
    if (g_channel < 0) {
        rc = g_channel;
        g_channel = -1;
        audio_decoder_close();
        return fail_rc(AUDIO_ERR_DRIVER, "no audio output channel was available", rc);
    }

    if (pcm_ring_init(&g_ring, g_ring_storage, AUDIO_RING_CAPACITY_FRAMES, AUDIO_SUPPORTED_CHANNELS) != AAC_CONFIG_OK) {
        /* Only reachable if the constants above are wrong, not from
         * anything a network could send -- but audio_decoder_close() must
         * still be able to run afterwards. */
        audio_decoder_close();
        return fail(AUDIO_ERR_ARG, "the PCM ring could not be initialised");
    }

    g_played_frames = 0;
    g_underruns     = 0;
    g_out_rc        = 0;
    g_stop          = 0;
    g_paused        = 0;

    g_lock = sceKernelCreateSema("hexfin_pcm", 0, 1, 1, NULL);
    if (g_lock < 0) {
        rc     = g_lock;
        g_lock = -1;
        audio_decoder_close();
        return fail_rc(AUDIO_ERR_DRIVER, "the PCM ring's lock could not be created", rc);
    }
    g_thread = sceKernelCreateThread("hexfin_audio", output_thread, AUDIO_THREAD_PRIO, AUDIO_THREAD_STACK, 0, NULL);
    if (g_thread < 0 || sceKernelStartThread(g_thread, 0, NULL) < 0) {
        rc = g_thread;
        if (g_thread >= 0) sceKernelDeleteThread(g_thread);
        g_thread = -1;
        audio_decoder_close();
        return fail_rc(AUDIO_ERR_DRIVER, "the audio output thread could not be started", rc);
    }

    g_open = 1;
    return AUDIO_OK;
}

/* ------------------------------------------------------------------ push */

int audio_decoder_push(const uint8_t *frame, uint32_t size, uint64_t dts) {
    int      rc;
    uint32_t out_bytes, out_frames;

    (void)dts; /* not used for anything on this side -- see audio_decoder.h */

    g_err[0] = 0;

    if (!g_open) return fail(AUDIO_ERR_STATE, "the decoder is not open");
    if (!frame || size == 0) return fail(AUDIO_ERR_ARG, "an empty AAC frame was pushed");
    if (size > AUDIO_INPUT_STAGE_CAP) return fail(AUDIO_ERR_BAD_STREAM, "an AAC frame claims an implausible size");
    if (g_out_rc) return fail_rc(AUDIO_ERR_DRIVER, "the audio channel refused the decoded PCM", g_out_rc);

    /* Back-pressure: decoding a frame the ring has no room for would lose
     * it, so the caller is told to come back instead, and nothing is
     * consumed. Room for the largest frame the output buffer can hold, not
     * just 1024 samples. */
    {
        uint32_t held;
        ring_lock();
        held = pcm_ring_available(&g_ring);
        ring_unlock();
        if (AUDIO_RING_CAPACITY_FRAMES - held < AUDIO_DECODE_OUT_BYTES / (AUDIO_SUPPORTED_CHANNELS * sizeof(int16_t)))
            return AUDIO_ERR_AGAIN;
    }

    /* The RAW frame, exactly as the MP4 sample holds it -- no ADTS header.
     * The first hardware run (2026-09-23) fed an ADTS-framed copy: the
     * decode returned 0 with err 0 and wrote nothing. The form below is what
     * decodes on hardware: raw frame, zero padding out to the fixed size the
     * firmware reads, that size in the input-size word, and the output
     * buffer's capacity in the output-size word. */
    memset(g_in_stage, 0, sizeof(g_in_stage));
    memcpy(g_in_stage, frame, size);

    /* The whole staging buffer, not just header+payload: whichever of the
     * two fixed upper bounds avcodec.prx actually reads (see
     * AUDIO_INPUT_STAGE_CAP), it reads within this buffer, and every byte
     * of it must already be in RAM rather than sitting in a dirty cache
     * line the ME cannot see. */
    sceKernelDcacheWritebackRange(g_in_stage, sizeof(g_in_stage));

    g_ctx.f.in_buf   = (uint32_t)(uintptr_t)g_in_stage;
    g_ctx.f.in_size  = (int32_t)sizeof(g_in_stage);
    g_ctx.f.out_buf  = (uint32_t)(uintptr_t)g_decode_out;
    g_ctx.f.out_size = (int32_t)AUDIO_DECODE_OUT_BYTES;
    g_ctx.f.err      = 0;

    /* WritebackINVALIDATE, not the plain writeback this used to be. The
     * control block is written by BOTH sides: the CPU fills in_buf/
     * in_size/out_buf/out_size, and the firmware writes err and out_size back into the
     * same 128 bytes. A plain writeback
     * pushes our bytes out to RAM but leaves the lines resident and clean,
     * so the reads of g_ctx.f.err and g_ctx.f.out_size just below
     * are served from the CPU's own pre-call copy -- the firmware's values
     * never arrive.
     *
     * The second-order effect is worse, and is the "works once, then breaks"
     * shape: those stale lines are still resident on the NEXT push, where
     * the CPU writes magic/in_buf/out_buf into them and writes all 128 bytes
     * back -- overwriting in RAM whatever the firmware had updated. The
     * invalidate must therefore happen both before the call (so the lines
     * are gone while the ME owns the block) and again after it (so the read
     * back comes from RAM). One writeback alone is not a cheaper version of
     * this; it is the bug. */
    sceKernelDcacheWritebackInvalidateRange(&g_ctx, sizeof(g_ctx));

    if (g_trace_pushes < AUDIO_TRACE_PUSHES) trace("audio: push %u size %u, sceAudiocodecDecode", g_trace_pushes, (unsigned)size);
    rc = sceAudiocodecDecode((unsigned long *)&g_ctx, PSP_CODEC_AAC);
    if (g_trace_pushes < AUDIO_TRACE_PUSHES)
        trace("audio: sceAudiocodecDecode -> 0x%08X err %d out %u", (unsigned)rc, (int)g_ctx.f.err, (unsigned)g_ctx.f.out_size);
    g_trace_pushes++;

    sceKernelDcacheWritebackInvalidateRange(&g_ctx, sizeof(g_ctx));

    /* Checked alongside g_ctx.f.err, not instead of it -- see this file's
     * header comment on why a return code alone might not be enough. */
    if (rc < 0 || g_ctx.f.err != 0) return fail_rc(AUDIO_ERR_DRIVER, "the Media Engine failed to decode an AAC frame", rc);

    /* The Media Engine wrote this behind the cache's back -- invalidate
     * before the CPU reads it, the same rule video_psp.c's fallback path
     * follows for its decoded picture. */
    sceKernelDcacheWritebackInvalidateRange((void *)(uintptr_t)g_ctx.f.out_buf, AUDIO_DECODE_OUT_BYTES);

    /* A frame of AAC-LC is 1024 samples, and the output-size word is not
     * known to be rewritten on every firmware.
     * So a plausible value is believed and anything else means one frame. */
    out_bytes = (uint32_t)g_ctx.f.out_size;
    if (out_bytes == 0 || out_bytes > AUDIO_DECODE_OUT_BYTES)
        out_bytes = AUDIO_OUT_CHUNK_FRAMES * AUDIO_SUPPORTED_CHANNELS * (uint32_t)sizeof(int16_t);

    out_frames = out_bytes / ((uint32_t)AUDIO_SUPPORTED_CHANNELS * (uint32_t)sizeof(int16_t));

    /* Room was checked before the decode, and only the output thread takes
     * from the ring in between, so this never clips. */
    ring_lock();
    (void)pcm_ring_write(&g_ring, (const int16_t *)(uintptr_t)g_ctx.f.out_buf, out_frames);
    ring_unlock();

    return AUDIO_OK;
}

/* --------------------------------------------------------------- queries */

uint32_t audio_decoder_played_frames(void) { return g_played_frames; }

void audio_decoder_pause(int paused) { g_paused = paused ? 1 : 0; }

uint32_t audio_decoder_underruns(void) { return g_underruns; }

uint32_t audio_decoder_underrun_at(uint32_t i) { return i < AUDIO_UNDERRUN_LOG ? g_underrun_at[i] : 0; }

uint32_t audio_decoder_buffered_frames(void) {
    uint32_t held;

    if (g_lock < 0) return 0;
    ring_lock();
    held = pcm_ring_available(&g_ring);
    ring_unlock();
    return held;
}

/* ----------------------------------------------------------------- close */

void audio_decoder_close(void) {
    /* Unwound in the reverse of the order open() built it, every step
     * guarded by its own flag -- this runs on open()'s failure paths too,
     * and must not touch g_err so a caller that closes after a failure
     * still has the reason. See video_decoder_close for the identical rule
     * and why design section 3.4 insists on it.
     *
     * The output thread first: it is the one thing still using the channel,
     * and it is blocked for at most one chunk. */
    if (g_thread >= 0) {
        SceUInt timeout = AUDIO_THREAD_EXIT_US;
        g_stop          = 1;
        if (sceKernelWaitThreadEnd(g_thread, &timeout) < 0) sceKernelTerminateThread(g_thread);
        sceKernelDeleteThread(g_thread);
        g_thread = -1;
    }
    if (g_lock >= 0) {
        sceKernelDeleteSema(g_lock);
        g_lock = -1;
    }
    if (g_channel >= 0) {
        sceAudioChRelease(g_channel);
        g_channel = -1;
    }

    if (g_codec_inited) {
        /* No documented "AAC decoder delete" beyond releasing its EDRAM --
         * unlike sceMpegDelete, there is no separate teardown call in
         * pspaudiocodec.h to make. */
        g_codec_inited = 0;
    }

    if (g_edram_claimed) {
        /* Exactly the leak this task's instructions warn about: EDRAM
         * survives THIS PROCESS if this is skipped, and the next run finds
         * it still held -- a failure that looks like "it worked once and
         * never again" because nothing about the run that leaked it looked
         * wrong from the outside. Called on every exit path out of open(),
         * not only this one that a caller reaches after a clean run. */
        /* magic first, like every other sceAudiocodec call in this file.
         * It was the one call that skipped it, and it is precisely the call
         * whose silent failure leaks the EDRAM and breaks the NEXT run --
         * the failure mode the comment above describes. */
        g_ctx.f.magic = AUDIO_CTX_MAGIC;
        sceKernelDcacheWritebackInvalidateRange(&g_ctx, sizeof(g_ctx));
        sceAudiocodecReleaseEDRAM((unsigned long *)&g_ctx);
        sceKernelDcacheWritebackInvalidateRange(&g_ctx, sizeof(g_ctx));
        g_edram_claimed = 0;
    }

    /* The AV modules are left loaded, for the identical reason
     * video_decoder_close leaves sceMpeg's: unloading is a step that can
     * itself fail mid-teardown, and their flags surviving close() means a
     * reopen does not ask for a module it already has. */

    memset(&g_ctx, 0, sizeof(g_ctx));
    memset(&g_ring, 0, sizeof(g_ring));
    g_open = 0;
}

const char *audio_decoder_error(void) { return g_err; }

#else

/* The host check build compiles every .c file under src/media. There is no
 * Media Engine here and nothing to stand in for one -- see video_psp.c's
 * identical guard and identical reason. */
typedef int audio_psp_needs_a_psp;

#endif /* __PSP__ */
