/* The console implementation of ui/render.h.
 *
 * Only compiled for the console, for the same reason media/video_psp.c is:
 * the host check build (scripts/test.sh) globs every .c file under src/ui,
 * and there is no sceDisplay or sceGe on the development host to link
 * against. The one piece of this module that IS host-testable,
 * render_letterbox_top, lives in ui/letterbox.c precisely so it does not
 * have to be guarded out along with the rest.
 *
 * CHOICE: sceDisplaySetFrameBuf directly, not sceGu.
 *
 * sceGu exists to build and submit display lists to the Graphics Engine --
 * triangles, textures, blending state, a depth buffer. Nothing in this
 * module ever asks the GE to draw anything: the picture arrives already
 * rendered, written by the Media Engine (or, on the fallback path, copied by
 * the CPU) directly into a buffer the display controller reads from. Bolting
 * sceGu onto that would mean standing up a GE context and a display-list
 * pipeline to move zero polygons, in a program whose whole point is to spend
 * as little of a PSP-1000's cycles and RAM on the picture as possible.
 * sceDisplay's own API -- SetFrameBuf to choose which buffer is scanned out,
 * WaitVblankStart to time the swap -- is the minimal thing that does what
 * this module needs and nothing more.
 *
 * VRAM addressing: sceGeEdramGetAddr() returns VRAM's base address in the
 * CPU's normal (cached) address space. OR-ing in 0x40000000 selects the
 * uncached alias of the same physical memory -- the same trick the SDK's own
 * display samples use for a buffer the CPU writes directly. Everything this
 * module hands out through render_target() is an uncached pointer, on
 * purpose: both writers that ever touch it (the CPU, clearing letterbox
 * bands, and the Media Engine, decoding a picture) need their stores visible
 * to the display controller immediately, and neither wants a D-cache line
 * that could still be dirty when the hardware reads it. */
#if defined(__PSP__)

#include "ui/render.h"

#include <pspdisplay.h>
#include <pspdebug.h>
#include <pspge.h>
#include <stdint.h>
#include <string.h>

#define PANEL_W    480
#define PANEL_H    272
#define BUF_STRIDE 512 /* the power-of-two line stride the hardware wants */
#define BUF_PIXELS (BUF_STRIDE * PANEL_H)
#define BUF_BYTES  (BUF_PIXELS * 4u) /* 8888: 4 bytes per pixel */

/* The Media Engine's own destination, at the START of VRAM with the two
 * display buffers after it: this project's first hardware decode, with the
 * destination after the display buffers instead, failed with 0x80628002.
 * Taller than the panel on purpose: the engine writes whole 16-row
 * macroblocks, and 272-row destinations fail every access unit on hardware,
 * so 288 rows is headroom, not a picture size. 2 x 557,056 +
 * 589,824 = 1,703,936 bytes, inside the 2 MB of VRAM. */
#define DECODE_ROWS  288
#define DECODE_BYTES (BUF_STRIDE * DECODE_ROWS * 4u)

#define VRAM_UNCACHED_BIT 0x40000000u

/* Two buffers, both carved out of VRAM by address arithmetic rather than
 * claimed from any allocator -- VRAM is not partition memory, and there is
 * exactly one display, so a fixed layout costs nothing and cannot fragment. */
static uint32_t *g_buf[2]; /* uncached pointers, index 0 and 1 */
static uint32_t *g_decode; /* uncached pointer to the engine's destination */
static int       g_show;   /* which buffer is currently on screen */
static int       g_init;

/* The geometry the bands around each buffer were last cleared for. -1 means
 * "never cleared", which forces a clear the first time that buffer is
 * targeted regardless of what picture size arrives first. Tracked per
 * buffer, not globally, because the two buffers are cleared independently --
 * a geometry change clears the buffer about to be written, not the one
 * currently on screen. */
static int g_cleared_w[2] = {-1, -1};
static int g_cleared_h[2] = {-1, -1};

static uint32_t *vram_uncached(uint32_t byte_offset) {
    return (uint32_t *)(((uint32_t)sceGeEdramGetAddr() | VRAM_UNCACHED_BIT) + byte_offset);
}

/* The cached-alias pointer sceDisplaySetFrameBuf wants: it addresses the
 * same VRAM the display controller reads from either way, but every sample
 * and every other title passes the plain (non-uncached-bit) form, so this
 * strips the bit back off rather than assuming the firmware treats both
 * identically. */
