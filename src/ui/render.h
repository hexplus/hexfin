/* The seam between a decoded picture and the panel it lands on.
 *
 * The panel is 480x272, 32-bit 8888, and the hardware wants a line stride
 * that is a power of two -- 512 pixels, not 480 -- so every buffer this
 * module owns is 512 pixels wide and only the left 480 of each row are ever
 * shown.
 *
 * Design section 3.2 hoped for zero copy: decode straight into the buffer
 * being shown, in the panel's own RGB565. The first hardware runs
 * (2026-09-23) ruled that shape out for now. sceMpegAvcDecode writes up to
 * four pictures per access unit, whole macroblocks at a time, so a
 * destination needs rows past the picture and cannot be a letterboxed
 * window inside a display buffer; and the configuration that works is 8888
 * output into a VRAM buffer of its own, copied to the panel. So there are two
 * things here: the engine's own destination, render_decode_target(), and
 * render_target(), which says where that picture is copied so it appears
 * centred.
 *
 * render_letterbox_top is pulled out as pure arithmetic on purpose: it is
 * the one part of "where does this picture go" that has nothing to do with
 * sceGu, sceDisplay or VRAM, and it is wrong exactly as often whether or not
 * a PSP is attached. It lives in its own host-compilable file (letterbox.c)
 * so the host build can check it without linking a single PSP header; see
 * scripts/test.sh and tests/test_render.c.
 *
 * No platform headers, so this header itself compiles on the host -- only
 * render.c (the sceDisplay/VRAM implementation) is PSP-only. */
#ifndef UI_RENDER_H
#define UI_RENDER_H

/* Sets up the two display buffers and the decoder's destination. Safe to
 * call more than once; the second call is a no-op. Returns 0 on success,
 * non-zero if the display could not be configured. */
int render_init(void);

/* Releases what render_init claimed. Safe to call when init failed or was
 * never called, for the same reason video_decoder_close() is: the teardown
 * path must be the one thing that cannot itself break. */
void render_shutdown(void);

/* Where should a `w`-by-`h` picture be copied so that it appears centred on
 * the panel? The address is of 32-bit pixels.
 *
 * Fills `*pixels` with the address inside the buffer NOT currently on
 * screen, and `*stride_pixels` with the stride (in pixels, matching
 * video_decoder.h's convention) that address was computed against -- always
 * 512, but a caller should read it from here rather than assume it, in case
 * this module ever backs onto something else.
 *
 * Refuses (returns non-zero, leaving `*pixels`/`*stride_pixels` untouched)
 * when render_init() has not succeeded, when `w` or `h` is not positive or
 * exceeds the panel (480x272 -- PROMPT.md section 35 is why this is checked
 * rather than trusted: the geometry comes off a network stream), or when the
 * computed placement would write outside the buffer.
 *
 * A caller that gets a non-zero return has one correct move: skip the copy.
 * The picture was still decoded; it is simply not shown. */
int render_target(int w, int h, void **pixels, int *stride_pixels);

/* The Media Engine's destination: an uncached VRAM buffer of `*rows` rows of
 * `*stride_pixels` 32-bit pixels, separate from both display buffers and
 * never shown directly. Refuses (non-zero) before render_init() succeeds. */
int render_decode_target(void **pixels, int *stride_pixels, int *rows);

/* Flips: the buffer render_target() last handed out becomes the one shown,
 * and the next render_target() call will hand out the other one. Does not
 * wait: the swap takes effect at the next vblank. */
void render_present(void);

/* Blanks the buffer the debug text is drawn into and puts it on screen: for
 * a text report after pictures have been shown. */
void render_show_text(void);

/* How many rows of black sit above a `height`-row picture centred on the
 * 272-row panel.
 *
 * (272 - height) / 2, clamped to 0 for height >= 272 so a picture at least
 * as tall as the panel is flush rather than pushed off the top, and for
 * height <= 0 so a degenerate reading never produces a negative offset that
 * would move the destination pointer before the start of the buffer.
 *
 * The division rounds down deliberately: an odd remainder leaves its spare
 * row at the BOTTOM, where the panel's own edge (and, on a real picture, the
 * more forgiving eye for a bottom-heavy frame than a top-heavy one) hides a
 * single stray line better than the top would. */
int render_letterbox_top(int height);

/* A screen of text, drawn off-screen and shown whole: text_begin clears the
 * buffer not on screen and points the debug-screen text at it, text_end
 * shows it. Between them, pspDebugScreenSetXY/Printf draw the page. Menus
 * redraw with this rather than onto the visible buffer, which flickers. */
void render_text_begin(void);
void render_text_end(void);

/* One line of text across the bottom of the picture on screen now, on a
 * dark band -- the pause screen, drawn over the paused frame. */
void render_osd(const char *line);

/* Forgets which buffers have their letterbox bands cleared, so the next
 * picture repaints them -- after text was drawn over a picture. */
void render_invalidate(void);

#endif
