/* hexfin_me.prx: the one piece of this project that runs in kernel mode.
 *
 * Its whole job is to restart the Media Engine in a mode that decodes H.264
 * from raw access units, before the EBOOT touches sceMpeg. The first hardware
 * runs (2026-09-23, 6.61 ARK-5) had every sceMpegAvcDecode refuse with
 * 0x80628002 even though every setup call returned 0 -- through the game-side
 * sceMpeg, at every resolution, with and without audio, at 222 and 333 MHz.
 * Booting the ME from a kernel module first is what makes raw-AU decoding
 * work on 6.61. This is that step and nothing else: no hooks, no patches, no
 * exports of its own.
 *
 * The boot mode is the EBOOT's to choose, passed as sceKernelStartModule's
 * argument (one int), because which mode is right is what the hardware runs
 * were for: mode 1 refused every frame on a PSP-1000; mode 4 decodes
 * Baseline (mode 3 is the one for Main).
 *
 * Built separately (src/kbridge/Makefile) and shipped next to EBOOT.PBP.
 * Loaded through ARK's kuKernelLoadModule; see platform/me_runtime.c. */
#include <pspkernel.h>

PSP_MODULE_INFO("hexfin_me", 0x1006, 1, 0);
PSP_MAIN_THREAD_ATTR(0);

/* sceMeCore_driver's boot entry on 6.60 and later; me_imports.S. */
extern int sceMeBootStart660(int mode);

#define ME_DEFAULT_MODE 1

int module_start(SceSize args, void *argp) {
    int mode = ME_DEFAULT_MODE;

    if (args >= sizeof(int) && argp) mode = *(const int *)argp;
    /* The result is the module's exit status, which sceKernelStartModule
     * hands back to the EBOOT to report. Returning a negative value from
     * module_start would unload the module, so the code is folded into a
     * non-negative status: 0 is success, anything else is 0x1000 plus the
     * low bits of the firmware's code, visible in the probe's log. */
    {
        int rc = sceMeBootStart660(mode);
        return rc < 0 ? (0x1000 | (rc & 0xFFF)) : 0;
    }
}

int module_stop(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    return 0;
}
