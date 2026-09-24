/* Hexfin: the program's entry point.
 *
 * Two programs share it, chosen at compile time by PROBE_MODE:
 *
 *   - THE APP (PROBE_MODE 0, the default): Wi-Fi, sign-in, the library as a
 *     list, and playback -- app/app.c.
 *   - THE PROBE (PROBE_MODE 1): plays one compile-time source -- a file on
 *     the memory stick, a URL, or one Jellyfin item -- and reports in plain
 *     words how far it got. It is kept, permanently, because it is how a
 *     decode regression is told apart from a network one: the same fixture
 *     through the same pipeline, with the radio out of the picture.
 *
 * Both play through player/player.c, the pipeline the probe proved on
 * hardware (docs/RESEARCH.md section 10). What stays here is what happens
 * once per run: bringing the console up, the Media Engine runtime, the
 * probe's report, and the teardown.
 *
 * TEARDOWN ORDER matters: the decoders first (player_run already closes them
 * whichever way playback ended; closing again is harmless), then the display
 * (render_shutdown), then the byte source, then the radio, then the console
 * (platform_exit). Every one of those close calls is safe when its matching
 * open never happened or failed partway through, and this file relies on
 * that: the SAME teardown() runs however far the run got. Skipping it, or
 * running it out of order, is design section 3.4's classic cause of a black
 * screen on HOME -- exiting underneath a reserved audio channel or a live
 * sceMpeg instance. */

#include "media/audio_decoder.h"
#include "media/av_sync.h"
#include "media/fmp4.h"
#include "media/source.h"
#include "media/video_decoder.h"
#include "app/app.h"
#include "jellyfin/jellyfin.h"
#include "net/http.h"
#include "net/http_parse.h"
#include "net/wifi.h"
#include "platform/me_runtime.h"
#include "platform/psp_platform.h"
#include "platform/trace.h"
#include "player/player.h"
#include "ui/render.h"
#include "ui/stats.h"
#include "utils/memwatch.h"

#include <pspdebug.h>
#include <pspkernel.h>
#include <psppower.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

