/* See app/app.h. */
#if defined(__PSP__)

#include "app/app.h"

#include "jellyfin/items.h"
#include "jellyfin/jellyfin.h"
#include "media/audio_decoder.h"
#include "media/video_decoder.h"
#include "net/http.h"
#include "net/http_parse.h"
#include "net/wifi.h"
#include "platform/input.h"
#include "platform/psp_platform.h"
#include "platform/trace.h"
#include "player/player.h"
#include "ui/menu.h"
#include "ui/render.h"
#include "ui/text.h"

#include <pspctrl.h>
#include <pspdebug.h>
#include <pspkernel.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- layout
 *
 * The debug font is 8x8 on a 480x272 panel: 68 columns that are fully
 * visible (480 / 7 would be 68.5 with its spacing) and 34 rows. */
#define COLS       68
#define ROW_TITLE  0
#define ROW_LIST   2
#define LIST_ROWS  28
#define ROW_NOTICE 31
#define ROW_HELP   33

/* 0xAABBGGRR */
#define COLOR_TEXT    0xFFFFFFFFu
#define COLOR_DIM     0xFF909090u
#define COLOR_ACCENT  0xFFFFC060u
#define COLOR_WARN    0xFF60C0FFu
#define COLOR_SEL_BG  0xFF7A3C00u
#define COLOR_BG      0xFF000000u

/* How deep the folders go: library, series, season is three; the rest is
 * room for folders of folders. */
#define MAX_DEPTH 8

static const app_env *g_env;

/* snprintf, for text that is shown to a person and may be cut to fit: most
 * of a title is better than none, and GCC's truncation warnings are about
 * exactly the cut this wants. */
static void fmt(char *dst, size_t cap, const char *f, ...) __attribute__((format(printf, 3, 4)));
static void fmt(char *dst, size_t cap, const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    vsnprintf(dst, cap, f, ap);
    va_end(ap);
}

/* ------------------------------------------------------------ drawing */

static void put(int col, int row, unsigned fg, unsigned bg, const char *ascii) {
    pspDebugScreenSetTextColor(fg);
    pspDebugScreenSetBackColor(bg);
    pspDebugScreenSetXY(col, row);
    pspDebugScreenPrintf("%s", ascii);
    pspDebugScreenSetTextColor(COLOR_TEXT);
    pspDebugScreenSetBackColor(COLOR_BG);
}

/* UTF-8 in, fitted to `width` columns, drawn. */
static void put_text(int col, int row, unsigned width, unsigned fg, unsigned bg, const char *utf8) {
    char ascii[256], fitted[COLS + 1];

    if (width > COLS) width = COLS;
    text_to_ascii(utf8, ascii, sizeof ascii);
    text_fit(ascii, width, fitted);
    put(col, row, fg, bg, fitted);
}

/* Word-wrapped from `row`, at most `max_rows`. Returns the rows used. */
static int put_wrapped(int row, int max_rows, unsigned fg, const char *utf8) {
    char        ascii[512];
    const char *p = ascii;
    int         used = 0;

    text_to_ascii(utf8, ascii, sizeof ascii);
    while (*p && used < max_rows) {
        char line[COLS + 1];
        int  n = (int)strlen(p), cut = n;

        if (n > COLS) {
            cut = COLS;
            while (cut > 0 && p[cut] != ' ') cut--;
            if (cut == 0) cut = COLS;
        }
        memcpy(line, p, (size_t)cut);
        line[cut] = 0;
        put(0, row + used, fg, COLOR_BG, line);
        used++;
        p += cut;
        while (*p == ' ') p++;
    }
    return used;
}

/* The clock and battery as last drawn, so a screen can tell when they
 * have changed and it is worth drawing again. */
static char               g_status[32];
static unsigned long long g_status_checked_us;

static void read_status(char *out, unsigned cap) {
    jf_config *cfg = jf_config_get();
    platform_status(out, cap, cfg->show_clock, cfg->show_battery);
}

/* 1 once a second at most, when the corner would now read differently: a
 * new minute, a battery percent gone. Screens that wait for buttons call it
 * in their loop and redraw on 1. */
