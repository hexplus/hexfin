/* See jellyfin/json.h and jellyfin/requests.h.
 *
 * The JSON reader eats a server's answer, so the checks that matter most are
 * the hostile ones: a document cut off in every possible place must be
 * refused without reading past its end (AddressSanitizer is what makes that
 * check mean something), and nesting past the bound must be refused rather
 * than recursed into. The request builders put server-supplied values into
 * header lines and a JSON body, so the checks there are that a value with a
 * quote or a line break in it never gets in. */

#include "jellyfin/items.h"
#include "jellyfin/json.h"
#include "jellyfin/requests.h"

#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Shaped like a real PlaybackInfo answer: the value wanted sits after an
 * array of objects that hold keys of the same name, and its '&'s are
 * escaped the way Jellyfin's serializer escapes them. */
static const char k_playback[] =
    "{\"MediaSources\":[{\"Protocol\":\"File\",\"Id\":\"abc\",\"MediaStreams\":["
    "{\"Codec\":\"h264\",\"Id\":\"nope\",\"IsDefault\":true,\"Height\":1040},"
    "{\"Codec\":\"aac\",\"Title\":\"Stereo \\\"Main\\\" \\u00e9\\ud83d\\ude00\"}],"
    "\"SupportsDirectPlay\":false,\"RunTimeTicks\":36000000000,"
    "\"TranscodingUrl\":\"/videos/abc/stream.mp4?DeviceId=psp\\u0026MediaSourceId=abc\\u0026ApiKey=tok\","
    "\"TranscodingSubProtocol\":\"http\"}],"
    "\"PlaySessionId\":\"f00d\"}";

static int t_json_paths(char *note, unsigned n) {
    char      s[256];
    int       b = -1;
    long long v = 0;
    size_t    len = strlen(k_playback);

    if (json_get_string(k_playback, len, "MediaSources.0.TranscodingUrl", s, sizeof s) != JSON_OK ||
        strcmp(s, "/videos/abc/stream.mp4?DeviceId=psp&MediaSourceId=abc&ApiKey=tok") != 0) {
        snprintf(note, n, "TranscodingUrl came out as \"%s\"", s);
        return 1;
    }
    if (json_get_string(k_playback, len, "PlaySessionId", s, sizeof s) != JSON_OK || strcmp(s, "f00d") != 0) {
        snprintf(note, n, "PlaySessionId came out as \"%s\"", s);
        return 1;
    }
    if (json_get_string(k_playback, len, "MediaSources.0.Id", s, sizeof s) != JSON_OK || strcmp(s, "abc") != 0) {
        snprintf(note, n, "the source's own Id came out as \"%s\" -- a nested Id was matched instead", s);
        return 1;
    }
    if (json_get_string(k_playback, len, "MediaSources.0.MediaStreams.1.Title", s, sizeof s) != JSON_OK ||
        strcmp(s, "Stereo \"Main\" \xC3\xA9\xF0\x9F\x98\x80") != 0) {
        snprintf(note, n, "escapes and a surrogate pair decoded to \"%s\"", s);
        return 1;
    }
    if (json_get_bool(k_playback, len, "MediaSources.0.MediaStreams.0.IsDefault", &b) != JSON_OK || b != 1) {
        snprintf(note, n, "a nested true was not read");
        return 1;
    }
    if (json_get_int(k_playback, len, "MediaSources.0.RunTimeTicks", &v) != JSON_OK || v != 36000000000LL) {
        snprintf(note, n, "RunTimeTicks came out as %lld", v);
        return 1;
    }
    return 0;
}

