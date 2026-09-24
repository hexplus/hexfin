/* A breadcrumb log on the memory stick, for the failures a screen cannot
 * report.
 *
 * The first hardware run of the decode path froze the console hard: black
 * screen, HOME dead, nothing drawn. Nothing on screen survives that, so this
 * writes each step to a file as it happens -- opened, appended, CLOSED, one
 * line at a time. The close is the point: FAT only records what was written
 * once the file is closed, and a line still sitting in a buffer when the
 * console locks up is a line that was never written. The last line in the
 * file is the last thing that finished.
 *
 * The file is ms0:/probe-<tag>.log, at the root of the memory stick where it
 * is easy to find, or probe-<tag>.log next to the EBOOT if the root cannot
 * be written. The tag names the build, so runs of different builds do not
 * overwrite each other. It is truncated at trace_init so every run of the
 * same build starts clean.
 *
 * Slow on purpose -- a memory-stick open/write/close per line -- so callers
 * log the first few samples in detail and then only per fragment. There is
 * also a hard cap on lines, so a caller that gets that wrong fills a few
 * hundred kB rather than the stick.
 *
 * Only compiled for the console. */
#ifndef PLATFORM_TRACE_H
#define PLATFORM_TRACE_H

/* Chooses the file and empties it. Until it has succeeded, trace() still
 * prints to the screen but writes nothing to the stick. */
void trace_init(const char *tag);

/* One line, printf-style, prefixed with milliseconds since trace_init. Also
 * printed on the debug screen's bottom row, replacing the one before. */
void trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* 1: from now on, lines are kept in memory instead of written -- for
 * playback, where a memory-stick write and sync on the main thread is long
 * enough to starve the audio (2026-09-24). 0: writes everything held, in one
 * go, and goes back to writing line by line. A crash while deferred loses
 * the held lines, which is the price; use it only around code already known
 * not to crash. */
void trace_defer(int on);

/* 0: lines go to the log only, not the screen's bottom row -- for the
 * player UI, which owns the whole screen. 1 (the default): both. */
void trace_screen(int on);

#endif /* PLATFORM_TRACE_H */