static int status_changed(void) {
    char               now[sizeof g_status];
    unsigned long long t = platform_now_us();

    if (t - g_status_checked_us < 1000000ull) return 0;
    g_status_checked_us = t;
    read_status(now, sizeof now);
    return strcmp(now, g_status) != 0;
}

/* The title row: where you are on the left; on the right, the signed-in
 * user and -- when Settings has them on -- the time and the battery. */
static void draw_title(const char *where) {
    char     right[96];
    unsigned n;

    read_status(g_status, sizeof g_status);
    g_status_checked_us = platform_now_us();
    fmt(right, sizeof right, "%s%s%s", jf_user_name(), jf_user_name()[0] && g_status[0] ? "   " : "", g_status);
    n = (unsigned)strlen(right);
    if (n > 40) n = 40;

    put_text(0, ROW_TITLE, COLS - n - 2, COLOR_ACCENT, COLOR_BG, where);
    if (n) put_text(COLS - n, ROW_TITLE, n, COLOR_DIM, COLOR_BG, right);
    put(0, ROW_TITLE + 1, COLOR_DIM, COLOR_BG,
        "--------------------------------------------------------------------");
}

/* A whole screen of one message: connecting, signing in, a failure. */
static void message_screen(const char *title, const char *line1, const char *line2, const char *help) {
    int row = 8;

    render_text_begin();
    draw_title("Hexfin");
    put_text(0, row, COLS, COLOR_ACCENT, COLOR_BG, title);
    row += 2;
    if (line1 && line1[0]) row += put_wrapped(row, 6, COLOR_TEXT, line1) + 1;
    if (line2 && line2[0]) put_wrapped(row, 6, COLOR_DIM, line2);
    if (help) put_text(0, ROW_HELP, COLS - 1, COLOR_DIM, COLOR_BG, help);
    render_text_end();
}

/* Shows a failure and waits for X (try again: 1) or HOME (0). */
static int failure_screen(const char *title, const char *why) {
    message_screen(title, why, NULL, "X try again     HOME quit");
    input_pressed();
    while (!platform_exit_requested()) {
        if (input_pressed() & PSP_CTRL_CROSS) return 1;
        sceKernelDelayThread(20 * 1000);
    }
    return 0;
}

/* --------------------------------------------------------- Wi-Fi, sign-in */

static int connect_wifi(void) {
    for (;;) {
        message_screen("Connecting to Wi-Fi...", "Using the first connection saved in the PSP's network settings.",
                       NULL, "HOME quit");
        if (wifi_connect(g_env->ap_config, g_env->wifi_timeout_ms) == 0) {
            trace("app: wifi up, %s", wifi_ip());
            return 1;
        }
        trace("app: wifi failed: %s", wifi_error());
        wifi_shutdown();
        if (platform_exit_requested() || !failure_screen("Could not connect to Wi-Fi", wifi_error())) return 0;
    }
}

static void sign_in_show(const char *line1, const char *line2) {
    message_screen("Sign in with Quick Connect", line1, line2, "HOME quit");
}

static int sign_in(void) {
    for (;;) {
        message_screen("Signing in...", jf_server(g_env->default_server), NULL, "HOME quit");
        if (jf_sign_in(jf_server(g_env->default_server), g_env->version, sign_in_show) == 0) return 1;
        trace("app: sign-in failed: %s", jf_error());
        if (platform_exit_requested() || !failure_screen("Could not sign in", jf_error())) return 0;
    }
}

/* ------------------------------------------------------------- the lists */

/* The home screen's own entries, told apart from library items by a type
 * no server uses. */
#define TYPE_RESUME   "@resume"
#define TYPE_SETTINGS "@settings"

typedef struct {
    int          is_home;
    jf_list_kind kind;
    char         parent[JF_ITEM_ID_MAX];
    char         title[JF_ITEM_NAME_MAX];
    int          start, count, total;
    int          loaded;
    jf_item      items[JF_PAGE_SIZE + 2];
    menu         m;
} level;

static level g_levels[MAX_DEPTH];
static int   g_depth; /* index of the level on screen */
static char  g_notice[160];
static unsigned g_notice_color = COLOR_TEXT;