static int t_json_absent_and_wrong_type(char *note, unsigned n) {
    char   s[16] = "x";
    int    b;
    size_t len = strlen(k_playback);

    if (json_get_string(k_playback, len, "ErrorCode", s, sizeof s) != JSON_NOT_FOUND || s[0]) {
        snprintf(note, n, "a missing key was not reported as missing");
        return 1;
    }
    if (json_get_string(k_playback, len, "MediaSources.3.Id", s, sizeof s) != JSON_NOT_FOUND) {
        snprintf(note, n, "an index past the array's end was not reported as missing");
        return 1;
    }
    if (json_get_string(k_playback, len, "MediaSources.0.SupportsDirectPlay", s, sizeof s) != JSON_TYPE) {
        snprintf(note, n, "a bool read as a string was not refused");
        return 1;
    }
    if (json_get_bool(k_playback, len, "PlaySessionId", &b) != JSON_TYPE) {
        snprintf(note, n, "a string read as a bool was not refused");
        return 1;
    }
    if (json_get_string(k_playback, len, "MediaSources.Id", s, sizeof s) != JSON_TYPE) {
        snprintf(note, n, "a key looked up in an array was not refused");
        return 1;
    }
    if (json_get_string(k_playback, len, "MediaSources.0.TranscodingUrl", s, sizeof s) != JSON_TOOBIG || s[0]) {
        snprintf(note, n, "a string longer than the buffer was not refused, or left something behind");
        return 1;
    }
    return 0;
}

/* Every prefix of a valid document is either still answerable (the value
 * came before the cut) or refused. Each prefix is copied into a buffer of
 * exactly its own size, so a read one byte too far is ASan's to catch. */
static int t_json_truncated_everywhere(char *note, unsigned n) {
    size_t len = strlen(k_playback);
    size_t whole = (size_t)(strstr(k_playback, "f00d\"") - k_playback) + 5; /* the value's closing quote */
    size_t cut;

    for (cut = 0; cut < len; cut++) {
        char    *copy = (char *)malloc(cut ? cut : 1);
        char     s[256];
        json_err e;

        memcpy(copy, k_playback, cut);
        e = json_get_string(copy, cut, "PlaySessionId", s, sizeof s);
        free(copy);
        if (e == JSON_OK && cut < whole) {
            snprintf(note, n, "PlaySessionId was found in the first %u bytes, before it is complete", (unsigned)cut);
            return 1;
        }
        if (e != JSON_OK && cut >= whole) {
            snprintf(note, n, "PlaySessionId was not found in %u bytes that hold all of it", (unsigned)cut);
            return 1;
        }
    }
    return 0;
}

static int t_json_malformed_refused(char *note, unsigned n) {
    static const char *const bad[] = {
        "{\"a\" 1}", "{\"a\":1,}x", "{\"a\":\"x\ny\"}", "{\"a\":\"\\q\"}", "{\"a\":\"\\u12\"}", "{\"a\":tru}",
        "{\"b\":[1,2,,3],\"a\":\"x\"}", "{\"b\":{\"c\":1 \"d\":2},\"a\":\"x\"}",
    };
    unsigned i;

    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char     s[16];
        json_err e = json_get_string(bad[i], strlen(bad[i]), "a", s, sizeof s);
        if (e == JSON_OK) {
            snprintf(note, n, "malformed document %u was read as \"%s\"", i, s);
            return 1;
        }
    }
    return 0;
}

static int t_json_depth_bounded(char *note, unsigned n) {
    static char doc[4096];
    size_t      k = 0, d;
    char        s[8];

    k += (size_t)sprintf(doc + k, "{\"b\":");
    for (d = 0; d < 1000; d++) doc[k++] = '[';
    for (d = 0; d < 1000; d++) doc[k++] = ']';
    k += (size_t)sprintf(doc + k, ",\"a\":\"x\"}");

    if (json_get_string(doc, k, "a", s, sizeof s) != JSON_MALFORMED) {
        snprintf(note, n, "1000 levels of nesting were walked rather than refused");
        return 1;
    }
    return 0;
}

