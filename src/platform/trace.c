/* See platform/trace.h. */
#include "platform/trace.h"

#include <pspdebug.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define TRACE_MAX_LINES 4000

/* The debug screen's last text row, and how many characters fit on it. The
 * newest line is printed there as well as to the file: a console that
 * switches itself off can take the memory stick's pending writes with it,
 * and a photo of the screen cannot be lost that way. */
#define TRACE_SCREEN_ROW  33
#define TRACE_SCREEN_COLS 68

/* Lines held in memory while deferred -- see trace_defer. 32 kB is a few
 * hundred lines, far more than a playback run writes. */
#define TRACE_DEFER_BYTES (96u * 1024u)

static char               g_defer[TRACE_DEFER_BYTES];
static unsigned           g_defer_len;
static int                g_deferring;
static int                g_no_screen;
static unsigned           g_defer_lost;

static char               g_paths[2][64];
static const char        *g_path;
static unsigned           g_lines;
static unsigned long long g_start_us;

void trace_init(const char *tag) {
    unsigned i;

    snprintf(g_paths[0], sizeof g_paths[0], "ms0:/probe-%s.log", tag);
    snprintf(g_paths[1], sizeof g_paths[1], "probe-%s.log", tag);

    g_path     = NULL;
    g_lines    = 0;
    g_start_us = (unsigned long long)sceKernelGetSystemTimeWide();

    for (i = 0; i < sizeof g_paths / sizeof g_paths[0]; i++) {
        SceUID fd = sceIoOpen(g_paths[i], PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd >= 0) {
            sceIoClose(fd);
            g_path = g_paths[i];
            return;
        }
    }
}

void trace(const char *fmt, ...) {
    char     line[192];
    char     shown[TRACE_SCREEN_COLS + 1];
    int      n;
    unsigned i;
    va_list  ap;
    SceUID   fd;

    if (g_lines >= TRACE_MAX_LINES) return;
    g_lines++;

    n = snprintf(line, sizeof line, "%7u ",
                 (unsigned)(((unsigned long long)sceKernelGetSystemTimeWide() - g_start_us) / 1000u));
    if (n < 0) return;
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;

    line[n++] = '\n';

    /* Deferred: into memory only. No screen either -- during playback the
     * text would land on the picture. */
    if (g_deferring) {
        if (g_defer_len + (unsigned)n <= TRACE_DEFER_BYTES) {
            memcpy(g_defer + g_defer_len, line, (size_t)n);
            g_defer_len += (unsigned)n;
        } else {
            g_defer_lost++;
        }
        return;
    }

    if (!g_no_screen) {
        for (i = 0; i < TRACE_SCREEN_COLS; i++) shown[i] = (i + 1 < (unsigned)n) ? line[i] : ' ';
        shown[TRACE_SCREEN_COLS] = 0;
        pspDebugScreenSetXY(0, TRACE_SCREEN_ROW);
        pspDebugScreenPrintf("%s", shown);
    }

    if (!g_path) return;
    fd = sceIoOpen(g_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, line, (SceSize)n);
    sceIoClose(fd);

    /* The close is not enough on its own: the memory-stick driver keeps
     * the FAT's own updates in a cache, and a console that then switches
     * itself off -- which is how the decode path fails on hardware -- loses
     * them, and with them every line. The sync is what puts them on the
     * stick. */
    sceIoSync("ms0:", 0);
}

void trace_screen(int on) { g_no_screen = !on; }

void trace_defer(int on) {
    if (on) {
        g_deferring = 1;
        return;
    }
    g_deferring = 0;
    if (g_path && g_defer_len) {
        SceUID fd = sceIoOpen(g_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
        if (fd >= 0) {
            sceIoWrite(fd, g_defer, g_defer_len);
            sceIoClose(fd);
            sceIoSync("ms0:", 0);
        }
    }
    g_defer_len = 0;
    if (g_defer_lost) {
        unsigned lost = g_defer_lost;
        g_defer_lost  = 0;
        trace("(%u deferred lines did not fit and were lost)", lost);
    }
}
