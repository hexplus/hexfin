/* The seam between this program and the console.
 *
 * Everything above this header compiles on the development host, which is the
 * only way the parsers and the sync logic get tested at all: the real decoder
 * exists on the console and nowhere else. */
#ifndef PLATFORM_PSP_PLATFORM_H
#define PLATFORM_PSP_PLATFORM_H

/* Sets up the exit callback and whatever else must exist before anything
 * else runs. 0 on success. */
int platform_init(void);

/* True once the user has asked to leave through HOME. The main loop polls
 * this rather than exiting from the callback thread: exiting from inside the
 * callback is what leaves threads half-torn-down and the screen black. The
 * callback does force the exit itself, but only as a last resort, a few
 * seconds later, when the main thread has plainly not come back. */
int platform_exit_requested(void);

/* Registers one function for the exit callback to call the INSTANT HOME is
 * pressed, before anybody polls anything.
 *
 * Polling is the right shape for a main loop and the wrong shape for a thread
 * that is sitting inside a blocking call: design section 3.4 is explicit that
 * a main thread parked in an HTTP read must not notice HOME only when the
 * read returns. The hook is how the thing being waited on gets told to stop
 * -- net/http.c registers http_cancel, which shuts the socket down and makes
 * the recv fail immediately.
 *
 * The hook runs on the callback's own thread (an ordinary user thread parked
 * in sceKernelSleepThreadCB, not an interrupt handler), so it may make
 * syscalls. It must still be short and must not tear anything down: that is
 * the main thread's job, in the order main.c's teardown() sets out. Passing
 * 0 removes it. */
void platform_set_stop_hook(void (*fn)(void));

/* Microseconds since some fixed point. Monotonic within a run. */
unsigned long long platform_now_us(void);

/* The partition, in kB -- the pool sceKernelAllocPartitionMemory draws from,
 * which is where the decoder's buffers come from. The newlib heap is carved
 * out of this same partition on its first malloc, but separately, and is not
 * counted here. These two fail APART: a heap with megabytes free spread
 * across many holes cannot satisfy a large aligned claim, and the second
 * number is the one that decides whether a film starts. */
unsigned platform_free_kb(void);
unsigned platform_largest_free_kb(void);

/* Keeps the screen on and the console awake for as long as the program
 * runs, whether or not buttons are pressed -- playback is exactly the time
 * nobody touches them. Starts a small thread; returns its start result. */
int platform_keep_awake(void);

/* The status corner's text: the local time (in the console's own 12/24-hour
 * form) and the battery level, each only when asked for. See ui/text.h. */
void platform_status(char *dst, unsigned cap, int show_clock, int show_battery);

/* The system volume the volume buttons set: 0..PLATFORM_VOLUME_MAX, and
 * whether it is muted. Returns 0, or -1 when this console's firmware does
 * not let it be read -- then it never will be, and nothing should be shown.
 * Cheap enough to poll ten times a second. */
#define PLATFORM_VOLUME_MAX 30
int platform_volume(int *level, int *muted);

/* 1 when the volume or mute has changed since the last call -- the volume
 * buttons were pressed -- with the new values; 0 otherwise, including on
 * the very first call and when the volume cannot be read. Reads at most ten
 * times a second, so it can be called every loop. One tracker for the whole
 * program, so a list and the player never both report the same press. */
int platform_volume_changed(int *level, int *muted);

/* Returns to the XMB. Everything must already be torn down. */
void platform_exit(void);

#endif