static void *vram_cached(const uint32_t *uncached) {
    return (void *)((uint32_t)uncached & ~VRAM_UNCACHED_BIT);
}

int render_init(void) {
    int i;

    if (g_init) return 0;

    if (2u * BUF_BYTES + DECODE_BYTES > (uint32_t)sceGeEdramGetSize()) return -1;

    g_decode = vram_uncached(0);
    g_buf[0] = vram_uncached(DECODE_BYTES);
    g_buf[1] = vram_uncached(DECODE_BYTES + BUF_BYTES);
    memset(g_decode, 0, DECODE_BYTES);

    for (i = 0; i < 2; i++) {
        memset(g_buf[i], 0, BUF_BYTES);
        g_cleared_w[i] = -1;
        g_cleared_h[i] = -1;
    }

    sceDisplaySetMode(0, PANEL_W, PANEL_H);
    sceDisplaySetFrameBuf(vram_cached(g_buf[0]), BUF_STRIDE, PSP_DISPLAY_PIXEL_FORMAT_8888,
                          PSP_DISPLAY_SETBUF_IMMEDIATE);
    g_show = 0;
    g_init = 1;

    /* The debug text draws at the start of VRAM unless told otherwise, and
     * that is now the decoder's buffer, never shown. Pointed at the buffer
     * on screen instead -- 8888 and a 512 stride, which is the debug
     * screen's own format, so the text is legible over the picture area. */
    pspDebugScreenSetBase((u32 *)g_buf[0]);
    return 0;
}

void render_shutdown(void) {
    /* Nothing here was claimed from an allocator -- VRAM is fixed hardware,
     * not a partition block -- so there is nothing to give back. This
     * exists so callers have the same open/close shape as video_decoder.h
     * and so a later change that DOES need teardown has somewhere to put it
     * without every caller changing. */
    g_init = 0;
}

/* Paints the black bands around a `w`x`h` picture placed at (`left`, `top`)
 * in buffer `idx`. Called only when that buffer's last-cleared geometry
 * differs from this one -- see render_target() -- because the bands are
 * static for as long as the picture size does not change, and repainting
 * ~250 KB of VRAM every frame on a machine with no bandwidth to spare would
 * be paying every frame for something that only changes at a scene's start
 * (or never, for a fixed-size stream). */
static void clear_bands(int idx, int top, int left, int w, int h) {
    uint32_t *buf    = g_buf[idx];
    int       bottom = top + h;
    int       right  = left + w;
    int       row;

    /* Top and bottom bands run the full buffer stride, so each is one
     * contiguous span of rows and a single memset covers it -- including
     * the 32 columns per row past x=480 that the display never shows, which
     * ride along for free inside the same contiguous run. */
    if (top > 0) memset(buf, 0, (size_t)top * BUF_STRIDE * 4u);
    if (bottom < PANEL_H)
        memset(buf + (uint32_t)bottom * BUF_STRIDE, 0, (size_t)(PANEL_H - bottom) * BUF_STRIDE * 4u);

    /* Left and right bands run alongside the picture rows, so they are not
     * contiguous across rows and are cleared one row at a time. */
    for (row = top; row < bottom; row++) {
        uint32_t *r = buf + (uint32_t)row * BUF_STRIDE;
        if (left > 0) memset(r, 0, (size_t)left * 4u);
        if (right < PANEL_W) memset(r + right, 0, (size_t)(PANEL_W - right) * 4u);
    }
}

int render_target(int w, int h, void **pixels, int *stride_pixels) {
    int idx, top, left;

    if (!g_init || !pixels || !stride_pixels) return -1;

    /* Guard the geometry: this size comes from a fragmented-MP4 track this
     * program does not control (PROMPT.md section 35), and a width or
     * height past what the panel can show must be refused here rather than
     * trusted into a pointer offset. */
    if (w <= 0 || h <= 0 || w > PANEL_W || h > PANEL_H) return -1;

    top  = render_letterbox_top(h);
    left = (PANEL_W - w) / 2;

    /* Belt and braces: with the checks above this cannot actually overrun,
     * but the placement that would write outside the buffer is refused
     * explicitly rather than relied on to fall out of the arithmetic above
     * staying correct forever. */
    if (top < 0 || left < 0 || top + h > PANEL_H || left + w > PANEL_W) return -1;

    /* The buffer NOT currently on screen -- writing into the one being
     * scanned out would show a picture being built line by line. */
    idx = g_show ^ 1;

    if (g_cleared_w[idx] != w || g_cleared_h[idx] != h) {
        clear_bands(idx, top, left, w, h);
        g_cleared_w[idx] = w;
        g_cleared_h[idx] = h;
    }

    *pixels        = g_buf[idx] + (uint32_t)top * BUF_STRIDE + left;
    *stride_pixels = BUF_STRIDE;
    return 0;
}

