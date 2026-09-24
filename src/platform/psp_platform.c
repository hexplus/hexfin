/* See platform/psp_platform.h. */

#include "platform/psp_platform.h"

#include <kubridge.h>
#include <pspkernel.h>
#include <psppower.h>
#include <psprtc.h>
#include <pspsysmem.h>
#include <psputility.h>
#include <psputils.h>

#include "platform/trace.h"
#include "ui/text.h"

#include <stdint.h>
#include <string.h>
#include <systemctrl.h>

static volatile int g_exit_requested;
/* -1 = not yet decided, 0 = registration failed, 1 = registered. Polled
 * (briefly, boundedly) by platform_init() so a dead HOME button is a
 * reported failure rather than a silent one. */
static volatile int g_callback_ready = -1;

static void (*volatile g_stop_hook)(void);

/* How long HOME waits for the main thread's own teardown before forcing the
 * exit. Teardown takes well under a second when nothing is stuck. */
#define EXIT_GRACE_MS 3000

static int exit_callback(int arg1, int arg2, void *common) {
    (void)arg1;
    (void)arg2;
    (void)common;
    /* The flag first, so anything the hook wakes finds the answer already
     * there rather than racing the write. Then only a flag and a nudge:
     * tearing down from the callback thread is what leaves the screen black
     * on the way out, so this thread never does -- see below for the one
     * case where it exits anyway. */
    g_exit_requested = 1;
    if (g_stop_hook) g_stop_hook();

    /* The main thread is supposed to notice the flag and leave through its
     * own teardown, and that return ends the program -- this thread along
     * with it. If it has not after EXIT_GRACE_MS, it is stuck inside a call
     * that will not come back (the first hardware run of the decoder froze
     * exactly like that, with HOME doing nothing), and a half-torn-down exit
     * is still better than a console that has to be switched off. */
    {
        int waited;
        for (waited = 0; waited < EXIT_GRACE_MS; waited += 100) sceKernelDelayThread(100 * 1000);
    }
    sceKernelExitGame();
    return 0;
}

static int callback_thread(SceSize args, void *argp) {
    int cbid;

    (void)args;
    (void)argp;
    cbid = sceKernelCreateCallback("exit", exit_callback, 0);
    if (cbid >= 0 && sceKernelRegisterExitCallback(cbid) >= 0) {
        g_callback_ready = 1;
    } else {
        g_callback_ready = 0;
        return 0;
    }
    sceKernelSleepThreadCB();
    return 0;
}

int platform_init(void) {
    SceUID thid = sceKernelCreateThread("cb", callback_thread, 0x11, 0xFA0, 0, 0);
    int attempts;

    if (thid < 0) return -1;
    if (sceKernelStartThread(thid, 0, 0) < 0) return -1;

    /* Bounded so a failure elsewhere cannot hang here forever. */
    for (attempts = 0; g_callback_ready < 0 && attempts < 100; attempts++)
        sceKernelDelayThread(1000);

    return (g_callback_ready == 1) ? 0 : -1;
}

/* Resets the firmware's idle timers every few seconds, so the backlight
 * does not dim and go out in the middle of a film nobody is touching the
 * buttons during -- and the console does not auto-sleep, which would take
 * the Wi-Fi and the stream with it. TICK_ALL covers both. The shortest
 * backlight setting in the PSP's own menu is 1 minute; 5 s is far inside
 * it. */
#define KEEP_AWAKE_US (5u * 1000u * 1000u)

static int keep_awake_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    while (!g_exit_requested) {
        scePowerTick(PSP_POWER_TICK_ALL);
        sceKernelDelayThread(KEEP_AWAKE_US);
    }
    return 0;
}

int platform_keep_awake(void) {
    /* Low priority: it does one call every five seconds and must never
     * delay the audio or the reader. */
    SceUID thid = sceKernelCreateThread("hexfin_awake", keep_awake_thread, 0x30, 0x800, 0, 0);

    if (thid < 0) return thid;
    return sceKernelStartThread(thid, 0, 0);
}