static void notice(unsigned color, const char *msg) {
    fmt(g_notice, sizeof g_notice, "%s", msg ? msg : "");
    g_notice_color = color;
    /* A warning on screen is gone at the next press; the log keeps it. */
    if (color == COLOR_WARN && g_notice[0]) trace("app: %s", g_notice);
}

static int has_prev(const level *lv) { return !lv->is_home && lv->start > 0; }
static int has_next(const level *lv) { return !lv->is_home && lv->start + lv->count < lv->total; }
static int row_count(const level *lv) { return lv->count + has_prev(lv) + has_next(lv); }

/* What a row of the menu is: -1 the previous page, -2 the next, or an index
 * into items. */
static int row_item(const level *lv, int row) {
    if (has_prev(lv)) {
        if (row == 0) return -1;
        row--;
    }
    if (row >= lv->count) return -2;
    return row;
}

static void add_home_entry(level *lv, const char *type, const char *name) {
    jf_item *it = &lv->items[lv->count++];
    memset(it, 0, sizeof *it);
    fmt(it->type, sizeof it->type, "%s", type);
    fmt(it->name, sizeof it->name, "%s", name);
    fmt(it->id, sizeof it->id, "%s", type);
    it->is_folder = 1;
}

static int load_home(level *lv) {
    static jf_item views[JF_PAGE_SIZE];
    int            n = 0, total = 0, i;

    lv->count = 0;
    add_home_entry(lv, TYPE_RESUME, "Continue Watching");
    if (jf_list(JF_LIST_VIEWS, NULL, 0, JF_PAGE_SIZE, views, &n, &total) != 0) {
        notice(COLOR_WARN, jf_error());
    } else {
        for (i = 0; i < n && lv->count < JF_PAGE_SIZE; i++)
            if (jf_view_has_video(&views[i])) lv->items[lv->count++] = views[i];
    }
    add_home_entry(lv, TYPE_SETTINGS, "Settings");
    lv->total  = lv->count;
    lv->loaded = 1;
    menu_set_count(&lv->m, row_count(lv));
    return 0;
}

static int load_level(level *lv) {
    int n = 0, total = 0;

    if (lv->is_home) return load_home(lv);
    message_screen("Loading...", lv->title, NULL, "HOME quit");
    if (jf_list(lv->kind, lv->parent, lv->start, JF_PAGE_SIZE, lv->items, &n, &total) != 0) {
        notice(COLOR_WARN, jf_error());
        lv->count = 0;
        lv->total = 0;
        menu_set_count(&lv->m, 0);
        return -1;
    }
    lv->count  = n;
    lv->total  = total;
    lv->loaded = 1;
    menu_set_count(&lv->m, row_count(lv));
    return 0;
}

static void push_level(jf_list_kind kind, const char *parent, const char *title) {
    level *lv;

    if (g_depth + 1 >= MAX_DEPTH) {
        notice(COLOR_WARN, "Folders nest deeper than this list can follow.");
        return;
    }
    lv = &g_levels[++g_depth];
    memset(lv, 0, sizeof *lv);
    lv->kind = kind;
    fmt(lv->parent, sizeof lv->parent, "%s", parent ? parent : "");
    fmt(lv->title, sizeof lv->title, "%s", title);
    menu_init(&lv->m, 0, LIST_ROWS);
    load_level(lv);
}

static void turn_page(level *lv, int dir) {
    int to = lv->start + dir * JF_PAGE_SIZE;

    if (lv->is_home || to < 0 || to >= lv->total || to == lv->start) return;
    lv->start = to;
    menu_init(&lv->m, 0, LIST_ROWS);
    load_level(lv);
    /* Forward lands on the first entry of the new page, back on the last,
     * as if the list had simply scrolled on. */
    menu_select(&lv->m, dir > 0 ? has_prev(lv) : row_count(lv) - 1 - has_next(lv));
}

/* The right-hand column: how far through, watched, or a folder mark. */
static void item_status(const jf_item *it, char *out, size_t cap) {
    out[0] = 0;
    if (it->type[0] == '@' || it->is_folder) {
        fmt(out, cap, ">");
    } else if (it->position_ticks > 0 && it->runtime_ticks > 0) {
        char pos[16], dur[16];
        text_ticks(it->position_ticks, pos, sizeof pos);
        text_ticks(it->runtime_ticks, dur, sizeof dur);
        fmt(out, cap, "%s/%s", pos, dur);
    } else if (it->played) {
        fmt(out, cap, "watched");
    } else if (it->runtime_ticks > 0) {
        text_ticks(it->runtime_ticks, out, cap);
    }
}

