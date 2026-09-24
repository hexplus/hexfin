#include "platform/av_modules.h"

#include <psputility.h>

/* The firmware's AV module ids are 0..7. The array is indexed by id rather
 * than searched, so it is exactly as wide as that range and no wider. */
#define AV_MODULE_COUNT 8

/* sceUtilityLoadAvModule's answer for a module that is already resident. */
#define AV_MODULE_ERROR_ALREADY_LOADED 0x80110F02u

static unsigned char g_loaded[AV_MODULE_COUNT];

int av_module_acquire(int module) {
    int rc;

    if (module < 0 || module >= AV_MODULE_COUNT) return -1;
    if (g_loaded[module]) return 0;

    rc = sceUtilityLoadAvModule(module);
    /* "Already loaded" is loaded: a previous run that exited without its
     * teardown leaves the module resident, and it works. */
    if (rc < 0 && (unsigned)rc != AV_MODULE_ERROR_ALREADY_LOADED) return rc;

    g_loaded[module] = 1;
    return 0;
}
