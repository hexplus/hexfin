/* See platform/input.h. */
#if defined(__PSP__)

#include "platform/input.h"

#include "platform/psp_platform.h"

#include <pspctrl.h>

#define REPEAT_FIRST_US 400000ull
#define REPEAT_NEXT_US  80000ull

#define REPEATING (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT | PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)

static int                g_ready;
static unsigned           g_prev;
static unsigned long long g_next_repeat_us;

static unsigned read_buttons(void) {
    SceCtrlData pad;

    if (!g_ready) {
        sceCtrlSetSamplingCycle(0);
        sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
        g_ready = 1;
    }
    if (sceCtrlPeekBufferPositive(&pad, 1) < 1) return 0;
    return pad.Buttons;
}

unsigned input_held(void) { return read_buttons(); }

unsigned input_pressed(void) {
    unsigned           now  = read_buttons();
    unsigned           out  = now & ~g_prev;
    unsigned long long t    = platform_now_us();
    unsigned           held = now & g_prev & REPEATING;

    if (out & REPEATING) g_next_repeat_us = t + REPEAT_FIRST_US;
    else if (held && t >= g_next_repeat_us) {
        out |= held;
        g_next_repeat_us = t + REPEAT_NEXT_US;
    }
    g_prev = now;
    return out;
}

#endif /* __PSP__ */