static void draw_list(void) {
    level *lv = &g_levels[g_depth];
    char   where[200];
    int    r;

    render_text_begin();
    if (lv->is_home) {
        fmt(where, sizeof where, "Hexfin");
    } else if (lv->total > JF_PAGE_SIZE) {
        fmt(where, sizeof where, "%s  (%d-%d of %d)", lv->title, lv->start + 1, lv->start + lv->count, lv->total);
    } else {
        fmt(where, sizeof where, "%s", lv->title);
    }
    draw_title(where);

    if (row_count(lv) == 0) put(2, ROW_LIST, COLOR_DIM, COLOR_BG, lv->loaded ? "(nothing here)" : "(not loaded)");

    for (r = 0; r < LIST_ROWS && lv->m.top + r < row_count(lv); r++) {
        int      row = lv->m.top + r;
        int      sel = row == lv->m.sel;
        unsigned bg  = sel ? COLOR_SEL_BG : COLOR_BG;
        int      idx = row_item(lv, row);
        char     label[300], status[40];

        if (idx == -1) {
            put_text(0, ROW_LIST + r, COLS, COLOR_ACCENT, bg, "  < Previous page");
            continue;
        }
        if (idx == -2) {
            put_text(0, ROW_LIST + r, COLS, COLOR_ACCENT, bg, "  Next page >");
            continue;
        }
        jf_item_label(&lv->items[idx], lv->kind == JF_LIST_RESUME, label + 2, sizeof label - 2);
        label[0] = sel ? '>' : ' ';
        label[1] = ' ';
        item_status(&lv->items[idx], status, sizeof status);
        put_text(0, ROW_LIST + r, COLS - 16, COLOR_TEXT, bg, label);
        put_text(COLS - 16, ROW_LIST + r, 16, lv->items[idx].played ? COLOR_DIM : COLOR_DIM, bg, status);
    }

    if (g_notice[0]) put_text(0, ROW_NOTICE, COLS, g_notice_color, COLOR_BG, g_notice);
    put_text(0, ROW_HELP, COLS - 1, COLOR_DIM, COLOR_BG,
             g_depth == 0 ? "X open   /\\ reload   START settings   HOME quit"
                          : "X open/play   O back   L/R page   /\\ reload   START settings");
    render_text_end();
}

/* ---------------------------------------------------------------- playing */

static char           g_stream_url[HTTP_HOST_MAX + HTTP_PATH_MAX + 16];
static player_session g_session;

/* ------------------------------------------------------------ reporting
 *
 * Jellyfin keeps Continue Watching, the resume position and "watched" from
 * what clients report. A stream is reported as started when it opens, as
 * progressing every REPORT_EVERY_US and at every pause and resume, and as
 * stopped when it closes -- at the end, on a stop, and before each seek
 * reopens it. The progress reports come from a thread of their own, so an
 * HTTP round trip over the PSP's Wi-Fi never holds up a frame; they go on
 * their own connection (net/http.h). HOME cuts every connection at once,
 * so a HOME leaves the last progress report, at most REPORT_EVERY_US old,
 * as the resume position. */
#define REPORT_EVERY_US  (10ull * 1000000ull)
#define REPORT_PRIO      0x30 /* below everything that plays */
#define REPORT_STACK     0x4000
#define REPORT_EXIT_US   (15u * 1000000u) /* a report in flight is given its full HTTP timeouts */

static volatile int g_report_stop;
static SceUID       g_report_thread = -1;
static uint64_t     g_report_start; /* ticks into the item where the current stream begins */

static uint64_t live_position_ticks(void) {
    return g_report_start + (uint64_t)g_session.live_ms * 10000u;
}

