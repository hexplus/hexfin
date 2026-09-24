/* See ui/render.h.
 *
 * render_init/render_target/render_present all need sceDisplay and real
 * VRAM, so none of them can run here -- render.c is compiled only for
 * __PSP__. What CAN be checked on the host is the one piece of the render
 * path that is pure arithmetic: render_letterbox_top, in ui/letterbox.c.
 * Getting that formula wrong is invisible in a screenshot review (a few rows
 * of black shift the picture by a line nobody counts) and obvious the moment
 * it is checked against numbers taken off the real server, which is exactly
 * why it is pulled out here rather than left inline in render.c where only a
 * PSP could ever exercise it. */

#include "ui/render.h"

#include "test.h"

#include <stdio.h>

/* The fixture's own picture: 360x204 (docs/PHASE0_FINDINGS.md's direct
 * encode, PSP1000_BASELINE-adjacent fixture used elsewhere in this repo). */
static int t_fixture_picture(char *note, unsigned n) {
    int top = render_letterbox_top(204);
    if (top != 34) {
        snprintf(note, n, "360x204 centred at top=%d, expected 34", top);
        return 1;
    }
    return 0;
}

/* Measured from the real server: a 1920x1040 source, through our device
 * profile, transcodes to 480x260 (docs/PHASE0_FINDINGS.md section 7). */
static int t_measured_server_picture(char *note, unsigned n) {
    int top = render_letterbox_top(260);
    if (top != 6) {
        snprintf(note, n, "480x260 centred at top=%d, expected 6", top);
        return 1;
    }
    return 0;
}

/* Full panel height: flush against the top, no band at all. */
static int t_full_height_is_flush(char *note, unsigned n) {
    int top = render_letterbox_top(272);
    if (top != 0) {
        snprintf(note, n, "272-row picture offset by %d, expected 0", top);
        return 1;
    }
    return 0;
}

/* Taller than the panel: this cannot happen for a picture the decoder would
 * accept (video_psp.c refuses height > 272 at open), but render.h promises
 * the clamp rather than a negative offset, and a promise is worth its own
 * check regardless of who else already guards the input. */
static int t_taller_than_panel_clamps(char *note, unsigned n) {
    int top = render_letterbox_top(273);
    if (top != 0) {
        snprintf(note, n, "273-row picture offset by %d, expected 0 (clamped)", top);
        return 1;
    }
    return 0;
}

/* Degenerate input -- a zero height should never walk the destination
 * pointer backwards off the start of the buffer. */
static int t_zero_height_does_not_go_negative(char *note, unsigned n) {
    int top = render_letterbox_top(0);
    if (top != 0) {
        snprintf(note, n, "0-row picture offset by %d, expected 0", top);
        return 1;
    }
    return 0;
}

void test_render_register(void) {
    test_add("render", "the fixture's 360x204 picture is centred at row 34", t_fixture_picture);
    test_add("render", "the measured 480x260 server picture is centred at row 6", t_measured_server_picture);
    test_add("render", "a full 272-row picture is flush, no band", t_full_height_is_flush);
    test_add("render", "a picture taller than the panel clamps to 0, not negative", t_taller_than_panel_clamps);
    test_add("render", "a degenerate zero height does not produce a negative offset", t_zero_height_does_not_go_negative);
}
