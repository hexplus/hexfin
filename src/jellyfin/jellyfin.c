/* See jellyfin/jellyfin.h. The text of every request is built, and every
 * answer read, by jellyfin/requests.c, jellyfin/items.c and jellyfin/json.c,
 * where the host build checks them; this file is the order the calls go in.
 *
 * Only compiled for the console, with the guard net/http.c uses. */
#if defined(__PSP__)

#include "jellyfin/jellyfin.h"

#include "jellyfin/json.h"
#include "media/source.h"
#include "net/http.h"
#include "net/http_parse.h"
#include "platform/psp_platform.h"
#include "platform/trace.h"

#include <pspkernel.h>
#include <stdio.h>
#include <string.h>

/* One answer at a time, whole. A PlaybackInfo answer lists every stream in
 * the file -- a film with a dozen subtitle tracks runs to tens of kB -- and
 * a page of Continue Watching is about 1 kB an item, so this is sized for
 * those rather than for the small calls. */
#define JF_ANSWER_MAX (96u * 1024u)

/* How long a Quick Connect code is waited on, and how often the server is
 * asked whether it has been entered. The server's own codes expire after
 * about ten minutes; five is long enough to find a phone. */
#define JF_QC_TIMEOUT_MS (5u * 60u * 1000u)
#define JF_QC_POLL_MS    2000u

#define JF_ERRLEN 256

static char g_answer[JF_ANSWER_MAX];
static char g_url[HTTP_HOST_MAX + HTTP_PATH_MAX + 16];
static char g_auth[HTTP_EXTRA_HEADERS_MAX];
static char g_body[4096];
static char g_path[512];
static char g_server[JF_SERVER_MAX];
static char g_version[32];
static char g_name[128];
static char g_user_name[128];
static char g_play_session[JF_ID_MAX];
static char g_item_id[JF_ID_MAX];      /* the item jf_open_item last opened */
static char g_media_source[JF_ID_MAX]; /* its MediaSources.0.Id */
static char g_report_body[512];        /* jf_report's own, so a report thread shares nothing with the rest */
static char g_err[JF_ERRLEN];

static char      g_cfg_path[256];
static jf_config g_cfg;

static void err_append(size_t *n, const char *s) {
    while (s && *s && *n + 1 < sizeof g_err) g_err[(*n)++] = *s++;
    g_err[*n] = 0;
}

/* A reason, cut to fit rather than refused: most of a sentence is still
 * worth reading. */
static void set_err(const char *what, const char *why) {
    size_t n = 0;

    g_err[0] = 0;
    err_append(&n, what);
    if (why && why[0]) {
        err_append(&n, ": ");
        err_append(&n, why);
    }
}

const char *jf_error(void) { return g_err; }
const char *jf_item_name(void) { return g_name; }
const char *jf_user_name(void) { return g_user_name; }

/* ---------------------------------------------------------------- config */

void jf_config_save(void) {
    size_t len = jf_config_format(&g_cfg, g_body, sizeof g_body);
    SceUID fd;

    if (len == 0 || !g_cfg_path[0]) return;
    fd = sceIoOpen(g_cfg_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        trace("jellyfin: could not save %s (0x%08X)", g_cfg_path, (unsigned)fd);
        return;
    }
    sceIoWrite(fd, g_body, len);
    sceIoClose(fd);
}

/* A device id this console keeps. Jellyfin ties a session to it, and a new
 * sign-in from the same id replaces the old session instead of adding one,
 * so it is made once and then kept with the token. */
static void make_device_id(void) {
    unsigned long long t = (unsigned long long)sceKernelGetSystemTimeWide();
    unsigned           a = (unsigned)t, b = (unsigned)(t >> 32) ^ (a * 2654435761u);

    snprintf(g_cfg.device_id, sizeof g_cfg.device_id, "psp-%08x%08x", a, b);
}

void jf_config_load(const char *path) {
    SceUID fd;
    int    n = 0;

    snprintf(g_cfg_path, sizeof g_cfg_path, "%s", path);
    memset(&g_cfg, 0, sizeof g_cfg);
    fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd >= 0) {
        n = sceIoRead(fd, g_body, sizeof g_body - 1);
        sceIoClose(fd);
    }
    /* Parsed even when there is no file, which is what sets the defaults. */
    jf_config_parse(g_body, n > 0 ? (size_t)n : 0u, &g_cfg);

    if (!jf_id_is_safe(g_cfg.device_id)) {
        make_device_id();
        g_cfg.token[0]   = 0;
        g_cfg.user_id[0] = 0;
        jf_config_save();
    }
}