static int report_thread(SceSize args, void *argp) {
    int                last_paused = 0;
    unsigned long long next        = platform_now_us() + REPORT_EVERY_US;

    (void)args;
    (void)argp;
    while (!g_report_stop && !platform_exit_requested()) {
        int                paused = g_session.live_paused;
        unsigned long long now    = platform_now_us();

        if (paused != last_paused) {
            jf_report(JF_REPORT_PROGRESS, live_position_ticks(), paused, paused ? "pause" : "unpause");
            last_paused = paused;
            next        = now + REPORT_EVERY_US;
        } else if (now >= next) {
            jf_report(JF_REPORT_PROGRESS, live_position_ticks(), paused, "timeupdate");
            next = now + REPORT_EVERY_US;
        }
        sceKernelDelayThread(250 * 1000);
    }
    return 0;
}

static void report_start(uint64_t start) {
    g_report_start      = start;
    g_session.live_ms   = 0;
    g_session.live_paused = 0;
    jf_report(JF_REPORT_START, start, 0, NULL);

    g_report_stop   = 0;
    g_report_thread = sceKernelCreateThread("hexfin_report", report_thread, REPORT_PRIO, REPORT_STACK, 0, NULL);
    if (g_report_thread >= 0 && sceKernelStartThread(g_report_thread, 0, NULL) < 0) {
        sceKernelDeleteThread(g_report_thread);
        g_report_thread = -1;
    }
    if (g_report_thread < 0) trace("app: no progress-report thread; only start and stop will be reported");
}

/* Stops the thread, then reports the stream stopped at `position`. */
static void report_stop(uint64_t position) {
    if (g_report_thread >= 0) {
        SceUInt timeout = REPORT_EXIT_US;
        g_report_stop   = 1;
        if (sceKernelWaitThreadEnd(g_report_thread, &timeout) < 0) sceKernelTerminateThread(g_report_thread);
        sceKernelDeleteThread(g_report_thread);
        g_report_thread = -1;
    }
    if (!platform_exit_requested()) jf_report(JF_REPORT_STOPPED, position, 0, NULL);
}

/* ---------------------------------------------------------------- resuming */

/* Where to start an item that was watched partway: its saved position, or
 * the beginning. Returns 0 with *start set, or -1 when the person backed out
 * (O) or pressed HOME. An item with no saved position starts at once. */
static int choose_start(const jf_item *it, const char *label, uint64_t *start) {
    menu m;
    char resume[48], at[16];
    int  redraw = 1;

    *start = 0;
    if (it->position_ticks == 0) return 0;

    text_ticks(it->position_ticks, at, sizeof at);
    fmt(resume, sizeof resume, "Resume from %s", at);
    menu_init(&m, 2, LIST_ROWS);
    input_pressed();

    while (!platform_exit_requested()) {
        unsigned b;

        if (redraw) {
            int r;
            render_text_begin();
            draw_title("Play");
            put_text(0, ROW_LIST, COLS, COLOR_ACCENT, COLOR_BG, label);
            for (r = 0; r < 2; r++) {
                char row[80];
                fmt(row, sizeof row, "%c %s", r == m.sel ? '>' : ' ', r == 0 ? resume : "Start from the beginning");
                put_text(0, ROW_LIST + 2 + r * 2, COLS, COLOR_TEXT, r == m.sel ? COLOR_SEL_BG : COLOR_BG, row);
            }
            put_text(0, ROW_HELP, COLS - 1, COLOR_DIM, COLOR_BG, "X play   O back   HOME quit");
            render_text_end();
            redraw = 0;
        }
        b = input_pressed();
        if (b & (PSP_CTRL_UP | PSP_CTRL_DOWN)) {
            menu_move(&m, (b & PSP_CTRL_UP) ? -1 : 1);
            redraw = 1;
        }
        if (b & PSP_CTRL_CIRCLE) return -1;
        if (b & PSP_CTRL_CROSS) {
            *start = m.sel == 0 ? it->position_ticks : 0;
            return 0;
        }
        sceKernelDelayThread(16 * 1000);
    }
    return -1;
}

/* ---------------------------------------------------------------- playing */

/* Plays `it` -- from its saved position when the person chooses to resume
 * -- and again from wherever L or R moves it to: each seek is a new stream
 * from the server, opened at the new position, through a fresh player_run.
 * Returns 1 when anything was played, so the list can be refreshed. */
