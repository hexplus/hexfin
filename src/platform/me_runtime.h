/* Prepares the Media Engine the way the players that decode H.264 on 6.61
 * do, before anything touches sceMpeg.
 *
 * Three steps, in this order, each reported through platform/trace.h:
 *
 *   1. hexfin_me.prx (src/kbridge), loaded through ARK's kuKernelLoadModule
 *      and started with the boot mode, restarts the Media Engine.
 *   2. The AVCODEC AV module, which mpeg_vsh's own imports need.
 *   3. flash0:/kd/mpeg_vsh.prx -- the console's own VSH copy of sceMpeg --
 *      loaded and started, so that this program's sceMpeg imports bind to
 *      it. It has to come before anything loads the game-side sceMpeg (the
 *      MPEGBASE AV module), because a library that already exists is not
 *      registered a second time.
 *
 * Why: through the game-side sceMpeg alone, every sceMpegAvcDecode on
 * hardware refused with 0x80628002 (docs/RESEARCH.md section 10).
 *
 * Requires ARK (or another CFW that provides KUBridge). Only compiled for the
 * console. */
#ifndef PLATFORM_ME_RUNTIME_H
#define PLATFORM_ME_RUNTIME_H

/* 0 when all three steps succeeded; otherwise the first failing step's
 * negative code, with the step named in the trace. `boot_mode` is passed to
 * hexfin_me.prx; see src/kbridge/me_boot.c. */
int me_runtime_load(int boot_mode);

#endif /* PLATFORM_ME_RUNTIME_H */
