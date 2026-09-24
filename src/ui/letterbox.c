/* render_letterbox_top: see ui/render.h for the contract and the reasoning
 * behind the clamp and the rounding direction.
 *
 * This is the entire zero-copy letterboxing trick in one line: because the
 * decoder writes wherever it is told, centring a short picture costs an
 * offset into the destination buffer rather than a blit. Kept in its own
 * file, with no PSP headers, so the host build (scripts/test.sh) can check
 * that arithmetic without pulling in sceDisplay or sceGe -- render.c, which
 * needs both, is compiled only for __PSP__ and never runs here. */

#include "ui/render.h"

#define PANEL_HEIGHT 272

int render_letterbox_top(int height) {
    if (height <= 0 || height >= PANEL_HEIGHT) return 0;
    return (PANEL_HEIGHT - height) / 2;
}