static int play_item(const jf_item *it) {
    char           label[300], msg[300];
    player_options opt;
    player_outcome out;
    uint64_t       start = 0, end_at;
    int            first = 1;

    jf_item_label(it, 1, label, sizeof label);
    if (choose_start(it, label, &start) != 0) return 0;
    trace("app: play %s from %u s", it->id, (unsigned)(start / 10000000u));

    for (;;) {
        if (first) {
            message_screen("Starting...", label, "Asking the server for a PSP-sized stream.", "HOME quit");
            first = 0;
        } else {
            /* A seek keeps the picture: the last frame stays on screen with
             * one line over it, the way the volume is shown, until the
             * stream from the new position brings the next one. */
            char at[16], dur[16], line[96];
            text_ticks(start, at, sizeof at);
            if (it->runtime_ticks) {
                text_ticks(it->runtime_ticks, dur, sizeof dur);
                fmt(line, sizeof line, "Going to %s / %s ...", at, dur);
            } else {
                fmt(line, sizeof line, "Going to %s ...", at);
            }
            render_osd(line);
            render_invalidate(); /* the first new picture clears what the line covered */
            trace("app: seek to %u s", (unsigned)(start / 10000000u));
        }

        if (jf_open_item(it->id, start, g_stream_url, sizeof g_stream_url) != 0) {
            notice(COLOR_WARN, jf_error());
            return 1;
        }
        if (source_http.open(g_stream_url) != SOURCE_OK) {
            fmt(msg, sizeof msg, "The stream would not start: %s", source_http.error());
            notice(COLOR_WARN, msg);
            source_http.close();
            jf_stop_item();
            return 1;
        }

        memset(&opt, 0, sizeof opt);
        opt.interactive    = 1;
        opt.show_clock     = jf_config_get()->show_clock;
        opt.show_battery   = jf_config_get()->show_battery;
        opt.title          = label;
        opt.duration_ticks = it->runtime_ticks;
        opt.start_ticks    = start;

        report_start(start);
        out = player_run(&g_session, &source_http, &opt);
        source_http.close();

        /* Where it got to. At the natural end, the item's full length, so
         * the server counts it as watched rather than a few seconds short. */
        end_at = start + g_session.position_us * 10u;
        if (out == PLAYER_ENDED && it->runtime_ticks > end_at) end_at = it->runtime_ticks;
        report_stop(end_at);
        if (out == PLAYER_HOME) return 1;
        jf_stop_item();

        if (out != PLAYER_SEEK) break;
        start = g_session.seek_to_ticks;
    }

    switch (out) {
        case PLAYER_ENDED: fmt(msg, sizeof msg, "Finished: %s", label); notice(COLOR_TEXT, msg); break;
        case PLAYER_STOPPED: {
            char at[16];
            text_ticks(end_at, at, sizeof at);
            fmt(msg, sizeof msg, "Stopped at %s: %s", at, label);
            notice(COLOR_TEXT, msg);
            break;
        }
        default:
            fmt(msg, sizeof msg, "Playback failed: %s",
                g_session.parse_fatal ? g_session.parse_fatal : "the stream could not be read");
            notice(COLOR_WARN, msg);
            break;
    }
    if (g_session.video_open_failed && out != PLAYER_FAILED) {
        fmt(msg, sizeof msg, "The picture could not be decoded: %s", video_decoder_error());
        notice(COLOR_WARN, msg);
    }
    return 1;
}

/* ------------------------------------------------------------------ about */

#define APP_AUTHOR     "hexplus"
#define APP_AUTHOR_URL "https://github.com/hexplus/"

