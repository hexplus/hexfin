#ifndef HEXFIN_AV_MODULES_H
#define HEXFIN_AV_MODULES_H

/* One owner for the firmware's AV modules.
 *
 * sceUtilityLoadAvModule refuses a module that is already loaded rather than
 * quietly succeeding. That matters here because AVCODEC is needed by BOTH
 * decoders: video_psp.c loads it before sceMpeg, and audio_psp.c loads it
 * before sceAudiocodec. Each file kept its own "have I loaded this" flag,
 * which is right within one file and wrong across two -- whichever decoder
 * opened second asked for a module that was already loaded and working, and
 * got a driver error for it.
 *
 * Measured, not theorised: with both decoders opening against the same
 * fixture, video opened cleanly and audio then refused with 0x80110F02 --
 * exactly the shape of failure that would have been blamed on the AAC path.
 *
 * So the flag belongs to the module rather than to the caller. Acquiring a
 * module that is already loaded succeeds.
 *
 * There is no release. Both decoders deliberately leave the AV modules loaded
 * at close (see video_decoder_close): unloading is a step that can itself
 * fail in the middle of a teardown that runs on the error path, and design
 * section 3.4 wants teardown to be the one thing that cannot break. Adding an
 * unload here that nothing calls would only make that decision harder to see.
 */

/* Returns 0 if the module is loaded -- whether this call loaded it or an
 * earlier one did. On failure returns the firmware's own negative result, so
 * the caller can report the code it would have reported before. A module id
 * outside the firmware's range returns -1, which is not a firmware code and
 * is not meant to look like one. */
int av_module_acquire(int module);

#endif /* HEXFIN_AV_MODULES_H */