static int t_auth_header(char *note, unsigned n) {
    char   h[512];
    size_t len = jf_auth_header(h, sizeof h, "0.1.0", "psp-0123abcd", "0123456789abcdef");

    if (len == 0 || strcmp(h, "Authorization: MediaBrowser Client=\"Hexfin\", Device=\"PSP\", "
                              "DeviceId=\"psp-0123abcd\", Version=\"0.1.0\", Token=\"0123456789abcdef\"\r\n") != 0) {
        snprintf(note, n, "header came out as \"%s\"", h);
        return 1;
    }
    if (jf_auth_header(h, sizeof h, "0.1.0", "psp-1", NULL) == 0 || strstr(h, "Token")) {
        snprintf(note, n, "a header with no token was refused or named one");
        return 1;
    }
    if (jf_auth_header(h, sizeof h, "0.1.0", "psp-1", "abc\r\nX-Evil: 1") != 0 ||
        jf_auth_header(h, sizeof h, "0.1.0", "psp\"1", NULL) != 0) {
        snprintf(note, n, "an id carrying a line break or a quote was let into the header");
        return 1;
    }
    if (jf_auth_header(h, 40, "0.1.0", "psp-1", NULL) != 0 || h[0]) {
        snprintf(note, n, "a header that does not fit was not refused");
        return 1;
    }
    return 0;
}

static int t_playback_body_is_json(char *note, unsigned n) {
    static char body[4096];
    char        s[64];
    int         b = -1;
    long long   start = -1;
    size_t      len   = jf_playback_info_body(body, sizeof body, "0f1e2d3c", 6000000000ull);

    if (len == 0) {
        snprintf(note, n, "the body did not fit in 4 kB");
        return 1;
    }
    if (json_get_string(body, len, "DeviceProfile.TranscodingProfiles.0.Container", s, sizeof s) != JSON_OK ||
        strcmp(s, "mp4") != 0) {
        snprintf(note, n, "the transcoding container read back as \"%s\"", s);
        return 1;
    }
    if (json_get_string(body, len, "DeviceProfile.CodecProfiles.0.Conditions.0.Value", s, sizeof s) != JSON_OK ||
        strcmp(s, "baseline|constrained baseline") != 0) {
        snprintf(note, n, "the H.264 profile condition read back as \"%s\"", s);
        return 1;
    }
    if (json_get_string(body, len, "UserId", s, sizeof s) != JSON_OK || strcmp(s, "0f1e2d3c") != 0) {
        snprintf(note, n, "UserId read back as \"%s\"", s);
        return 1;
    }
    if (json_get_int(body, len, "StartTimeTicks", &start) != JSON_OK || start != 6000000000LL) {
        snprintf(note, n, "the start position read back as %lld", start);
        return 1;
    }
    if (json_get_bool(body, len, "EnableDirectPlay", &b) != JSON_OK || b != 0) {
        snprintf(note, n, "direct play was not switched off");
        return 1;
    }
    if (jf_playback_info_body(body, sizeof body, "x\",\"EnableDirectPlay\":true,\"y\":\"", 0) != 0) {
        snprintf(note, n, "a user id that rewrites the body was let in");
        return 1;
    }
    snprintf(note, n, "%u bytes", (unsigned)len);
    return 0;
}