void platform_status(char *dst, unsigned cap, int show_clock, int show_battery) {
    ScePspDateTime t;
    int            hour = -1, minute = -1, fmt = PSP_SYSTEMPARAM_TIME_FORMAT_24HR;
    int            pct = -1, charging = 0;

    if (show_clock && sceRtcGetCurrentClockLocalTime(&t) >= 0) {
        hour   = t.hour;
        minute = t.minute;
        /* The console's own 12/24-hour choice, as its menu shows the time. */
        if (sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_TIME_FORMAT, &fmt) < 0)
            fmt = PSP_SYSTEMPARAM_TIME_FORMAT_24HR;
    }
    if (show_battery && scePowerIsBatteryExist()) {
        pct      = scePowerGetBatteryLifePercent();
        charging = scePowerIsBatteryCharging() > 0;
        if (pct < 0) pct = -1;
    }
    text_status(dst, cap, show_clock, hour, minute, fmt == PSP_SYSTEMPARAM_TIME_FORMAT_24HR, show_battery, pct,
                charging);
}

/* ------------------------------------------------------------ the volume
 *
 * The level the volume buttons set lives in the firmware's impose module,
 * and the only call that reads it, sceImposeGetParam, is kernel-only
 * (pspimpose_driver.h). Two things ARK provides get us there from user mode:
 * sctrlHENFindFunction finds the function's address -- asked with its
 * 3.52-era NID, 0x531C9778, which the CFW's NID resolver is there to
 * translate to 6.61's -- and kuKernelCall runs it with kernel privilege.
 *
 * UNVERIFIED -- REQUIRES REAL PSP TEST. If the function is not found, or
 * the call fails, the volume reads as unavailable once and for all, and the
 * caller shows nothing: never a guess, never a crash. */
#define IMPOSE_MODULE          "sceImpose_Driver"
#define IMPOSE_LIBRARY         "sceImpose_driver"
#define IMPOSE_GET_PARAM_NID   0x531C9778u
#define IMPOSE_MAIN_VOLUME     0x1
#define IMPOSE_MUTE            0x8

static int   g_volume_state; /* 0 not tried, 1 ready, -1 unavailable */
static void *g_get_param;

static int impose_get(int param, int *out) {
    KernelCallArg a;

    memset(&a, 0, sizeof a);
    a.arg1 = (uint32_t)param;
    if (kuKernelCall(g_get_param, &a) < 0 || (int)a.ret1 < 0) return -1;
    *out = (int)a.ret1;
    return 0;
}

int platform_volume(int *level, int *muted) {
    int v = 0, m = 0;

    if (g_volume_state == 0) {
        uint32_t addr = sctrlHENFindFunction(IMPOSE_MODULE, IMPOSE_LIBRARY, IMPOSE_GET_PARAM_NID);

        /* Only a word-aligned address in kernel RAM is a function this
         * program will jump to with kernel privilege. Anything else is an
         * error code in disguise: an unlinked import answers 0x8002013A
         * (seen in the emulator, 2026-09-24), and jumping there would crash. */
        g_volume_state = ((addr & 3u) == 0 && addr >= 0x88000000u && addr < 0x8C000000u) ? 1 : -1;
        g_get_param    = g_volume_state == 1 ? (void *)(uintptr_t)addr : NULL;
        trace("volume: sceImposeGetParam at 0x%08X, %s", (unsigned)addr,
              g_volume_state == 1 ? "volume display on" : "not usable, no volume display");
    }
    if (g_volume_state != 1) return -1;
    if (impose_get(IMPOSE_MAIN_VOLUME, &v) < 0) {
        g_volume_state = -1;
        trace("volume: reading it failed, no volume display");
        return -1;
    }
    if (impose_get(IMPOSE_MUTE, &m) < 0) m = 0;
    *level = v;
    *muted = m != 0;
    return 0;
}

int platform_volume_changed(int *level, int *muted) {
    static int                last_level = -1, last_muted = -1;
    static unsigned long long checked_us;
    unsigned long long        now = platform_now_us();
    int                       v, m;

    if (now - checked_us < 100000ull) return 0;
    checked_us = now;
    if (platform_volume(&v, &m) != 0) return 0;
    if (last_level < 0) { /* the first reading is where it starts, not a change */
        last_level = v;
        last_muted = m;
        return 0;
    }
    if (v == last_level && m == last_muted) return 0;
    last_level = v;
    last_muted = m;
    *level     = v;
    *muted     = m;
    return 1;
}

int platform_exit_requested(void) { return g_exit_requested; }

void platform_set_stop_hook(void (*fn)(void)) { g_stop_hook = fn; }

unsigned long long platform_now_us(void) {
    return (unsigned long long)sceKernelGetSystemTimeWide();
}

unsigned platform_free_kb(void) { return (unsigned)(sceKernelTotalFreeMemSize() / 1024); }

unsigned platform_largest_free_kb(void) { return (unsigned)(sceKernelMaxFreeMemSize() / 1024); }

void platform_exit(void) { sceKernelExitGame(); }
