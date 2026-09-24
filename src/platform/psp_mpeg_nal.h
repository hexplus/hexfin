/* The one sceMpeg function PSPSDK does not declare.
 *
 * pspmpeg.h covers everything else this project calls. sceMpegGetAvcNalAu
 * (NID 0x11F95CF1) is absent from it and unstubbed in libpspmpeg, so both the
 * import entry (src/platform/psp_mpeg_import.S) and the declaration below are
 * ours. It exists here rather than in video_psp.c so that the one piece of
 * genuinely unverified interface in the project sits in a file by itself,
 * where it can be found and corrected.
 *
 *
 * THE SIGNATURE, AND HOW IT WAS SETTLED
 *
 * This header used to declare a guessed five-argument form (handle, access
 * unit, length, avcC record, SceMpegAu). The first hardware run of it,
 * 2026-09-23, switched the PSP-1000 off inside the very first call: the
 * firmware read our access unit's bytes as a pointer to a structure and
 * followed them. The emulator could never have caught it -- it does not
 * implement this function.
 *
 * The form below is the firmware's: three arguments, the handle, a pointer
 * to a 32-byte input structure, and the SceMpegAu to fill (NID 0x11F95CF1,
 * a 32-byte input and a 24-byte output). It is what runs on hardware.
 *
 * What the fields mean:
 *
 *   - sps/pps: the raw parameter-set NAL units, separately, each with its
 *     own length -- no start codes, no length prefix, not the avcC record.
 *   - nal_prefix_size: the width of the length prefix on each NAL inside the
 *     access unit (avcC's lengthSizeMinusOne + 1; 4 for Jellyfin).
 *   - nal/nal_size: the access unit itself, exactly as the MP4 sample holds
 *     it.
 *   - mode: 3 until the decoder has produced its first picture, 0 after.
 *     Every caller does this; none documents why.
 */
#ifndef PLATFORM_PSP_MPEG_NAL_H
#define PLATFORM_PSP_MPEG_NAL_H

#include <pspmpeg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* sceMpegGetAvcNalAu's input. 32 bytes, in this order -- the firmware reads
 * it by offset, so neither the order nor the widths may change. */
typedef struct {
    const void *sps;
    SceInt32    sps_size;
    const void *pps;
    SceInt32    pps_size;
    SceInt32    nal_prefix_size;
    const void *nal;
    SceInt32    nal_size;
    SceInt32    mode;
} psp_mpeg_avc_nal;

/* The two values `mode` takes; see above. */
#define PSP_MPEG_NAL_MODE_FIRST 3
#define PSP_MPEG_NAL_MODE_NEXT  0

/* Hands one access unit, and the parameter sets that describe it, to the
 * decoder, and fills `pAu` for sceMpegAvcDecode.
 *
 * Every buffer `pNal` points at is read by the Media Engine directly and must
 * already be written back out of the CPU cache.
 *
 * @return 0 on success, a negative firmware status otherwise. */
SceInt32 sceMpegGetAvcNalAu(SceMpeg *Mpeg, const psp_mpeg_avc_nal *pNal, SceMpegAu *pAu);

/* ------------------------------------------------ decoding to nowhere, then
 * converting
 *
 * The shape that works on 6.61:
 * sceMpegAvcDecode with a NULL destination leaves the picture as YUV planes in
 * the Media Engine's memory; sceMpegAvcDecodeDetail2 says where; and
 * sceMpegBaseCscAvc converts them into an 8888 buffer. The layouts below are
 * read by offset. */

typedef struct {
    SceInt32 unk0, unk1;
    SceInt32 width;  /* coded width, in pixels */
    SceInt32 height; /* coded height, in pixels */
} psp_mpeg_avc_picture;

typedef struct {
    void *plane[8]; /* the YUV planes, in sceMpegBaseCscAvc's order */
} psp_mpeg_avc_yuv;

typedef struct {
    SceInt32              unk0[4];
    psp_mpeg_avc_picture *picture; /* +16 */
    SceInt32              unk1[6];
    psp_mpeg_avc_yuv     *yuv; /* +44 */
} psp_mpeg_avc_detail;

/* sceMpegBaseCscAvc's description of the picture: 48 bytes. */
typedef struct {
    SceInt32 height_blocks; /* (height + 15) / 16 */
    SceInt32 width_blocks;  /* (width + 15) / 16 */
    SceInt32 mode0, mode1;  /* 0, 0 */
    void    *plane[8];
} psp_mpeg_avc_csc;

SceInt32 sceMpegAvcDecodeDetail2(SceMpeg *Mpeg, psp_mpeg_avc_detail **detail);

/* In libpspmpegbase, stubbed but not declared by the SDK. `unk` is 0 in every
 * caller; `stride` is the destination's row length in pixels. */
SceInt32 sceMpegBaseCscAvc(void *dst, SceUInt32 unk, SceUInt32 stride, psp_mpeg_avc_csc *csc);

/* sceMpegAvcDecode's fatal decode error. B-slices are the cause worth naming
 * (docs/RESEARCH.md section 2): the Media Engine cannot decode them, it is the
 * most likely real-world failure of this whole path, and it appears several
 * seconds INTO an otherwise healthy stream rather than on the first frame.
 *
 * But this code is NOT specific to B-slices. The emulator returns the same
 * value whenever it cannot get a picture out of the decoder for any reason
 * -- including, as this project measured, a Constrained Baseline stream with
 * has_b_frames=0 fed through the NAL-AU path it does not implement -- and
 * on hardware it came back for a wrong Media Engine boot mode too. So the name says what the firmware says,
 * and video_psp.c reports B-slices as the likely cause rather than as fact. */
#define SCE_MPEG_ERROR_AVC_DECODE_FATAL 0x80628002

#ifdef __cplusplus
}
#endif

#endif