static int t_report_bodies(char *note, unsigned n) {
    char      body[512];
    char      s[64];
    long long pos = -1;
    int       b   = -1;
    size_t    len;

    len = jf_report_body(body, sizeof body, JF_REPORT_PROGRESS, "item1", "src1", "sess1", 6000000000ull, 1, "pause");
    if (len == 0 || json_get_string(body, len, "EventName", s, sizeof s) != JSON_OK || strcmp(s, "pause") != 0 ||
        json_get_int(body, len, "PositionTicks", &pos) != JSON_OK || pos != 6000000000LL ||
        json_get_bool(body, len, "IsPaused", &b) != JSON_OK || b != 1 ||
        json_get_string(body, len, "PlaySessionId", s, sizeof s) != JSON_OK || strcmp(s, "sess1") != 0) {
        snprintf(note, n, "the progress report read back wrong: %s", body);
        return 1;
    }
    len = jf_report_body(body, sizeof body, JF_REPORT_STOPPED, "item1", "src1", "sess1", 42, 0, "pause");
    if (len == 0 || strstr(body, "IsPaused") || strstr(body, "EventName") ||
        json_get_int(body, len, "PositionTicks", &pos) != JSON_OK || pos != 42) {
        snprintf(note, n, "the stop report carried start/progress fields: %s", body);
        return 1;
    }
    len = jf_report_body(body, sizeof body, JF_REPORT_START, "item1", NULL, "", 0, 0, NULL);
    if (len == 0 || strstr(body, "MediaSourceId") || strstr(body, "PlaySessionId")) {
        snprintf(note, n, "a start without a session still named one: %s", body);
        return 1;
    }
    if (jf_report_body(body, sizeof body, JF_REPORT_PROGRESS, "item\",\"x", "src1", "sess1", 0, 0, NULL) != 0 ||
        jf_report_body(body, sizeof body, JF_REPORT_PROGRESS, "item1", "src1", "sess1", 0, 0, "a\"b") != 0) {
        snprintf(note, n, "an id or event that rewrites the body was let in");
        return 1;
    }
    return 0;
}

static int t_stream_url(char *note, unsigned n) {
    char u[128];

    if (!jf_stream_url(u, sizeof u, "http://192.168.100.78:8096/", "/videos/a/stream.mp4?x=1&y=2", 0) ||
        strcmp(u, "http://192.168.100.78:8096/videos/a/stream.mp4?x=1&y=2") != 0) {
        snprintf(note, n, "joined URL came out as \"%s\"", u);
        return 1;
    }
    if (!jf_stream_url(u, sizeof u, "http://h", "/v/s.mp4?x=1", 6000000000ull) ||
        strcmp(u, "http://h/v/s.mp4?x=1&StartTimeTicks=6000000000") != 0) {
        snprintf(note, n, "a start position was added as \"%s\"", u);
        return 1;
    }
    if (!jf_stream_url(u, sizeof u, "http://h", "/v/s.mp4?starttimeticks=5", 70) ||
        strcmp(u, "http://h/v/s.mp4?starttimeticks=5") != 0) {
        snprintf(note, n, "a URL that already starts somewhere got a second start: \"%s\"", u);
        return 1;
    }
    if (jf_stream_url(u, sizeof u, "http://h", "/a b", 0) || jf_stream_url(u, sizeof u, "http://h", "/a\r\nX: 1", 0) ||
        jf_stream_url(u, sizeof u, "http://h", "http://elsewhere/a", 0)) {
        snprintf(note, n, "a TranscodingUrl with a space, a line break or its own host was accepted");
        return 1;
    }
    if (jf_stream_url(u, 12, "http://h", "/videos/a/stream.mp4", 0)) {
        snprintf(note, n, "a URL that does not fit was accepted");
        return 1;
    }
    return 0;
}

static int t_config_round_trip(char *note, unsigned n) {
    jf_config c, back;
    char      text[512];
    size_t    len;

    memset(&c, 0, sizeof c);
    strcpy(c.device_id, "psp-00ff");
    strcpy(c.user_id, "0123abcd");
    strcpy(c.token, "feedface");
    strcpy(c.server, "http://192.168.100.78:8096");
    c.show_clock   = 0;
    c.show_battery = 1;
    len = jf_config_format(&c, text, sizeof text);
    jf_config_parse(text, len, &back);
    if (len == 0 || memcmp(&c, &back, sizeof c) != 0) {
        snprintf(note, n, "the saved config did not read back the same");
        return 1;
    }

    /* CRLF line ends (a file edited on Windows), an unknown key, and values
     * that are not safe: kept, ignored and dropped respectively. */
    {
        static const char messy[] = "device_id=psp-1\r\nfavourite=yes\r\ntoken=bad token\r\nuser_id=u1\r\n"
                                    "server=https://x\r\ndisplay=sideways\r\n";
        jf_config_parse(messy, strlen(messy), &back);
        if (strcmp(back.device_id, "psp-1") != 0 || strcmp(back.user_id, "u1") != 0 || back.token[0] ||
            back.server[0] || !back.show_clock || !back.show_battery) {
            snprintf(note, n, "a hand-edited file read as device \"%s\" user \"%s\" token \"%s\" server \"%s\"",
                     back.device_id, back.user_id, back.token, back.server);
            return 1;
        }
    }
    if (jf_server_is_safe("http://a b") || jf_server_is_safe("http://") || jf_server_is_safe("ftp://x") ||
        !jf_server_is_safe("http://jellyfin.lan:8096")) {
        snprintf(note, n, "server address checking is wrong");
        return 1;
    }
    return 0;
}

