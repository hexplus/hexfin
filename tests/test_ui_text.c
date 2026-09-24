/* See ui/text.h and ui/menu.h. */

#include "ui/menu.h"
#include "ui/text.h"

#include "test.h"

#include <stdio.h>
#include <string.h>

static int t_ascii_folding(char *note, unsigned n) {
    static const struct {
        const char *in, *out;
    } cases[] = {
        {"Pok\xC3\xA9mon", "Pokemon"},
        {"Espa\xC3\xB1ol \xC2\xBF" "Qu\xC3\xA9?", "Espanol ?Que?"},
        {"\xE2\x80\x9CQuoted\xE2\x80\x9D \xE2\x80\x94 it\xE2\x80\x99s", "\"Quoted\" - it's"},
        {"Stra\xC3\x9F" "e", "Strasse"},
        {"\xE6\x97\xA5\xE6\x9C\xAC", "??"}, /* CJK: no ASCII, one '?' a character */
        {"\xF0\x9F\x98\x80!", "?!"},       /* an emoji, four bytes */
        {"cut \xC3", "cut ?"},             /* a sequence the string ends inside */
        {"bad \x80\xBF x", "bad ?? x"},    /* stray continuation bytes */
        {"tab\there", "tab here"},
    };
    unsigned i;
    char     out[64];

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        text_to_ascii(cases[i].in, out, sizeof out);
        if (strcmp(out, cases[i].out) != 0) {
            snprintf(note, n, "case %u folded to \"%s\", not \"%s\"", i, out, cases[i].out);
            return 1;
        }
    }
    text_to_ascii("\xC3\xA6\xC3\xA6\xC3\xA6", out, 4); /* "aeaeae" into 3 characters */
    if (strcmp(out, "aea") != 0) {
        snprintf(note, n, "a small buffer held \"%s\"", out);
        return 1;
    }
    return 0;
}

static int t_fit_and_ticks(char *note, unsigned n) {
    char out[32];

    text_fit("abc", 6, out);
    if (strcmp(out, "abc   ") != 0) {
        snprintf(note, n, "short text padded to \"%s\"", out);
        return 1;
    }
    text_fit("abcdefghij", 6, out);
    if (strcmp(out, "abc...") != 0) {
        snprintf(note, n, "long text cut to \"%s\"", out);
        return 1;
    }
    text_ticks(14770000000ull, out, sizeof out);
    if (strcmp(out, "24:37") != 0) {
        snprintf(note, n, "24 minutes read \"%s\"", out);
        return 1;
    }
    text_ticks(65410700000ull, out, sizeof out);
    if (strcmp(out, "1:49:01") != 0) {
        snprintf(note, n, "an hour and more read \"%s\"", out);
        return 1;
    }
    return 0;
}

static int t_status(char *note, unsigned n) {
    static const struct {
        int         clock, hour, minute, h24, batt, pct, chg;
        const char *want;
    } cases[] = {
        {1, 14, 5, 1, 1, 87, 0, "14:05  87%"},
        {1, 14, 5, 0, 0, 87, 0, "2:05 PM"},
        {1, 0, 7, 0, 0, 0, 0, "12:07 AM"},
        {1, 12, 0, 0, 0, 0, 0, "12:00 PM"},
        {0, 14, 5, 1, 1, 40, 1, "40%+"},
        {0, 0, 0, 1, 1, -1, 0, "AC"},
        {0, 0, 0, 1, 0, 50, 0, ""},
        {1, 25, 0, 1, 1, 150, 0, "100%"}, /* a nonsense hour is left out, a nonsense level capped */
    };
    unsigned i;
    char     out[32];

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        text_status(out, sizeof out, cases[i].clock, cases[i].hour, cases[i].minute, cases[i].h24, cases[i].batt,
                    cases[i].pct, cases[i].chg);
        if (strcmp(out, cases[i].want) != 0) {
            snprintf(note, n, "case %u gave \"%s\", not \"%s\"", i, out, cases[i].want);
            return 1;
        }
    }
    return 0;
}

static int t_volume(char *note, unsigned n) {
    char out[64];

    text_volume(out, sizeof out, 15, 30, 0);
    if (strcmp(out, "Volume [##########----------] 15/30") != 0) {
        snprintf(note, n, "half volume read \"%s\"", out);
        return 1;
    }
    text_volume(out, sizeof out, 0, 30, 0);
    if (strcmp(out, "Volume [--------------------] 0/30") != 0) {
        snprintf(note, n, "no volume read \"%s\"", out);
        return 1;
    }
    text_volume(out, sizeof out, 99, 30, 0);
    if (strcmp(out, "Volume [####################] 30/30") != 0) {
        snprintf(note, n, "an out-of-range level read \"%s\"", out);
        return 1;
    }
    text_volume(out, sizeof out, 12, 30, 1);
    if (strcmp(out, "Volume: muted") != 0) {
        snprintf(note, n, "muted read \"%s\"", out);
        return 1;
    }
    return 0;
}

static int t_menu(char *note, unsigned n) {
    menu m;

    menu_init(&m, 100, 10);
    menu_move(&m, 1);
    menu_move(&m, 12);
    if (m.sel != 13 || m.top != 4) {
        snprintf(note, n, "after moving down 13: sel %d top %d", m.sel, m.top);
        return 1;
    }
    menu_move(&m, -50);
    if (m.sel != 0 || m.top != 0) {
        snprintf(note, n, "a page up past the start left sel %d top %d", m.sel, m.top);
        return 1;
    }
    menu_move(&m, -1);
    if (m.sel != 99 || m.top != 90) {
        snprintf(note, n, "up from the first entry went to %d (top %d), not the last", m.sel, m.top);
        return 1;
    }
    menu_move(&m, 1);
    if (m.sel != 0) {
        snprintf(note, n, "down from the last entry went to %d, not the first", m.sel);
        return 1;
    }
    menu_select(&m, 95);
    menu_set_count(&m, 20);
    if (m.sel != 19 || m.top != 10) {
        snprintf(note, n, "shrinking the list left sel %d top %d", m.sel, m.top);
        return 1;
    }
    menu_init(&m, 0, 10);
    menu_move(&m, 1);
    if (m.sel != 0 || m.top != 0) {
        snprintf(note, n, "an empty list moved");
        return 1;
    }
    menu_init(&m, 3, 10);
    menu_move(&m, 5);
    if (m.sel != 2 || m.top != 0) {
        snprintf(note, n, "a short list scrolled: sel %d top %d", m.sel, m.top);
        return 1;
    }
    return 0;
}

void test_ui_text_register(void) {
    test_add("ui", "UTF-8 names fold to ASCII the font can draw", t_ascii_folding);
    test_add("ui", "text is fitted to a width, and times formatted", t_fit_and_ticks);
    test_add("ui", "a menu's selection and scrolling stay in bounds", t_menu);
    test_add("ui", "the clock and battery corner reads as the settings ask", t_status);
    test_add("ui", "the volume bar is drawn to scale, and says when muted", t_volume);
}