static void about_screen(void) {
    char     line[160];
    int      row = ROW_LIST;
    unsigned b;

    render_text_begin();
    draw_title("About");
    fmt(line, sizeof line, "Hexfin  version %s", g_env->version);
    put_text(0, row++, COLS, COLOR_ACCENT, COLOR_BG, line);
    put_text(0, row++, COLS, COLOR_DIM, COLOR_BG, "A Jellyfin client for the Sony PSP, played on its Media Engine.");
    row++;
    fmt(line, sizeof line, "Author:   %s", APP_AUTHOR);
    put_text(0, row++, COLS, COLOR_TEXT, COLOR_BG, line);
    fmt(line, sizeof line, "GitHub:   %s", APP_AUTHOR_URL);
    put_text(0, row++, COLS, COLOR_TEXT, COLOR_BG, line);
    row++;
    fmt(line, sizeof line, "Server:   %s", jf_server(g_env->default_server));
    put_text(0, row++, COLS, COLOR_DIM, COLOR_BG, line);
    put_text(0, ROW_HELP, COLS - 1, COLOR_DIM, COLOR_BG, "O back   HOME quit");
    render_text_end();

    input_pressed();
    while (!platform_exit_requested()) {
        b = input_pressed();
        if (b & (PSP_CTRL_CIRCLE | PSP_CTRL_CROSS)) return;
        sceKernelDelayThread(16 * 1000);
    }
}

/* --------------------------------------------------------------- settings */

typedef enum { SETTINGS_BACK, SETTINGS_SIGNED_OUT } settings_result;

static settings_result settings_screen(void) {
    menu m;
    char lines[6][160];
    int  redraw = 1;

    menu_init(&m, 6, LIST_ROWS);
    for (;;) {
        unsigned b;
        int      r;

        if (platform_exit_requested()) return SETTINGS_BACK;
        if (redraw) {
            jf_config *cfg = jf_config_get();

            fmt(lines[0], sizeof lines[0], "Clock:     %s", cfg->show_clock ? "Show" : "Hide");
            fmt(lines[1], sizeof lines[1], "Battery:   %s", cfg->show_battery ? "Show" : "Hide");
            fmt(lines[2], sizeof lines[2], "Server:    %s", jf_server(g_env->default_server));
            fmt(lines[3], sizeof lines[3], "User:      %s", jf_user_name()[0] ? jf_user_name() : "(signed in)");
            fmt(lines[4], sizeof lines[4], "Sign out");
            fmt(lines[5], sizeof lines[5], "About Hexfin %s", g_env->version);

            render_text_begin();
            draw_title("Settings");
            for (r = 0; r < 6; r++) {
                char row[170];
                fmt(row, sizeof row, "%c %s", r == m.sel ? '>' : ' ', lines[r]);
                put_text(0, ROW_LIST + r * 2, COLS, COLOR_TEXT, r == m.sel ? COLOR_SEL_BG : COLOR_BG, row);
            }
            {
                const char *hint = "";
                switch (m.sel) {
                    case 0: hint = "X shows or hides the time of day in the top corner and on the pause screen."; break;
                    case 1: hint = "X shows or hides the battery level in the top corner and on the pause screen."; break;
                    case 2: hint = "To change it, edit server= in jellyfin.cfg next to the EBOOT."; break;
                    case 4: hint = "X forgets this PSP's sign-in. Quick Connect is needed again."; break;
                    case 5: hint = "X shows the version and the author."; break;
                    default: break;
                }
                put_wrapped(ROW_LIST + 12, 4, COLOR_DIM, hint);
            }
            put_text(0, ROW_HELP, COLS - 1, COLOR_DIM, COLOR_BG, "X select   O back   HOME quit");
            render_text_end();
            redraw = 0;
        }

        b = input_pressed();
        if (b & PSP_CTRL_UP) {
            menu_move(&m, -1);
            redraw = 1;
        }
        if (b & PSP_CTRL_DOWN) {
            menu_move(&m, 1);
            redraw = 1;
        }
        if (b & PSP_CTRL_CIRCLE) return SETTINGS_BACK;
        if ((b & PSP_CTRL_CROSS) && (m.sel == 0 || m.sel == 1)) {
            jf_config *cfg  = jf_config_get();
            int       *flag = m.sel == 0 ? &cfg->show_clock : &cfg->show_battery;
            *flag           = !*flag;
            jf_config_save();
            redraw = 1;
        }
        if ((b & PSP_CTRL_CROSS) && m.sel == 5) {
            about_screen();
            redraw = 1;
        }
        if (status_changed()) redraw = 1;
        if ((b & PSP_CTRL_CROSS) && m.sel == 4) {
            message_screen("Signing out...", NULL, NULL, NULL);
            jf_sign_out();
            trace("app: signed out");
            return SETTINGS_SIGNED_OUT;
        }
        sceKernelDelayThread(16 * 1000);
    }
}