static int t_array_walk(char *note, unsigned n) {
    static const char doc[] = "{\"Items\": [ {\"a\":1}, \"two\" ,[3,[4]], {} ], \"Empty\":[ ], \"Obj\":{}}";
    static const char *const want[] = {"{\"a\":1}", "\"two\"", "[3,[4]]", "{}"};
    json_span arr, el;
    size_t    pos = 0;
    int       k   = 0;

    if (json_get_span(doc, strlen(doc), "Items", &arr) != JSON_OK) {
        snprintf(note, n, "the array was not found");
        return 1;
    }
    while (json_array_next(&arr, &pos, &el) == JSON_OK) {
        if (k >= 4 || el.len != strlen(want[k]) || memcmp(el.p, want[k], el.len) != 0) {
            snprintf(note, n, "element %d came out as \"%.*s\"", k, (int)el.len, el.p);
            return 1;
        }
        k++;
    }
    if (k != 4) {
        snprintf(note, n, "walked %d elements, not 4", k);
        return 1;
    }
    pos = 0;
    if (json_get_span(doc, strlen(doc), "Empty", &arr) != JSON_OK ||
        json_array_next(&arr, &pos, &el) != JSON_NOT_FOUND) {
        snprintf(note, n, "an empty array yielded an element");
        return 1;
    }
    pos = 0;
    if (json_get_span(doc, strlen(doc), "Obj", &arr) != JSON_OK || json_array_next(&arr, &pos, &el) != JSON_TYPE) {
        snprintf(note, n, "an object was walked as if it were an array");
        return 1;
    }
    return 0;
}

/* Shaped like /UserItems/Resume and /UserViews: episodes and a movie, UTF-8
 * names, an item with no usable id, libraries of two kinds, and more
 * records on the server than on the page. */
static const char k_list[] =
    "{\"Items\":["
    "{\"Name\":\"Episodio 1\",\"Id\":\"9fbc78ebb17c7a615d03d9bb56e1dc7e\",\"RunTimeTicks\":30870833333,"
    "\"IsFolder\":false,\"Type\":\"Episode\",\"ParentIndexNumber\":1,\"IndexNumber\":1,"
    "\"SeriesName\":\"Perdidos en el espacio\",\"UserData\":{\"PlaybackPositionTicks\":8374870000,\"Played\":false}},"
    "{\"Name\":\"Pok\\u00e9mon: The Movie\",\"Id\":\"aa\",\"IsFolder\":false,\"Type\":\"Movie\",\"ProductionYear\":1998},"
    "{\"Name\":\"no id\",\"Id\":\"x y\",\"Type\":\"Movie\"},"
    "{\"Name\":\"Shows\",\"Id\":\"a656b907eb3a73532e40e44b968d0225\",\"IsFolder\":true,\"Type\":\"CollectionFolder\","
    "\"CollectionType\":\"tvshows\"},"
    "{\"Name\":\"Metal Music\",\"Id\":\"bb\",\"IsFolder\":true,\"Type\":\"CollectionFolder\",\"CollectionType\":\"music\"}"
    "],\"TotalRecordCount\":45,\"StartIndex\":0}";

