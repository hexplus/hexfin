/* See net/net_modules.h.
 *
 * Only compiled for the console; scripts/test.sh compiles every .c under
 * src/net and there is no sceUtility there. */
#if defined(__PSP__)

#include "net/net_modules.h"

#include <psputility.h>

/* The firmware's net module ids are 1..7 (psputility_netmodules.h). The array
 * is indexed by id rather than searched, so it is exactly as wide as that
 * range and no wider; slot 0 is never a valid id and is simply never read. */
#define NET_MODULE_COUNT 8

static unsigned char g_loaded[NET_MODULE_COUNT];

int net_module_acquire(int module) {
    int rc;

    if (module <= 0 || module >= NET_MODULE_COUNT) return -1;
    if (g_loaded[module]) return 0;

    rc = sceUtilityLoadNetModule(module);
    if (rc < 0) return rc;

    g_loaded[module] = 1;
    return 0;
}

#endif /* __PSP__ */
