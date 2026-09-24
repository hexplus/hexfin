/* Text for the PSP's debug-screen font, which draws ASCII and nothing else.
 *
 * Library names are UTF-8 and often not English: "Episodio 1" is fine,
 * "Perdidos en el espacio" is fine, "Pokémon" is not. So text is folded to
 * ASCII on its way to the screen -- accented Latin letters to their base
 * letter, typographic quotes and dashes to their plain forms, and anything
 * else to '?' -- rather than drawn as the raw bytes, which the font shows as
 * two or three wrong symbols per character.
 *
 * Pure, so the host build checks it: the bytes are a server's. */
#ifndef UI_TEXT_H
#define UI_TEXT_H

#include <stddef.h>
#include <stdint.h>

/* `src` (UTF-8, possibly malformed) folded to printable ASCII in `dst`,
 * NUL-terminated. Returns the length written. */
size_t text_to_ascii(const char *src, char *dst, size_t cap);

/* Exactly `width` characters of `src` into `dst` (which holds width + 1):
 * padded with spaces, or cut with "..." at the end when longer. */
void text_fit(const char *src, unsigned width, char *dst);

/* Jellyfin ticks (10 000 000 a second) as "m:ss" or "h:mm:ss". */
void text_ticks(uint64_t ticks, char *dst, size_t cap);

/* The status corner: the time of day and the battery, each only when asked
 * for, joined by two spaces -- "14:05  87%", "2:05 PM", "87%+" (charging),
 * "AC" (no battery fitted). `h24` follows the console's own 12/24-hour
 * setting. `percent` < 0 means there is no battery to read. "" when neither
 * is shown. */
void text_status(char *dst, size_t cap, int show_clock, int hour, int minute, int h24, int show_battery,
                 int percent, int charging);

/* The volume as a bar: "Volume [##########----------] 18/30", or
 * "Volume: muted". `level` is clamped to 0..`max`. */
void text_volume(char *dst, size_t cap, int level, int max, int muted);

#endif /* UI_TEXT_H */
