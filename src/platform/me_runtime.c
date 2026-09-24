/* See platform/me_runtime.h. */
#include "platform/me_runtime.h"

#include "platform/av_modules.h"
#include "platform/trace.h"

#include <kubridge.h>
#include <pspkernel.h>
#include <psputility.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* The bridge travels next to EBOOT.PBP. A homebrew's working directory is
 * the folder it was launched from; the ms0: path is the fallback for a
 * launcher that leaves it elsewhere. kuKernelLoadModule wants an absolute
 * path, so the working directory is asked for rather than assumed. */
#define ME_BRIDGE_NAME     "hexfin_me.prx"
#define ME_BRIDGE_FALLBACK "ms0:/PSP/GAME/Hexfin/" ME_BRIDGE_NAME
#define ME_MPEG_VSH        "flash0:/kd/mpeg_vsh.prx"

/* sceKernelLoadModule's "no such file". */
#define ME_ERR_NOENT 0x80010002u

static int load_and_start(const char *path, SceSize args, void *argp, const char *what) {
    SceUID id;
    int    status = 0, rc;

    id = kuKernelLoadModule(path, 0, NULL);
    trace("me: load %s (%s) -> 0x%08X", what, path, (unsigned)id);
    if (id < 0) return id;

    rc = sceKernelStartModule(id, args, argp, &status, NULL);
    trace("me: start %s -> 0x%08X status 0x%08X", what, (unsigned)rc, (unsigned)status);
    return rc < 0 ? rc : 0;
}

int me_runtime_load(int boot_mode) {
    char path[256];
    int  rc;

    if (getcwd(path, sizeof path - sizeof ME_BRIDGE_NAME - 1)) {
        size_t n = strlen(path);
        if (n && path[n - 1] != '/') path[n++] = '/';
        strcpy(path + n, ME_BRIDGE_NAME);
    } else {
        strcpy(path, ME_BRIDGE_FALLBACK);
    }

    rc = load_and_start(path, sizeof boot_mode, &boot_mode, "bridge");
    if ((unsigned)rc == ME_ERR_NOENT) rc = load_and_start(ME_BRIDGE_FALLBACK, sizeof boot_mode, &boot_mode, "bridge");
    if (rc < 0) return rc;

    rc = av_module_acquire(PSP_AV_MODULE_AVCODEC);
    trace("me: AVCODEC -> 0x%08X", (unsigned)rc);
    if (rc < 0) return rc;

    return load_and_start(ME_MPEG_VSH, 0, NULL, "mpeg_vsh");
}