PSP_MODULE_INFO("Hexfin", 0, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

/* Explicit, because the default is "take the largest free block on the first
 * malloc" -- which would hand the newlib heap the very block the decoder
 * needs and make platform_free_kb() report a partition that no longer
 * exists. This program still avoids the heap by design: every buffer below,
 * including the read ring, is a fixed-size static rather than a malloc, so
 * 1 MB remains generous. */
PSP_HEAP_SIZE_KB(1024);

#define HEXFIN_VERSION "0.2.0"

/* 0 builds the app, 1 the probe; see the top of this file. */
#ifndef PROBE_MODE
#define PROBE_MODE 0
#endif

/* The settings file, next to the EBOOT like the Media Engine bridge. Holds a
 * device id, the sign-in and the settings: delete it to sign in again. */
#define HEXFIN_CFG_NAME "jellyfin.cfg"

/* Which hardware experiment this build is. Shown on screen, and it names the
 * log file (ms0:/probe-<name>.log), because several builds that all say
 * "Hexfin" are indistinguishable once they are on the console. */
#ifndef PROBE_BUILD_NAME
#define PROBE_BUILD_NAME "app"
#endif

/* ------------------------------------------------------- where bytes come from
 *
 * One switch, set at compile time, and printed on the screen so a run can
 * never be confused with the other kind of run. It is compile-time and not a
 * menu because this is a probe: a menu is a thing to get wrong at three in
 * the morning, and the two builds are meant to be compared against each
 * other, not switched between.
 *
 * The file path is not a leftover. It is the control: the same fixture,
 * parser, Media Engine and screen, with the radio out of the picture
 * entirely. When the HTTP build stops decoding, the file build is what says
 * whether the decoder broke or the network did -- and PROMPT.md Rule 4 means
 * that question will be asked often and mostly on hardware.
 *
 * PROBE_HTTP_URL is unauthenticated on purpose: it proves the radio comes up,
 * a socket opens, a response is parsed within its bounds, and the fMP4 reader
 * is as happy with bytes off a socket as with bytes off a memory stick, with
 * no server logic in the way. PROBE_JELLYFIN, below, is the signed-in path to
 * a real library item.
 *
 * The default is the file, because it is the build that needs nothing but a
 * memory stick to be meaningful. Set PROBE_SOURCE_HTTP to 1 and this URL
 * answers 404 today -- which is itself worth seeing, because a legible "the
 * server answered 404, not 200" proves the radio, the socket, the request and
 * the status line all the way to the server's own reply. It was exercised
 * against a chunked fragmented-MP4 response as well; docs/RESEARCH.md section
 * 9 records what that run showed and what it did not. */
#ifndef PROBE_SOURCE_HTTP
#define PROBE_SOURCE_HTTP 0
#endif
#ifndef PROBE_HTTP_URL
#define PROBE_HTTP_URL "http://192.168.100.78:8096/probe360.mp4"
#endif

/* Play an item from the Jellyfin library instead of PROBE_HTTP_URL: sign in
 * (Quick Connect the first time, the kept token after that -- see
 * jellyfin/jellyfin.h), ask PlaybackInfo for the PSP's shape, and stream the
 * TranscodingUrl it answers with through the same pipeline. Implies the
 * HTTP source. The server is addressed on the LAN by plain HTTP: the PSP
 * has no TLS client. */
#ifndef PROBE_JELLYFIN
#define PROBE_JELLYFIN 0
#endif
#ifndef PROBE_JF_SERVER
#define PROBE_JF_SERVER "http://192.168.100.78:8096"
#endif
#ifndef PROBE_JF_ITEM
#define PROBE_JF_ITEM "50f9bb3d8ba6b9eda4e27b3b141ba5f0"
#endif

#if PROBE_JELLYFIN && !PROBE_SOURCE_HTTP
#undef PROBE_SOURCE_HTTP
#define PROBE_SOURCE_HTTP 1
#endif

/* The connection profile to use: 0 is the first one stored in the PSP's own
 * network settings, and a number picks that slot. This client
 * does not scan and does not ask for a key: the console has a better screen
 * for that than this probe ever will, and PROMPT.md's MVP is a client for a
 * network the operator already joined. */
#define PROBE_AP_CONFIG       0

/* Bisection switches for the hardware freeze: each one takes a single piece
 * of the decode path out of the run, so "which piece switches the console
 * off" can be answered by which build survives, even when no log does.
 * All 0 is the real probe. */
#ifndef PROBE_SKIP_VIDEO
#define PROBE_SKIP_VIDEO 0 /* never open the video decoder */
#endif
#ifndef PROBE_SKIP_AUDIO
#define PROBE_SKIP_AUDIO 0 /* never open the audio decoder */
#endif
#ifndef PROBE_SKIP_RENDER
#define PROBE_SKIP_RENDER 0 /* no render_init: video decodes into RAM, not VRAM */
#endif
/* platform/me_runtime.c: restart the Media Engine from a kernel module and
 * load the console's own mpeg_vsh before any decoder opens. 0 skips it --
 * the game-side sceMpeg path, which refused every frame on hardware. The
 * mode is hexfin_me.prx's boot mode: 4 is the one that decoded, on a PSP-1000
 * with 6.61 ARK-5 on 2026-09-24; mode 1 refused every frame exactly as the
 * game-side path did (docs/RESEARCH.md section 10). */
#ifndef PROBE_ME_RUNTIME
#define PROBE_ME_RUNTIME 1
#endif
#ifndef PROBE_ME_BOOT_MODE
#define PROBE_ME_BOOT_MODE 4
#endif

/* Show every decoded picture, whatever the A/V clock says. A probe-only
 * switch for looking at the picture: this probe decodes each fragment's video
 * in one burst ahead of its audio, so its clock calls almost every frame late
 * and drops it, which says nothing about the decoder. */
#ifndef PROBE_SHOW_ALL
#define PROBE_SHOW_ALL 0
#endif

#ifndef PROBE_CPU_MHZ
#define PROBE_CPU_MHZ 0 /* 0 leaves the clock alone; 333 runs CPU and bus flat out */
#endif
#define PROBE_WIFI_TIMEOUT_MS 20000u


#if !PROBE_SOURCE_HTTP
/* Several candidates, tried in order, rather than one hard-coded path.
 *
 * ms0: is where the file belongs on a real PSP and is tried first. The rest
 * are there because the fixture cannot always be put next to the EBOOT: on
 * this development machine Windows Defender's Controlled Folder Access owns
 * the emulator's memory-stick directory and silently refuses every write to
 * it, so a probe that knew only ms0: could not be fed at all without the
 * operator changing a Defender setting. host0: is the directory the loaded
 * executable came from, which the emulator maps and a real PSP does not --
 * that costs one failed sceIoOpen on hardware and nothing else.
 *
 * The path that actually opened is reported on screen, so a run can never
 * leave you guessing which file was read. */
static const char *const g_media_candidates[] = {
    "ms0:/PSP/GAME/Hexfin/probe360.mp4",
    "host0:/probe360.mp4",
    "probe360.mp4",
};
#endif

/* The byte source of a probe run, bound at compile time along with the
 * switch it follows: the screen is drawn once BEFORE the source is opened --
 * opening it is the part that can sit for twenty seconds waiting on an
 * access point -- and a report that dereferenced this before then would turn
 * a slow connect into a crash. */
#if PROBE_SOURCE_HTTP
static const media_source *g_source = &source_http;
#else
static const media_source *g_source = &source_file;
#endif

/* The folder the EBOOT was started from, as me_runtime.c finds it, and the
 * settings file in it. */
static void cfg_path(char *out, size_t cap) {
    if (getcwd(out, cap - sizeof HEXFIN_CFG_NAME - 1)) {
        size_t n = strlen(out);
        if (n && out[n - 1] != '/') out[n++] = '/';
        strcpy(out + n, HEXFIN_CFG_NAME);
    } else {
        snprintf(out, cap, "ms0:/PSP/GAME/Hexfin/%s", HEXFIN_CFG_NAME);
    }
}

/* ------------------------------------------------------------- probe state
 *
 * The run itself is player/player.h's player_session; this adds what only a
 * probe reports -- how far opening the source got. */
typedef struct {
    player_session s;
    int            played;             /* player_run was reached */
    int            source_open_failed;
    int            wifi_failed;        /* only meaningful in an HTTP build */
    int            jf_failed;          /* only meaningful in a Jellyfin build: signing in or PlaybackInfo failed */
    const char    *media_path;         /* the path or URL this run used; NULL when none was reached */
    const char    *open_failure;       /* why opening failed, kept */
} probe_state;

static char g_open_failure[192];

static const char *keep_open_failure(const char *msg) {
    snprintf(g_open_failure, sizeof g_open_failure, "%s", msg ? msg : "");
    return g_open_failure;
}

/* ----------------------------------------------------------- opening a source
 *
 * Both halves end the same way -- ps->media_path set to whatever the screen
 * should name, and a yes or a no -- so that everything downstream of here,
 * the report included, has one shape rather than two. */
#if PROBE_JELLYFIN
/* The stream URL carries the access token, so it is kept for the request and
 * never put on the screen or in the log: what they name is the item. */
static char g_jf_url[HTTP_HOST_MAX + HTTP_PATH_MAX + 16];
static char g_jf_label[160];

/* Well below the report, which is a handful of rows while opening, and above
 * the bottom row trace() writes to. The code is the one thing on this
 * screen a person has to read, so it gets rows of its own. */
#define JF_SHOW_ROW 14

static void jf_show(const char *line1, const char *line2) {
    pspDebugScreenSetXY(0, JF_SHOW_ROW);
    pspDebugScreenPrintf("%-67.67s\n", line1 ? line1 : "");
    pspDebugScreenSetXY(0, JF_SHOW_ROW + 2);
    pspDebugScreenPrintf("%-67.67s\n", line2 ? line2 : "");
}

static void jf_clear_show(void) { jf_show("", ""); }
#endif

static int open_source(probe_state *ps) {
#if PROBE_SOURCE_HTTP
    /* Named before the connection is even attempted, because a failure has
     * to be able to say what it was trying to reach. */
#if PROBE_JELLYFIN
    snprintf(g_jf_label, sizeof g_jf_label, "jellyfin item %s", PROBE_JF_ITEM);
    ps->media_path = g_jf_label;
#else
    ps->media_path = PROBE_HTTP_URL;
#endif

    /* The stop hook goes in BEFORE the first blocking call, not after the
     * connection is up: wifi_connect's own wait is already long enough that
     * HOME during it has to do something, and design section 3.4 wants the
     * wakeup channel to exist before the thing that blocks does. */
    platform_set_stop_hook(http_cancel);

    if (wifi_connect(PROBE_AP_CONFIG, PROBE_WIFI_TIMEOUT_MS) != 0) {
        ps->wifi_failed  = 1;
        ps->open_failure = keep_open_failure(wifi_error());
        return 0;
    }
#if PROBE_JELLYFIN
    {
        char cfg[256];

        cfg_path(cfg, sizeof cfg);
        jf_config_load(cfg);
        jf_show("Signing in to Jellyfin...", jf_server(PROBE_JF_SERVER));
        if (jf_sign_in(jf_server(PROBE_JF_SERVER), HEXFIN_VERSION, jf_show) != 0 ||
            jf_open_item(PROBE_JF_ITEM, 0, g_jf_url, sizeof g_jf_url) != 0) {
            ps->jf_failed    = 1;
            ps->open_failure = keep_open_failure(jf_error());
            jf_clear_show();
            return 0;
        }
        jf_clear_show();
        if (jf_item_name()[0]) snprintf(g_jf_label, sizeof g_jf_label, "jellyfin: %s", jf_item_name());
        if (g_source->open(g_jf_url) == SOURCE_OK) return 1;
    }
#else
    if (g_source->open(PROBE_HTTP_URL) == SOURCE_OK) return 1;
#endif
    ps->open_failure = keep_open_failure(g_source->error());
    return 0;
#else
    unsigned i;

    for (i = 0; i < sizeof(g_media_candidates) / sizeof(g_media_candidates[0]); i++) {
        if (g_source->open(g_media_candidates[i]) == SOURCE_OK) {
            ps->media_path = g_media_candidates[i];
            return 1;
        }
    }
    ps->open_failure = keep_open_failure(g_source->error());
    return 0;
#endif
}

/* ------------------------------------------------------------------ report */

/* The debug screen is 68 characters wide (480 px / 7 px per glyph) and does
 * not wrap: anything past column 68 is simply lost. The diagnostics this probe
 * exists to produce are whole sentences, and a sentence cut off at "the audio
 * codec module would not load (fir" tells you almost nothing -- the error code
 * that follows is the part worth reading. So they are wrapped here, on word
 * boundaries, with continuation lines indented under the prefix. */
#define SCREEN_COLS 68

static void print_wrapped(int *row, const char *prefix, const char *msg) {
    unsigned indent = 0;
    unsigned col;
    const char *p = msg;

    while (prefix[indent]) indent++;
    if (indent > SCREEN_COLS / 2) indent = SCREEN_COLS / 2; /* never leave no room for text */

    col = indent;
    pspDebugScreenSetXY(0, (*row)++);
    pspDebugScreenPrintf("%s", prefix);

    while (*p) {
        unsigned word = 0;
        while (p[word] && p[word] != ' ') word++;

        /* Break before the word, not inside it -- unless the word alone is
         * wider than the screen, in which case it has to be split or it would
         * loop here forever. */
        if (col + word > SCREEN_COLS && col > indent) {
            unsigned i;
            pspDebugScreenSetXY(0, (*row)++);
            for (i = 0; i < indent; i++) pspDebugScreenPrintf(" ");
            col = indent;
        }

        while (word > 0) {
            unsigned take = (col + word > SCREEN_COLS) ? (SCREEN_COLS - col) : word;
            unsigned i;
            if (take == 0) { /* the word does not fit on a fresh line either */
                pspDebugScreenSetXY(0, (*row)++);
                for (i = 0; i < indent; i++) pspDebugScreenPrintf(" ");
                col  = indent;
                take = SCREEN_COLS - col;
            }
            for (i = 0; i < take; i++) pspDebugScreenPrintf("%c", p[i]);
            p += take;
            col += take;
            word -= take;
        }

        while (*p == ' ') {
            if (col < SCREEN_COLS) {
                pspDebugScreenPrintf(" ");
                col++;
            }
            p++;
        }
    }
    pspDebugScreenPrintf("\n");
}

/* The tallest report drawn so far.
 *
 * pspDebugScreen never clears a row nobody writes to, and this report is no
 * longer a fixed height: the screen is drawn once before the source is opened
 * -- which is short -- and the run's real report may be shorter still if
 * opening failed. Without this, a failed connect leaves the tail of an
 * earlier, taller screen sitting underneath it, reading as part of the same
 * report. Blanking the difference rather than clearing the whole screen every
 * time is what keeps a report redrawn from flickering. */
static int g_rows_drawn;

static void draw_screen(const probe_state *ps) {
    const player_session *s   = &ps->s;
    int                   row = 0;

    pspDebugScreenSetXY(0, row++);
    pspDebugScreenPrintf("Hexfin probe %s [%s]\n", HEXFIN_VERSION, PROBE_BUILD_NAME);
    row++;

    /* Which source, always, and before anything it produced: every number
     * below means something different depending on this one word. */
    pspDebugScreenSetXY(0, row++);
    pspDebugScreenPrintf("source %s\n", g_source->name);

#if PROBE_SOURCE_HTTP
    /* Which profile, before whether it worked: a failure against the wrong
     * access point and one against the right one are fixed differently. */
    if (wifi_profile()[0]) print_wrapped(&row, "ap    ", wifi_profile());
    if (ps->wifi_failed) {
        print_wrapped(&row, "wifi: ", wifi_error());
    } else {
        pspDebugScreenSetXY(0, row++);
        pspDebugScreenPrintf("wifi  %s\n", wifi_ip()[0] ? wifi_ip() : "(connecting)");
    }
#endif

    if (ps->source_open_failed) {
#if PROBE_SOURCE_HTTP
        print_wrapped(&row, "url   ", ps->media_path ? ps->media_path : PROBE_HTTP_URL);
        /* Not printed when the radio never came up: the reason is already
         * on the wifi line, and printed twice it reads as a second failure. */
        if (!ps->wifi_failed) print_wrapped(&row, "why:  ", ps->open_failure ? ps->open_failure : "");
#else
        /* Every candidate, not just the first: "not found" is only useful
         * when it says where it looked. */
        unsigned i;
        pspDebugScreenSetXY(0, row++);
        pspDebugScreenPrintf("none of these could be opened:\n");
        for (i = 0; i < sizeof(g_media_candidates) / sizeof(g_media_candidates[0]); i++) {
            pspDebugScreenSetXY(0, row++);
            pspDebugScreenPrintf("      %s\n", g_media_candidates[i]);
        }
        print_wrapped(&row, "why:  ", ps->open_failure ? ps->open_failure : "");
#endif
    } else {
        print_wrapped(&row, "from  ", ps->media_path ? ps->media_path : "(opening)");

        pspDebugScreenSetXY(0, row++);
        pspDebugScreenPrintf("fragments parsed  %u\n", (unsigned)s->fragments_parsed);
        pspDebugScreenSetXY(0, row++);
        pspDebugScreenPrintf("video samples     %u\n", (unsigned)s->video_samples);
        pspDebugScreenSetXY(0, row++);
        pspDebugScreenPrintf("audio samples     %u\n", (unsigned)s->audio_samples);

        if (!ps->played) {
            pspDebugScreenSetXY(0, row++);
            pspDebugScreenPrintf("opening...\n");
        } else {
            if (!s->m.have_video) {
                pspDebugScreenSetXY(0, row++);
                pspDebugScreenPrintf("video: no video track in this file\n");
            } else if (s->video_open_failed) {
                print_wrapped(&row, "video: decoder refused -- ", video_decoder_error());
            } else {
                pspDebugScreenSetXY(0, row++);
                pspDebugScreenPrintf("video: decoded (%ux%u)\n", (unsigned)s->m.video.width, (unsigned)s->m.video.height);
            }

            if (!s->m.have_audio) {
                pspDebugScreenSetXY(0, row++);
                pspDebugScreenPrintf("audio: no audio track in this file\n");
            } else if (s->audio_open_failed) {
                print_wrapped(&row, "audio: decoder refused -- ", audio_decoder_error());
            } else {
                pspDebugScreenSetXY(0, row++);
                pspDebugScreenPrintf("audio: decoded (%u Hz)\n", (unsigned)s->m.audio.sample_rate);
            }

            if (s->parse_fatal) {
                print_wrapped(&row, "parse stopped: ", s->parse_fatal);
            } else if (s->done_parsing) {
                pspDebugScreenSetXY(0, row++);
                pspDebugScreenPrintf("parse complete (end of file)\n");
            } else {
                pspDebugScreenSetXY(0, row++);
                pspDebugScreenPrintf("stopped by HOME\n");
            }
        }
    }
    row++;

    {
        stats_snapshot snap;

        memset(&snap, 0, sizeof snap);
        snap.frames_shown       = s->sync.frames_shown;
        snap.frames_dropped     = s->sync.frames_dropped;
        snap.decode_us_avg      = s->video_decode_count ? (uint32_t)(s->video_decode_us_total / s->video_decode_count) : 0;
        snap.decode_us_worst    = s->video_decode_us_worst;
        snap.audio_underruns    = s->audio_underruns;
        snap.queue_depth        = s->sync.queue_depth;
        snap.elapsed_us         = (uint32_t)(s->last_shown_us - s->first_shown_us);
        snap.mem_min_free_kb    = memwatch_min_free_kb();
        snap.mem_min_largest_kb = memwatch_min_largest_kb();
        stats_draw(&snap, row);
        row += STATS_LINE_COUNT;
    }

    pspDebugScreenSetXY(0, row++);
    pspDebugScreenPrintf("HOME to exit\n");

    while (row < g_rows_drawn) {
        int c;
        pspDebugScreenSetXY(0, row++);
        for (c = 0; c < SCREEN_COLS; c++) pspDebugScreenPrintf(" ");
    }
    g_rows_drawn = row;
}

/* One probe run: open the compile-time source, play it, report, wait for
 * HOME. */
static void probe_main(void) {
    static probe_state ps;
    player_options     opt;

    memset(&ps, 0, sizeof ps);
    memset(&opt, 0, sizeof opt);
    opt.show_all   = PROBE_SHOW_ALL;
    opt.skip_audio = PROBE_SKIP_AUDIO;
    opt.skip_video = PROBE_SKIP_VIDEO;

    /* Drawn once before the source is opened, because opening it is the part
     * that can sit there for twenty seconds waiting on an access point, and a
     * blank screen for twenty seconds is indistinguishable from a hang. */
    draw_screen(&ps);

    trace("opening the source");
    if (!open_source(&ps)) {
        trace("source did not open: %s", ps.open_failure ? ps.open_failure : "");
        ps.source_open_failed = 1;
    } else {
        player_outcome out;
        ps.played = 1;
        out       = player_run(&ps.s, g_source, &opt);
        (void)out;
    }

    render_show_text();
    draw_screen(&ps); /* the final report, even if nothing played */

    /* --------------------------------------------------------- idle for HOME
     *
     * The loop samples as well as draws: a partition read only once cannot
     * show a drift, and a drift with nothing decoding any more is still
     * worth being able to see. */
    while (!platform_exit_requested()) {
        memwatch_sample(platform_free_kb(), platform_largest_free_kb());
        draw_screen(&ps);
        sceKernelDelayThread(250 * 1000);
    }
}

/* ---------------------------------------------------------------- teardown
 *
 * Safe to call from any point this program can reach, including before
 * anything after platform_init() has succeeded -- every close() below
 * already tolerates its matching open never having run (see each module's
 * own header). The decoders are already closed: player_run closes them
 * before it returns, whichever way playback ended. */
static void teardown(void) {
    audio_decoder_close();
    video_decoder_close();
    render_shutdown();
    /* The hook goes before the source does, so the callback can never reach
     * into a source that is halfway through going away. */
    platform_set_stop_hook(0);
    if (g_source) g_source->close();
    source_http.close();
    /* Unconditional, and safe when the radio was never brought up: in a file
     * build there is nothing to take down, and in an HTTP build that failed
     * before wifi_connect there is nothing either. A teardown with a
     * precondition is a teardown that gets skipped on the path that needed it
     * most. */
    wifi_shutdown();
    platform_exit();
}

int main(void) {
    pspDebugScreenInit();

    if (platform_init() != 0) {
        pspDebugScreenPrintf("platform init failed\n");
        sceKernelDelayThread(3 * 1000 * 1000);
        sceKernelExitGame();
        return 1;
    }

    trace_init(PROBE_BUILD_NAME);
    if (platform_keep_awake() < 0) trace("keep-awake thread did not start: the screen may turn off during playback");
    if (PROBE_CPU_MHZ) {
        int prc = scePowerSetClockFrequency(PROBE_CPU_MHZ, PROBE_CPU_MHZ, PROBE_CPU_MHZ / 2);
        trace("cpu: asked %d MHz -> 0x%08X, now %d", PROBE_CPU_MHZ, (unsigned)prc, scePowerGetCpuClockFrequency());
    }
    trace("%s %s started", PROBE_MODE ? "probe" : "app", HEXFIN_VERSION);

    memwatch_reset();

    if (PROBE_SKIP_RENDER) {
        trace("render_init skipped (PROBE_SKIP_RENDER)");
    } else if (render_init() != 0) {
        /* Not fatal: pspDebugScreenPrintf works regardless, and a video
         * frame with nowhere to go is refused by render_target() itself
         * (ui/render.c). */
        pspDebugScreenPrintf("render_init failed -- continuing text-only\n");
    }
    trace(PROBE_SKIP_RENDER ? "no display set up" : "render_init done");

    /* Before any source, and so before either decoder: mpeg_vsh must be the
     * sceMpeg that registers first. A failure is reported and the run
     * carries on down the game-side path, so the log still says how far
     * that gets. */
    if (PROBE_ME_RUNTIME) {
        int mrc = me_runtime_load(PROBE_ME_BOOT_MODE);
        trace("me runtime (boot mode %d) -> 0x%08X", PROBE_ME_BOOT_MODE, (unsigned)mrc);
    }

    if (PROBE_MODE) {
        probe_main();
    } else {
        app_env env;
        char    cfg[256];

        cfg_path(cfg, sizeof cfg);
        memset(&env, 0, sizeof env);
        env.version        = HEXFIN_VERSION;
        env.default_server = PROBE_JF_SERVER;
        env.cfg_path       = cfg;
        env.ap_config      = PROBE_AP_CONFIG;
        env.wifi_timeout_ms = PROBE_WIFI_TIMEOUT_MS;
        app_run(&env);
    }

    trace("HOME: tearing down");
    teardown();
    trace("teardown done");
    return 0;
}