jf_config *jf_config_get(void) { return &g_cfg; }

const char *jf_server(const char *fallback) { return g_cfg.server[0] ? g_cfg.server : fallback; }

/* ------------------------------------------------------------ the calls */

/* Rebuilds the Authorization header from the current sign-in. */
static int make_auth(void) {
    return jf_auth_header(g_auth, sizeof g_auth, g_version, g_cfg.device_id, g_cfg.token) != 0;
}

/* One request to `path` on the server, answer in g_answer. */
static int call(const char *method, const char *path, const char *body, uint32_t *out_len) {
    http_request r;
    int          n = snprintf(g_url, sizeof g_url, "%s%s", g_server, path);

    if (n < 0 || (size_t)n >= sizeof g_url) {
        set_err("the request URL is too long", path);
        return SOURCE_ERR_ARG;
    }
    memset(&r, 0, sizeof r);
    r.method   = method;
    r.url      = g_url;
    r.headers  = g_auth;
    r.body     = body;
    r.body_len = body ? (uint32_t)strlen(body) : 0;
    return http_fetch(&r, g_answer, sizeof g_answer, out_len);
}

/* ------------------------------------------------------------ signing in */

/* 1 the kept token works, 0 it does not (and has been dropped), -1 the
 * question could not be asked at all. */
static int token_works(void) {
    uint32_t len = 0;
    char     id[JF_ID_MAX];
    int      rc;

    if (!g_cfg.token[0]) return 0;
    rc = call("GET", "/Users/Me", NULL, &len);
    if (rc == SOURCE_OK && json_get_string(g_answer, len, "Id", id, sizeof id) == JSON_OK && jf_id_is_safe(id)) {
        memcpy(g_cfg.user_id, id, sizeof id);
        json_get_string(g_answer, len, "Name", g_user_name, sizeof g_user_name);
        return 1;
    }
    if (http_status() == 401 || http_status() == 403) {
        trace("jellyfin: the kept sign-in was refused (%d), signing in again", http_status());
        g_cfg.token[0]   = 0;
        g_cfg.user_id[0] = 0;
        jf_config_save();
        return 0;
    }
    set_err("could not reach the Jellyfin server", http_error());
    return -1;
}

/* Sleeps `ms` in slices, and says whether HOME was pressed meanwhile. */
static int wait_or_home(unsigned ms) {
    unsigned waited;
    for (waited = 0; waited < ms; waited += 100) {
        if (platform_exit_requested()) return 1;
        sceKernelDelayThread(100 * 1000);
    }
    return platform_exit_requested();
}

static int quick_connect(jf_show_fn show) {
    char     secret[JF_ID_MAX], code[16], line[80];
    uint32_t len = 0;
    unsigned waited;
    int      rc;

    rc = call("POST", "/QuickConnect/Initiate", NULL, &len);
    if (rc != SOURCE_OK) {
        if (http_status() == 401)
            set_err("Quick Connect is switched off on the server -- turn it on under Dashboard > General", NULL);
        else
            set_err("the server would not start Quick Connect", http_error());
        return -1;
    }
    if (json_get_string(g_answer, len, "Secret", secret, sizeof secret) != JSON_OK || !jf_id_is_safe(secret) ||
        json_get_string(g_answer, len, "Code", code, sizeof code) != JSON_OK || !code[0]) {
        set_err("the server's Quick Connect answer had no usable code", NULL);
        return -1;
    }

    trace("jellyfin: Quick Connect code %s", code);
    snprintf(line, sizeof line, "Quick Connect code:  %s", code);
    if (show) show(line, "Enter it in a signed-in Jellyfin app: Settings > Quick Connect");

    for (waited = 0;; waited += JF_QC_POLL_MS) {
        int authenticated = 0;

        if (waited >= JF_QC_TIMEOUT_MS) {
            set_err("nobody entered the Quick Connect code in time -- try again for a new one", NULL);
            return -1;
        }
        if (wait_or_home(JF_QC_POLL_MS)) {
            set_err("stopped while waiting for the Quick Connect code", NULL);
            return -1;
        }
        snprintf(g_path, sizeof g_path, "/QuickConnect/Connect?Secret=%s", secret);
        rc = call("GET", g_path, NULL, &len);
        if (rc != SOURCE_OK) {
            if (http_status() == 404) set_err("the Quick Connect code expired -- try again for a new one", NULL);
            else set_err("lost the server while waiting for the Quick Connect code", http_error());
            return -1;
        }
        if (json_get_bool(g_answer, len, "Authenticated", &authenticated) == JSON_OK && authenticated) break;
    }

    snprintf(g_body, sizeof g_body, "{\"Secret\":\"%s\"}", secret);
    rc = call("POST", "/Users/AuthenticateWithQuickConnect", g_body, &len);
    if (rc != SOURCE_OK) {
        set_err("the code was accepted but the sign-in failed", http_error());
        return -1;
    }
    if (json_get_string(g_answer, len, "AccessToken", g_cfg.token, sizeof g_cfg.token) != JSON_OK ||
        !jf_id_is_safe(g_cfg.token) ||
        json_get_string(g_answer, len, "User.Id", g_cfg.user_id, sizeof g_cfg.user_id) != JSON_OK ||
        !jf_id_is_safe(g_cfg.user_id)) {
        g_cfg.token[0] = 0;
        set_err("the server's sign-in answer had no usable token", NULL);
        return -1;
    }
    json_get_string(g_answer, len, "User.Name", g_user_name, sizeof g_user_name);

    jf_config_save();
    return make_auth() ? 0 : -1;
}