/* ------------------------------------------------------------------- run */

/* One signed-in session: the lists until HOME (returns 0) or a sign-out
 * (returns 1). */
static int browse(void) {
    int redraw = 1;

    g_depth = 0;
    memset(&g_levels[0], 0, sizeof g_levels[0]);
    g_levels[0].is_home = 1;
    fmt(g_levels[0].title, sizeof g_levels[0].title, "Home");
    menu_init(&g_levels[0].m, 0, LIST_ROWS);
    load_home(&g_levels[0]);

    while (!platform_exit_requested()) {
        level   *lv = &g_levels[g_depth];
        unsigned b;

        if (redraw) {
            draw_list();
            redraw = 0;
        }
        b = input_pressed();
        if (!b) {
            int level, muted;
            if (status_changed()) redraw = 1;
            /* The volume buttons: the level on the notice line. */
            if (platform_volume_changed(&level, &muted)) {
                char line[64];
                text_volume(line, sizeof line, level, PLATFORM_VOLUME_MAX, muted);
                notice(COLOR_TEXT, line);
                redraw = 1;
            }
            sceKernelDelayThread(16 * 1000);
            continue;
        }
        redraw = 1;

        if (b & PSP_CTRL_UP) menu_move(&lv->m, -1);
        if (b & PSP_CTRL_DOWN) menu_move(&lv->m, 1);
        if (b & PSP_CTRL_LEFT) menu_move(&lv->m, -LIST_ROWS);
        if (b & PSP_CTRL_RIGHT) menu_move(&lv->m, LIST_ROWS);
        if (b & PSP_CTRL_LTRIGGER) turn_page(lv, -1);
        if (b & PSP_CTRL_RTRIGGER) turn_page(lv, 1);
        if (b & PSP_CTRL_TRIANGLE) {
            notice(COLOR_TEXT, "");
            load_level(lv);
        }
        if ((b & PSP_CTRL_CIRCLE) && g_depth > 0) {
            g_depth--;
            notice(COLOR_TEXT, "");
        }
        if (b & PSP_CTRL_START) {
            if (settings_screen() == SETTINGS_SIGNED_OUT) return 1;
        }
        if (b & PSP_CTRL_CROSS) {
            int idx = row_item(lv, lv->m.sel);

            notice(COLOR_TEXT, "");
            if (idx == -1) turn_page(lv, -1);
            else if (idx == -2) turn_page(lv, 1);
            else if (idx >= 0 && idx < lv->count) {
                jf_item *it = &lv->items[idx];

                if (strcmp(it->type, TYPE_SETTINGS) == 0) {
                    if (settings_screen() == SETTINGS_SIGNED_OUT) return 1;
                } else if (strcmp(it->type, TYPE_RESUME) == 0) {
                    push_level(JF_LIST_RESUME, NULL, "Continue Watching");
                } else if (jf_item_playable(it)) {
                    /* Afterwards the list is read again: positions, "watched",
                     * and Continue Watching itself have changed. The notice
                     * the playback left is kept across the reload. */
                    if (play_item(it) && !platform_exit_requested()) {
                        char kept[sizeof g_notice];
                        unsigned kept_color = g_notice_color;
                        fmt(kept, sizeof kept, "%s", g_notice);
                        load_level(lv);
                        notice(kept_color, kept);
                    }
                } else if (jf_item_browsable(it)) {
                    char title[JF_ITEM_NAME_MAX + 8];
                    jf_item_label(it, 0, title, sizeof title);
                    push_level(JF_LIST_CHILDREN, it->id, title);
                } else {
                    notice(COLOR_WARN, "This PSP client only plays video.");
                }
            }
        }
    }
    return 0;
}

void app_run(const app_env *env) {
    g_env = env;
    trace_screen(0);
    pspDebugScreenEnableBackColor(1);
    platform_set_stop_hook(http_cancel);

    jf_config_load(env->cfg_path);
    if (!connect_wifi()) return;

    for (;;) {
        if (!sign_in()) return;
        notice(COLOR_TEXT, "");
        if (!browse()) return;
    }
}

#endif /* __PSP__ */
