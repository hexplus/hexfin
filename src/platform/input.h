/* The buttons, as presses rather than states.
 *
 * A list moved one entry per frame a button is held would be unusable, and
 * one entry per press would make a 3000-item library a chore, so the
 * direction buttons and the shoulders repeat while held: once on the press,
 * again after 400 ms, then every 80 ms. The rest fire once per press.
 *
 * HOME is not here: the firmware keeps it, and it arrives through the exit
 * callback (platform/psp_platform.h).
 *
 * Values are the PSP_CTRL_* bits from <pspctrl.h>. Only compiled for the
 * console. */
#ifndef PLATFORM_INPUT_H
#define PLATFORM_INPUT_H

/* Buttons pressed since the last call, plus repeats of the ones held. */
unsigned input_pressed(void);

/* Buttons held right now, without consuming any presses. */
unsigned input_held(void);

#endif /* PLATFORM_INPUT_H */