int jf_sign_in(const char *server, const char *version, jf_show_fn show) {
    int rc;

    g_err[0]       = 0;
    g_name[0]      = 0;
    g_user_name[0] = 0;
    snprintf(g_version, sizeof g_version, "%s", version);
    snprintf(g_server, sizeof g_server, "%s", server);
    {
        size_t n = strlen(g_server);
        while (n > 0 && g_server[n - 1] == '/') g_server[--n] = 0;
    }
    trace("jellyfin: device %s, %s", g_cfg.device_id, g_cfg.token[0] ? "has a kept sign-in" : "not signed in");

    if (!make_auth()) {
        set_err("the settings file is damaged -- delete it and sign in again", g_cfg_path);
        return -1;
    }

    rc = token_works();
    if (rc < 0) return -1;
    if (rc == 0) {
        if (!make_auth()) return -1;
        if (quick_connect(show) != 0) return -1;
    }
    trace("jellyfin: signed in as user %s", g_cfg.user_id);
    return 0;
}

void jf_sign_out(void) {
    uint32_t len = 0;

    if (g_cfg.token[0]) {
        if (call("POST", "/Sessions/Logout", NULL, &len) != SOURCE_OK)
            trace("jellyfin: logout not confirmed: %s", http_error());
    }
    g_cfg.token[0]   = 0;
    g_cfg.user_id[0] = 0;
    g_user_name[0]   = 0;
    jf_config_save();
    make_auth();
    http_set_stream_headers(NULL);
}

/* ------------------------------------------------------------- browsing */

