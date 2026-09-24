/* See ui/text.h. */
#include "ui/text.h"

#include <stdio.h>
#include <string.h>

/* U+00C0 .. U+00FF, folded. Two characters where one letter becomes two
 * (AE, ss, th); a space means "no good ASCII, use '?'". */
static const char *const k_latin1[64] = {
    "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
    "D", "N", "O", "O", "O", "O", "O",  "x", "O", "U", "U", "U", "U", "Y", "Th", "ss",
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
    "d", "n", "o", "o", "o", "o", "o",  "/", "o", "u", "u", "u", "u", "y", "th", "y",
};

static const char *fold(unsigned cp) {
    if (cp >= 0xC0 && cp <= 0xFF) return k_latin1[cp - 0xC0];
    switch (cp) {
        case 0xA0: return " ";           /* no-break space */
        case 0xA1: return "!";           /* inverted exclamation */
        case 0xBF: return "?";           /* inverted question */
        case 0xAB: return "<<";
        case 0xBB: return ">>";
        case 0xB7: return ".";
        case 0x2018: case 0x2019: case 0x201B: case 0x2032: return "'";
        case 0x201C: case 0x201D: case 0x2033: return "\"";
        case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return "-";
        case 0x2026: return "...";
        case 0x2022: return "*";
        case 0x0152: return "OE";
        case 0x0153: return "oe";
        case 0x0160: return "S";
        case 0x0161: return "s";
        case 0x017D: return "Z";
        case 0x017E: return "z";
        case 0x0178: return "Y";
        default: return "?";
    }
}

size_t text_to_ascii(const char *src, char *dst, size_t cap) {
    const unsigned char *s = (const unsigned char *)src;
    size_t               n = 0;

    if (!dst || cap == 0) return 0;
    if (!src) {
        dst[0] = 0;
        return 0;
    }

    while (*s && n + 1 < cap) {
        unsigned    cp;
        int         extra, i;
        const char *rep;

        if (*s < 0x80) {
            dst[n++] = (*s >= 0x20 && *s < 0x7F) ? (char)*s : ' ';
            s++;
            continue;
        }
        if ((*s & 0xE0) == 0xC0) {
            cp    = *s & 0x1Fu;
            extra = 1;
        } else if ((*s & 0xF0) == 0xE0) {
            cp    = *s & 0x0Fu;
            extra = 2;
        } else if ((*s & 0xF8) == 0xF0) {
            cp    = *s & 0x07u;
            extra = 3;
        } else {
            s++; /* a stray continuation byte, or not UTF-8 at all */
            dst[n++] = '?';
            continue;
        }
        s++;
        for (i = 0; i < extra; i++) {
            if ((*s & 0xC0) != 0x80) break; /* cut short: stop at the byte that broke it, never past a NUL */
            cp = (cp << 6) | (*s & 0x3Fu);
            s++;
        }
        rep = (i == extra) ? fold(cp) : "?";
        while (*rep && n + 1 < cap) dst[n++] = *rep++;
    }
    dst[n] = 0;
    return n;
}

void text_fit(const char *src, unsigned width, char *dst) {
    size_t len = strlen(src);

    if (len <= width) {
        memcpy(dst, src, len);
        memset(dst + len, ' ', width - len);
    } else if (width >= 3) {
        memcpy(dst, src, width - 3);
        memcpy(dst + width - 3, "...", 3);
    } else {
        memcpy(dst, src, width);
    }
    dst[width] = 0;
}

void text_status(char *dst, size_t cap, int show_clock, int hour, int minute, int h24, int show_battery,
                 int percent, int charging) {
    char clock[16] = "", batt[16] = "";

    if (!dst || cap == 0) return;
    if (show_clock && hour >= 0 && hour < 24 && minute >= 0 && minute < 60) {
        if (h24) snprintf(clock, sizeof clock, "%02d:%02d", hour, minute);
        else snprintf(clock, sizeof clock, "%d:%02d %s", hour % 12 ? hour % 12 : 12, minute, hour < 12 ? "AM" : "PM");
    }
    if (show_battery) {
        if (percent < 0) snprintf(batt, sizeof batt, "AC");
        else snprintf(batt, sizeof batt, "%d%%%s", percent > 100 ? 100 : percent, charging ? "+" : "");
    }
    snprintf(dst, cap, "%s%s%s", clock, clock[0] && batt[0] ? "  " : "", batt);
}

void text_volume(char *dst, size_t cap, int level, int max, int muted) {
    char bar[21];
    int  i, filled;

    if (!dst || cap == 0) return;
    if (muted) {
        snprintf(dst, cap, "Volume: muted");
        return;
    }
    if (max <= 0) max = 1;
    if (level < 0) level = 0;
    if (level > max) level = max;
    filled = (level * 20 + max / 2) / max;
    for (i = 0; i < 20; i++) bar[i] = i < filled ? '#' : '-';
    bar[20] = 0;
    snprintf(dst, cap, "Volume [%s] %d/%d", bar, level, max);
}

void text_ticks(uint64_t ticks, char *dst, size_t cap) {
    uint64_t s = ticks / 10000000u;
    unsigned h = (unsigned)(s / 3600u), m = (unsigned)((s / 60u) % 60u), sec = (unsigned)(s % 60u);

    if (h) snprintf(dst, cap, "%u:%02u:%02u", h, m, sec);
    else snprintf(dst, cap, "%u:%02u", m, sec);
}