void render_present(void) {
    int shown = g_show ^ 1; /* the buffer render_target() last handed out */

    /* NEXTFRAME latches the swap at the next vblank, which is what keeps it
     * from landing mid-scanout. This used to wait for that vblank as well,
     * which parked the whole decode loop for up to 16.7 ms a frame. Not
     * waiting leaves one hazard: writing the next picture into the buffer
     * that is still on screen until that vblank. At 25-30 fps the next
     * picture is 33-40 ms away, two vblanks later, so it does not arise;
     * a burst of frames inside one vblank would tear, not crash. */
    sceDisplaySetFrameBuf(vram_cached(g_buf[shown]), BUF_STRIDE, PSP_DISPLAY_PIXEL_FORMAT_8888,
                          PSP_DISPLAY_SETBUF_NEXTFRAME);
    g_show = shown;
}

void render_show_text(void) {
    if (!g_init) return;
    memset(g_buf[0], 0, BUF_BYTES);
    g_cleared_w[0] = -1;
    g_cleared_h[0] = -1;
    pspDebugScreenSetBase((u32 *)g_buf[0]); /* render_text_begin may have pointed it elsewhere */
    sceDisplaySetFrameBuf(vram_cached(g_buf[0]), BUF_STRIDE, PSP_DISPLAY_PIXEL_FORMAT_8888,
                          PSP_DISPLAY_SETBUF_NEXTFRAME);
    g_show = 0;
}

void render_invalidate(void) {
    g_cleared_w[0] = g_cleared_h[0] = -1;
    g_cleared_w[1] = g_cleared_h[1] = -1;
}

void render_text_begin(void) {
    int idx;

    if (!g_init) return;
    idx = g_show ^ 1;
    memset(g_buf[idx], 0, BUF_BYTES);
    pspDebugScreenSetBase((u32 *)g_buf[idx]);
    pspDebugScreenSetXY(0, 0);
}

void render_text_end(void) {
    int idx;

    if (!g_init) return;
    idx = g_show ^ 1;
    sceDisplaySetFrameBuf(vram_cached(g_buf[idx]), BUF_STRIDE, PSP_DISPLAY_PIXEL_FORMAT_8888,
                          PSP_DISPLAY_SETBUF_NEXTFRAME);
    g_show = idx;
    render_invalidate();
}

/* The debug font is 8 pixels tall and the panel 272: row 33 is the last. */
#define OSD_ROW     33
#define OSD_COLUMNS 67 /* not 68: a character in the last column of the last row wraps the cursor to row 0, and the debug screen clears that row */

void render_osd(const char *line) {
    uint32_t *buf;
    int       row, n;

    if (!g_init) return;
    buf = g_buf[g_show];
    for (row = OSD_ROW * 8 - 2; row < PANEL_H; row++) memset(buf + (uint32_t)row * BUF_STRIDE, 0, PANEL_W * 4u);

    pspDebugScreenSetBase((u32 *)buf);
    pspDebugScreenSetXY(0, OSD_ROW);
    for (n = 0; line && line[n] && n < OSD_COLUMNS; n++) pspDebugScreenPrintf("%c", line[n]);
}

int render_decode_target(void **pixels, int *stride_pixels, int *rows) {
    if (!g_init || !pixels || !stride_pixels || !rows) return -1;
    *pixels        = g_decode;
    *stride_pixels = BUF_STRIDE;
    *rows          = DECODE_ROWS;
    return 0;
}

#else

/* The host check build compiles every .c file under src/ui. There is no
 * sceDisplay here and nothing to stand in for one -- ui/letterbox.c is where
 * the host-testable part of this module lives -- but an empty translation
 * unit is not strictly legal C, hence the typedef (see media/video_psp.c for
 * the same pattern). */
typedef int render_psp_needs_a_psp;

#endif /* __PSP__ */