int jf_list(jf_list_kind kind, const char *parent_id, int start, int max, jf_item *out, int *count, int *total) {
    uint32_t len = 0;
    int      n;

    *count = 0;
    *total = 0;
    if (start < 0) start = 0;
    switch (kind) {
        case JF_LIST_VIEWS:
            n = snprintf(g_path, sizeof g_path, "/UserViews?userId=%s", g_cfg.user_id);
            break;
        case JF_LIST_RESUME:
            n = snprintf(g_path, sizeof g_path,
                         "/UserItems/Resume?userId=%s&mediaTypes=Video&startIndex=%d&limit=%d"
                         "&enableImages=false&enableTotalRecordCount=true&fields=",
                         g_cfg.user_id, start, max);
            break;
        default:
            if (!jf_id_is_safe(parent_id)) {
                set_err("the folder id is not a Jellyfin id", parent_id);
                return -1;
            }
            /* Episode order inside a season, name order everywhere else:
             * the two index numbers are empty outside a series, so they
             * fall through to the name. */
            n = snprintf(g_path, sizeof g_path,
                         "/Items?userId=%s&parentId=%s&startIndex=%d&limit=%d"
                         "&sortBy=ParentIndexNumber,IndexNumber,SortName&sortOrder=Ascending"
                         "&enableImages=false&enableTotalRecordCount=true&fields=",
                         g_cfg.user_id, parent_id, start, max);
            break;
    }
    if (n < 0 || (size_t)n >= sizeof g_path) {
        set_err("the list request is too long", NULL);
        return -1;
    }
    if (call("GET", g_path, NULL, &len) != SOURCE_OK) {
        set_err("the server would not list this", http_error());
        return -1;
    }
    if (jf_parse_items(g_answer, len, out, max, count, total) != 0) {
        set_err("the server's list answer could not be read", NULL);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------- playing */

int jf_open_item(const char *item_id, unsigned long long start_ticks, char *url, size_t cap) {
    char     transcoding[HTTP_PATH_MAX];
    uint32_t len = 0;
    int      rc;

    g_name[0]         = 0;
    g_play_session[0] = 0;
    g_item_id[0]      = 0;
    g_media_source[0] = 0;
    if (!jf_id_is_safe(item_id)) {
        set_err("the item id is not a Jellyfin id", item_id);
        return -1;
    }

    /* The name first, only for the screen: a failure here is not one. */
    snprintf(g_path, sizeof g_path, "/Items/%s?userId=%s", item_id, g_cfg.user_id);
    if (call("GET", g_path, NULL, &len) == SOURCE_OK) json_get_string(g_answer, len, "Name", g_name, sizeof g_name);

    if (jf_playback_info_body(g_body, sizeof g_body, g_cfg.user_id, start_ticks) == 0) {
        set_err("the playback request could not be built", NULL);
        return -1;
    }
    snprintf(g_path, sizeof g_path, "/Items/%s/PlaybackInfo", item_id);
    rc = call("POST", g_path, g_body, &len);
    if (rc != SOURCE_OK) {
        if (http_status() == 404) set_err("the server has no item with that id", item_id);
        else set_err("the server refused to play the item", http_error());
        return -1;
    }

    {
        json_err e = json_get_string(g_answer, len, "MediaSources.0.TranscodingUrl", transcoding, sizeof transcoding);
        if (e != JSON_OK) {
            char code[64];
            if (json_get_string(g_answer, len, "ErrorCode", code, sizeof code) == JSON_OK)
                set_err("the server will not play this item for the PSP", code);
            else if (e == JSON_TOOBIG)
                set_err("the server's stream URL is longer than this client holds", NULL);
            else
                set_err("the server offered no transcode for the PSP", NULL);
            return -1;
        }
    }
    if (json_get_string(g_answer, len, "PlaySessionId", g_play_session, sizeof g_play_session) != JSON_OK ||
        !jf_id_is_safe(g_play_session))
        g_play_session[0] = 0;
    if (json_get_string(g_answer, len, "MediaSources.0.Id", g_media_source, sizeof g_media_source) != JSON_OK ||
        !jf_id_is_safe(g_media_source))
        g_media_source[0] = 0;
    snprintf(g_item_id, sizeof g_item_id, "%s", item_id);
    if (!jf_stream_url(url, cap, g_server, transcoding, start_ticks)) {
        set_err("the server's stream URL is not one this client will request", NULL);
        return -1;
    }

    http_set_stream_headers(g_auth);
    trace("jellyfin: item \"%s\" -> %u-character stream URL", g_name, (unsigned)strlen(url));
    return 0;
}

int jf_report(jf_report_kind kind, unsigned long long position_ticks, int paused, const char *event) {
    static const char *const paths[] = {"/Sessions/Playing", "/Sessions/Playing/Progress", "/Sessions/Playing/Stopped"};
    static char              url[HTTP_HOST_MAX + 64];
    static char              answer[256];
    http_request             r;
    uint32_t                 len = 0;
    int                      rc;

    if (!g_item_id[0]) return -1;
    if (jf_report_body(g_report_body, sizeof g_report_body, kind, g_item_id, g_media_source, g_play_session,
                       position_ticks, paused, kind == JF_REPORT_PROGRESS ? event : NULL) == 0)
        return -1;

    /* Its own URL and answer buffers, not call()'s: a report runs on a
     * thread of its own while the rest of this file may be in use. */
    snprintf(url, sizeof url, "%s%s", g_server, paths[kind]);
    memset(&r, 0, sizeof r);
    r.method   = "POST";
    r.url      = url;
    r.headers  = g_auth;
    r.body     = g_report_body;
    r.body_len = (uint32_t)strlen(g_report_body);
    rc         = http_fetch(&r, answer, sizeof answer, &len);
    if (rc != SOURCE_OK) {
        trace("jellyfin: %s report at %u s not taken: %s", paths[kind], (unsigned)(position_ticks / 10000000u),
              http_error());
        return -1;
    }
    return 0;
}

void jf_stop_item(void) {
    uint32_t len = 0;

    if (!g_play_session[0]) return;
    snprintf(g_path, sizeof g_path, "/Videos/ActiveEncodings?deviceId=%s&playSessionId=%s", g_cfg.device_id,
             g_play_session);
    if (call("DELETE", g_path, NULL, &len) != SOURCE_OK) trace("jellyfin: stopping the transcode: %s", http_error());
    g_play_session[0] = 0;
}

#endif /* __PSP__ */
