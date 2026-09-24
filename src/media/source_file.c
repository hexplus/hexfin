/* The memory-stick implementation of media/source.h.
 *
 * This is the code that used to sit inline in main.c, moved rather than
 * rewritten: same sceIoOpen, same sceIoRead, same meaning for a zero-length
 * read. What it gains is a reason string, because the interface has one and
 * the probe now reports whichever source it used through the same line --
 * "the file could not be opened" and "the socket refused" have to arrive at
 * draw_screen() by the same route or the screen grows a special case per
 * source.
 *
 * Only compiled for the console. The host check build (scripts/test.sh)
 * compiles every .c file under src/media, and there is no sceIoRead there;
 * the guard is the same one media/video_psp.c uses for the same reason. */
#if defined(__PSP__)

#include "media/source.h"

#include <pspiofilemgr.h>

#include <stddef.h>

/* Long enough for the longest sentence below plus the firmware's own code in
 * hex. The same reasoning as media/video_psp.c's VIDEO_ERRLEN: a message
 * truncated one word before the number is a message that wasted the screen
 * line it took. */
#define FILE_ERRLEN 160

static SceUID g_fd = -1;
static char   g_err[FILE_ERRLEN];

/* Formatted by hand rather than with snprintf, for the reason
 * media/video_psp.c gives at length: newlib's printf is a large dependency
 * to pull in for eight hex digits. */
static void set_err(const char *msg, int code, int with_code) {
    static const char hex[] = "0123456789ABCDEF";
    size_t            n     = 0;
    int               i;

    while (msg[n] && n + 1 < FILE_ERRLEN) {
        g_err[n] = msg[n];
        n++;
    }
    if (with_code) {
        const char *suffix = " (firmware returned 0x";
        size_t      s      = 0;
        while (suffix[s] && n + 1 < FILE_ERRLEN) g_err[n++] = suffix[s++];
        for (i = 28; i >= 0 && n + 1 < FILE_ERRLEN; i -= 4)
            g_err[n++] = hex[((unsigned)code >> i) & 0xFu];
        if (n + 1 < FILE_ERRLEN) g_err[n++] = ')';
    }
    g_err[n] = 0;
}

static int file_open(const char *location) {
    if (!location) {
        set_err("no path was given to the file source", 0, 0);
        return SOURCE_ERR_ARG;
    }
    if (g_fd >= 0) {
        set_err("the file source was opened twice without a close in between", 0, 0);
        return SOURCE_ERR_STATE;
    }

    g_fd = sceIoOpen(location, PSP_O_RDONLY, 0);
    if (g_fd < 0) {
        set_err("the file could not be opened", (int)g_fd, 1);
        return SOURCE_ERR_OPEN;
    }

    g_err[0] = 0;
    return SOURCE_OK;
}

static int file_read(uint8_t *dst, uint32_t cap, uint32_t *out_got) {
    int n;

    if (out_got) *out_got = 0;
    if (!dst || cap == 0 || !out_got) {
        set_err("the file source was asked to read into nothing", 0, 0);
        return SOURCE_ERR_ARG;
    }
    if (g_fd < 0) {
        set_err("the file source was read before it was opened", 0, 0);
        return SOURCE_ERR_STATE;
    }

    n = sceIoRead(g_fd, dst, (SceSize)cap);
    if (n < 0) {
        set_err("a read from the memory stick failed", n, 1);
        return SOURCE_ERR_IO;
    }
    /* sceIoRead answers 0 only at the end of the file, so this is the clean
     * end of the stream rather than the "nothing arrived yet" a socket can
     * report -- which is why the two sources need separate implementations
     * of this call and not a shared one with a flag. */
    if (n == 0) return SOURCE_ERR_EOF;

    *out_got = (uint32_t)n;
    return SOURCE_OK;
}

static void file_close(void) {
    if (g_fd >= 0) {
        sceIoClose(g_fd);
        g_fd = -1;
    }
}

static const char *file_error(void) { return g_err; }

const media_source source_file = {"file", file_open, file_read, file_close, file_error};

#endif /* __PSP__ */