static int t_items_parse(char *note, unsigned n) {
    jf_item items[8];
    int     count = 0, total = 0;
    char    label[160];

    if (jf_parse_items(k_list, strlen(k_list), items, 8, &count, &total) != 0 || count != 4 || total != 45) {
        snprintf(note, n, "read %d items of %d, not 4 of 45", count, total);
        return 1;
    }
    if (items[0].season != 1 || items[0].episode != 1 || items[0].position_ticks != 8374870000ull ||
        items[0].runtime_ticks != 30870833333ull || !jf_item_playable(&items[0])) {
        snprintf(note, n, "the episode's numbers or position were misread");
        return 1;
    }
    jf_item_label(&items[0], 1, label, sizeof label);
    if (strcmp(label, "Perdidos en el espacio - S1E1 Episodio 1") != 0) {
        snprintf(note, n, "episode label \"%s\"", label);
        return 1;
    }
    jf_item_label(&items[1], 1, label, sizeof label);
    if (strcmp(label, "Pok\xC3\xA9mon: The Movie (1998)") != 0) {
        snprintf(note, n, "movie label \"%s\"", label);
        return 1;
    }
    if (!jf_item_browsable(&items[2]) || jf_item_playable(&items[2]) || !jf_view_has_video(&items[2]) ||
        jf_view_has_video(&items[3])) {
        snprintf(note, n, "a library was classified wrongly");
        return 1;
    }
    if (jf_parse_items(k_list, strlen(k_list), items, 2, &count, &total) != 0 || count != 2) {
        snprintf(note, n, "a page limit of 2 read %d", count);
        return 1;
    }
    if (jf_parse_items("{\"Nope\":1}", 10, items, 8, &count, &total) == 0) {
        snprintf(note, n, "an answer with no Items array was accepted");
        return 1;
    }
    return 0;
}

/* A name longer than the record is cut on a character boundary, never in
 * the middle of one. */
static int t_items_long_name_cut(char *note, unsigned n) {
    static char doc[2048];
    jf_item     it;
    int         count, total, k, len = 0;

    len += sprintf(doc + len, "{\"Items\":[{\"Id\":\"ab\",\"Name\":\"");
    for (k = 0; k < 200; k++) len += sprintf(doc + len, "\\u00e9"); /* 2 bytes each in UTF-8 */
    len += sprintf(doc + len, "\"}]}");

    if (jf_parse_items(doc, (size_t)len, &it, 1, &count, &total) != 0 || count != 1) {
        snprintf(note, n, "the item was not read");
        return 1;
    }
    k = (int)strlen(it.name);
    if (k == 0 || k >= JF_ITEM_NAME_MAX || (k % 2) != 0) {
        snprintf(note, n, "the name was cut to %d bytes, splitting a character", k);
        return 1;
    }
    return 0;
}

void test_jellyfin_register(void) {
    test_add("jellyfin", "values are found by path, past same-named keys", t_json_paths);
    test_add("jellyfin", "a missing key, a wrong type and a long string are told apart", t_json_absent_and_wrong_type);
    test_add("jellyfin", "a document cut off anywhere is never read past its end", t_json_truncated_everywhere);
    test_add("jellyfin", "malformed JSON is refused", t_json_malformed_refused);
    test_add("jellyfin", "nesting past the bound is refused, not recursed into", t_json_depth_bounded);
    test_add("jellyfin", "the Authorization header is built, and refuses unsafe ids", t_auth_header);
    test_add("jellyfin", "the PlaybackInfo body is JSON with the profile in it", t_playback_body_is_json);
    test_add("jellyfin", "a TranscodingUrl joins the server only as a clean path", t_stream_url);
    test_add("jellyfin", "playback reports are JSON, and stop reports carry only a position", t_report_bodies);
    test_add("jellyfin", "the saved config round-trips and survives a damaged file", t_config_round_trip);
    test_add("jellyfin", "an array is walked element by element", t_array_walk);
    test_add("jellyfin", "a list answer reads into items with the right labels", t_items_parse);
    test_add("jellyfin", "a name too long for its record is cut on a character boundary", t_items_long_name_cut);
}
