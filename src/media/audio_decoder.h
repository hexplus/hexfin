/* The seam between "AAC bytes that came off a network" and "PCM a channel can
 * play" -- audio's half of the pair with media/video_decoder.h, and built to
 * the same rule: this header exists so the parsing and buffering around the
 * codec can be exercised without a PSP, and the real implementation
 * (audio_psp.c) decodes on the Media Engine's sceAudiocodec and exists on the
 * console and nowhere else. A software decoder could implement the same four
 * calls later without a caller changing a line.
 *
 * Nothing here mentions sceAudiocodec, ADTS or EDRAM on purpose, for the same
 * reason video_decoder.h says nothing about sceMpeg: the moment a caller
 * knows what is behind this header, swapping the implementation stops being
 * possible.
 *
 * No platform headers, so the host can at least compile a caller.
 *
 * Errors return a code and leave a readable reason behind (PROMPT.md
 * section 47), exactly as video_decoder.h's do. */
#ifndef MEDIA_AUDIO_DECODER_H
#define MEDIA_AUDIO_DECODER_H

#include <stdint.h>

typedef enum {
    AUDIO_OK = 0,
    AUDIO_ERR_STATE,       /* called out of order: open twice, push before open */
    AUDIO_ERR_ARG,         /* the caller's arguments cannot be right */
    AUDIO_ERR_MEMORY,      /* the partition or EDRAM could not give us what we need */
    AUDIO_ERR_DRIVER,      /* a firmware call refused; the reason carries its code */
    AUDIO_ERR_BAD_STREAM,  /* the bytes or config handed in do not describe usable AAC */
    AUDIO_ERR_UNSUPPORTED, /* valid AAC, but not a shape this decoder can take */
    AUDIO_ERR_AGAIN        /* no PCM ready yet. NOT a failure -- see below */
} audio_err;

/* Claims everything the decoder will ever need and hands it the stream's
 * AudioSpecificConfig. Called once, before playback, for the same reason
 * video_decoder_open is: design section 3.3 wants the large blocks (here,
 * EDRAM) claimed in a fixed order before anything has had a chance to
 * fragment the partition.
 *
 * `asc` is the AudioSpecificConfig exactly as fmp4 copied it out of esds --
 * ISO/IEC 14496-3's 5-bit object type / 4-bit sampling frequency index /
 * 4-bit channel configuration, plus whatever GASpecificConfig bits follow
 * that this decoder does not read.
 *
 * `sample_rate` and `channels` come from fmp4's own reading of the mp4a
 * sample entry, a different part of the container than the ASC. They are
 * passed alongside it, not derived from it a second time, so open() can
 * refuse a stream where the two disagree -- which is possible for a
 * malformed or hostile file (PROMPT.md section 35) and would otherwise pick
 * one of two contradictory numbers by accident.
 *
 * Returns AUDIO_OK, or a code with the reason in audio_decoder_error(). */
int audio_decoder_open(const uint8_t *asc, uint16_t asc_len, uint32_t sample_rate, uint8_t channels);

/* Submits one AAC access unit -- one frame's worth of raw AAC bytes, exactly
 * as fmp4 hands them out of mdat with no ADTS framing of its own. `frame`
 * need not stay valid after the call returns.
 *
 * `dts` rides along for the same reason video_decoder_push's does: matching
 * against the video/audio clock is a caller concern, not this one.
 *
 * An AAC-LC frame decodes to a fixed 1024 samples per channel, and the PCM
 * goes straight into a ring that this module's own output thread plays from
 * -- the caller never handles PCM. Playback starts by itself once the first
 * frame is in, and paces itself on the hardware.
 *
 * Returns AUDIO_ERR_AGAIN, having consumed nothing, when the ring has no room
 * for another frame: NOT a failure, but back-pressure. Push the same frame
 * again later; meanwhile the output thread is playing what is already held. */
int audio_decoder_push(const uint8_t *frame, uint32_t size, uint64_t dts);

/* Sample-frames the output thread has handed to the hardware since open --
 * the audio clock. Divide by the sample rate for seconds. It runs ahead of
 * what the speaker has actually played by the hardware's own queue (about
 * one 1024-frame chunk). */
uint32_t audio_decoder_played_frames(void);

/* How many times playback ran dry after it had started: gaps the listener
 * heard. The end of a stream counts once. */
uint32_t audio_decoder_underruns(void);

/* The played-frame position of underrun `i`, for the first 32; 0 past that. */
uint32_t audio_decoder_underrun_at(uint32_t i);

/* Sample-frames decoded and waiting in the ring. */
uint32_t audio_decoder_buffered_frames(void);

/* Releases everything audio_decoder_open claimed -- most importantly the
 * Media Engine's EDRAM claim, which (unlike ordinary partition memory)
 * survives this process if it is never released: the next run finds the
 * EDRAM still held and cannot start its own decoder, a failure that looks
 * exactly like "it worked once and never again" because nothing about THIS
 * run's exit looked wrong.
 *
 * Safe to call twice, and safe to call when open failed or was never called,
 * for the same reason video_decoder_close is: the teardown path runs after
 * failures too. */
void audio_decoder_close(void);

/* 1: the output thread stops taking from its ring, so the sound stops and
 * audio_decoder_played_frames() -- the clock -- stops with it, without
 * counting an underrun. 0: carries on from the same sample. */
void audio_decoder_pause(int paused);

/* The reason for the last failure, in words a person can act on, or "" if
 * nothing has failed. Valid until the next call into this module. */
const char *audio_decoder_error(void);

#endif
