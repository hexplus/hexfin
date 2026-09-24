/* See jellyfin/requests.h. */
#include "jellyfin/requests.h"

#include <string.h>

typedef struct {
    char  *out;
    size_t cap;
    size_t len;
    int    overflow;
} buf;

static void add(buf *b, const char *s) {
    while (*s) {
        if (b->len + 1 >= b->cap) {
            b->overflow = 1;
            break;
        }
        b->out[b->len++] = *s++;
    }
    b->out[b->len] = 0;
}

static void add_dec(buf *b, unsigned v) {
    char     tmp[12];
    unsigned n = 0;
    char     one[2] = {0, 0};

    do {
        tmp[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v && n < sizeof tmp);
    while (n) {
        one[0] = tmp[--n];
        add(b, one);
    }
}

static void add_u64(buf *b, unsigned long long v) {
    char     tmp[24];
    unsigned n = 0;
    char     one[2] = {0, 0};

    do {
        tmp[n++] = (char)('0' + (unsigned)(v % 10u));
        v /= 10u;
    } while (v && n < sizeof tmp);
    while (n) {
        one[0] = tmp[--n];
        add(b, one);
    }
}

static size_t finish(buf *b) {
    if (b->overflow) {
        if (b->cap) b->out[0] = 0;
        return 0;
    }
    return b->len;
}

int jf_id_is_safe(const char *s) {
    size_t n = 0;

    if (!s) return 0;
    for (; s[n]; n++) {
        char ch = s[n];
        if (n + 1 >= JF_ID_MAX) return 0;
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '-' ||
              ch == '_'))
            return 0;
    }
    return n > 0;
}

size_t jf_auth_header(char *out, size_t cap, const char *version, const char *device_id, const char *token) {
    buf b = {out, cap, 0, 0};

    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (!jf_id_is_safe(device_id)) return 0;
    if (token && token[0] && !jf_id_is_safe(token)) return 0;
    /* The version is ours, but it goes inside quotes all the same. */
    if (!version || strpbrk(version, "\"\r\n")) return 0;

    add(&b, "Authorization: MediaBrowser Client=\"" JF_CLIENT_NAME "\", Device=\"" JF_DEVICE_NAME "\", DeviceId=\"");
    add(&b, device_id);
    add(&b, "\", Version=\"");
    add(&b, version);
    add(&b, "\"");
    if (token && token[0]) {
        add(&b, ", Token=\"");
        add(&b, token);
        add(&b, "\"");
    }
    add(&b, "\r\n");
    return finish(&b);
}

/* The device profile, condition by condition. Every one is here because
 * something was measured, not because a PSP spec sheet lists it (PROMPT.md
 * section 9: advertise only what the decoder has proven).
 *
 *  - No DirectPlayProfiles, and direct play, direct stream and stream copy
 *    all switched off: the Media Engine reads fragmented MP4 through this
 *    client's own parser, and only a transcode is guaranteed to be one.
 *  - The transcode is progressive fragmented MP4 over one HTTP response
 *    (Protocol "http", docs/PHASE0_FINDINGS.md section 7), which is what the
 *    pipeline played on hardware.
 *  - VideoProfile baseline|constrained baseline: without it this server
 *    encodes High profile with B-slices, which the Media Engine refuses
 *    (docs/RESEARCH.md section 2). Jellyfin turns it into
 *    -profile:v constrained_baseline and keeps VAAPI.
 *  - 480x272 at most, 8-bit, progressive: the panel, and what decoded.
 *    Heights below 272 decoded too (368x208 coded), so aspect is kept.
 *  - RefFrames 1: every stream that decoded had one; the MMCO rewrite in
 *    media/h264_mmco.c relies on it.
 *  - AAC-LC, two channels, 44.1 kHz: the only audio audio_psp.c has
 *    played, and the rate the PSP's output runs at. 96 kbps because
 *    Jellyfin takes the ceiling of the condition (PHASE0_FINDINGS.md
 *    section 7 got 128 kbps from a 128000 condition).
 *  - Subtitles burned in: there is no subtitle renderer yet. */
static const char k_profile_head[] =
    "{\"DeviceProfile\":{"
    "\"Name\":\"" JF_CLIENT_NAME "\","
    "\"MaxStreamingBitrate\":";

static const char k_profile_tail[] =
    ","
    "\"MusicStreamingTranscodingBitrate\":96000,"
    "\"DirectPlayProfiles\":[],"
    "\"TranscodingProfiles\":[{"
    "\"Container\":\"mp4\",\"Type\":\"Video\",\"VideoCodec\":\"h264\",\"AudioCodec\":\"aac\","
    "\"Protocol\":\"http\",\"Context\":\"Streaming\",\"MaxAudioChannels\":\"2\","
    "\"BreakOnNonKeyFrames\":false,\"CopyTimestamps\":false}],"
    "\"ContainerProfiles\":[],"
    "\"CodecProfiles\":["
    "{\"Type\":\"Video\",\"Codec\":\"h264\",\"Conditions\":["
    "{\"Condition\":\"EqualsAny\",\"Property\":\"VideoProfile\",\"Value\":\"baseline|constrained baseline\","
    "\"IsRequired\":true},"
    "{\"Condition\":\"LessThanEqual\",\"Property\":\"Width\",\"Value\":\"480\",\"IsRequired\":true},"
    "{\"Condition\":\"LessThanEqual\",\"Property\":\"Height\",\"Value\":\"272\",\"IsRequired\":true},"
    "{\"Condition\":\"LessThanEqual\",\"Property\":\"VideoBitDepth\",\"Value\":\"8\",\"IsRequired\":true},"
    "{\"Condition\":\"LessThanEqual\",\"Property\":\"VideoLevel\",\"Value\":\"30\",\"IsRequired\":false},"
    "{\"Condition\":\"LessThanEqual\",\"Property\":\"RefFrames\",\"Value\":\"1\",\"IsRequired\":false},"
    "{\"Condition\":\"LessThanEqual\",\"Property\":\"VideoFramerate\",\"Value\":\"30\",\"IsRequired\":false},"
    "{\"Condition\":\"NotEquals\",\"Property\":\"IsInterlaced\",\"Value\":\"true\",\"IsRequired\":false}"
    "]},"
    "{\"Type\":\"VideoAudio\",\"Codec\":\"aac\",\"Conditions\":["
    "{\"Condition\":\"LessThanEqual\",\"Property\":\"AudioChannels\",\"Value\":\"2\",\"IsRequired\":true},"
    "{\"Condition\":\"LessThanEqual\",\"Property\":\"AudioBitrate\",\"Value\":\"96000\",\"IsRequired\":false},"
    "{\"Condition\":\"Equals\",\"Property\":\"AudioSampleRate\",\"Value\":\"44100\",\"IsRequired\":true},"
    "{\"Condition\":\"EqualsAny\",\"Property\":\"AudioProfile\",\"Value\":\"LC\",\"IsRequired\":true}"
    "]}],"
    "\"SubtitleProfiles\":["
    "{\"Format\":\"srt\",\"Method\":\"Encode\"},{\"Format\":\"subrip\",\"Method\":\"Encode\"},"
    "{\"Format\":\"ass\",\"Method\":\"Encode\"},{\"Format\":\"ssa\",\"Method\":\"Encode\"},"
    "{\"Format\":\"vtt\",\"Method\":\"Encode\"},{\"Format\":\"pgssub\",\"Method\":\"Encode\"},"
    "{\"Format\":\"dvdsub\",\"Method\":\"Encode\"}]"
    "},"
    "\"EnableDirectPlay\":false,\"EnableDirectStream\":false,\"EnableTranscoding\":true,"
    "\"AllowVideoStreamCopy\":false,\"AllowAudioStreamCopy\":false,"
    "\"AutoOpenLiveStream\":true,";

size_t jf_playback_info_body(char *out, size_t cap, const char *user_id, unsigned long long start_ticks) {
    buf b = {out, cap, 0, 0};

    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (!jf_id_is_safe(user_id)) return 0;

    add(&b, k_profile_head);
    add_dec(&b, JF_MAX_STREAMING_BITRATE);
    add(&b, k_profile_tail);
    add(&b, "\"StartTimeTicks\":");
    add_u64(&b, start_ticks);
    add(&b, ",\"MaxStreamingBitrate\":");
    add_dec(&b, JF_MAX_STREAMING_BITRATE);
    add(&b, ",\"UserId\":\"");
    add(&b, user_id);
    add(&b, "\"}");
    return finish(&b);
}

size_t jf_report_body(char *out, size_t cap, jf_report_kind kind, const char *item_id, const char *media_source_id,
                      const char *play_session_id, unsigned long long position_ticks, int paused, const char *event) {
    buf b = {out, cap, 0, 0};

    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (!jf_id_is_safe(item_id)) return 0;
    if (media_source_id && media_source_id[0] && !jf_id_is_safe(media_source_id)) return 0;
    if (play_session_id && play_session_id[0] && !jf_id_is_safe(play_session_id)) return 0;
    if (event && !jf_id_is_safe(event)) return 0;

    add(&b, "{\"ItemId\":\"");
    add(&b, item_id);
    add(&b, "\"");
    if (media_source_id && media_source_id[0]) {
        add(&b, ",\"MediaSourceId\":\"");
        add(&b, media_source_id);
        add(&b, "\"");
    }
    if (play_session_id && play_session_id[0]) {
        add(&b, ",\"PlaySessionId\":\"");
        add(&b, play_session_id);
        add(&b, "\"");
    }
    add(&b, ",\"PositionTicks\":");
    add_u64(&b, position_ticks);
    if (kind != JF_REPORT_STOPPED) {
        add(&b, paused ? ",\"IsPaused\":true" : ",\"IsPaused\":false");
        add(&b, ",\"IsMuted\":false,\"CanSeek\":true,\"PlayMethod\":\"Transcode\"");
    }
    if (kind == JF_REPORT_PROGRESS && event) {
        add(&b, ",\"EventName\":\"");
        add(&b, event);
        add(&b, "\"");
    }
    add(&b, "}");
    return finish(&b);
}

/* Case-insensitive, because the server writes its own query keys in more
 * than one case. */
static int has_param(const char *url, const char *key) {
    size_t      k = strlen(key);
    const char *p;

    for (p = url; *p; p++) {
        size_t i;
        if (p != url && p[-1] != '?' && p[-1] != '&') continue;
        for (i = 0; i < k; i++) {
            char a = p[i], c = key[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (a != c) break;
        }
        if (i == k && p[k] == '=') return 1;
    }
    return 0;
}

int jf_stream_url(char *out, size_t cap, const char *server, const char *transcoding_url,
                  unsigned long long start_ticks) {
    buf         b = {out, cap, 0, 0};
    const char *p;
    size_t      n;

    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (!server || !transcoding_url || transcoding_url[0] != '/') return 0;
    for (p = transcoding_url; *p; p++)
        if ((unsigned char)*p <= 0x20 || (unsigned char)*p > 0x7E) return 0;

    n = strlen(server);
    while (n > 0 && server[n - 1] == '/') n--;
    if (n + 1 >= cap) return 0;
    memcpy(out, server, n);
    out[n] = 0;
    b.len  = n;
    add(&b, transcoding_url);
    if (start_ticks && !has_param(transcoding_url, "StartTimeTicks")) {
        add(&b, strchr(transcoding_url, '?') ? "&StartTimeTicks=" : "?StartTimeTicks=");
        add_u64(&b, start_ticks);
    }
    return finish(&b) != 0;
}

/* ------------------------------------------------------------- config */

int jf_server_is_safe(const char *s) {
    size_t n;

    if (!s || strncmp(s, "http://", 7) != 0 || !s[7]) return 0;
    for (n = 0; s[n]; n++) {
        if (n + 1 >= JF_SERVER_MAX) return 0;
        if ((unsigned char)s[n] <= 0x20 || (unsigned char)s[n] > 0x7E || s[n] == '"') return 0;
    }
    return 1;
}

static int key_is(const char *key, size_t key_len, const char *name) {
    return key_len == strlen(name) && memcmp(key, name, key_len) == 0;
}

static void config_take(const char *key, size_t key_len, const char *val, size_t val_len, jf_config *c) {
    char tmp[JF_SERVER_MAX];

    if (val_len >= sizeof tmp) return;
    memcpy(tmp, val, val_len);
    tmp[val_len] = 0;

    if (key_is(key, key_len, "clock") || key_is(key, key_len, "battery")) {
        int *flag = key_is(key, key_len, "clock") ? &c->show_clock : &c->show_battery;
        if (strcmp(tmp, "on") == 0) *flag = 1;
        else if (strcmp(tmp, "off") == 0) *flag = 0;
        return;
    }
    if (key_is(key, key_len, "server")) {
        if (jf_server_is_safe(tmp)) memcpy(c->server, tmp, val_len + 1);
        return;
    }
    if (val_len >= JF_ID_MAX || !jf_id_is_safe(tmp)) return;
    if (key_is(key, key_len, "device_id")) memcpy(c->device_id, tmp, val_len + 1);
    else if (key_is(key, key_len, "user_id")) memcpy(c->user_id, tmp, val_len + 1);
    else if (key_is(key, key_len, "token")) memcpy(c->token, tmp, val_len + 1);
}

void jf_config_parse(const char *text, size_t len, jf_config *out) {
    size_t i = 0;

    memset(out, 0, sizeof *out);
    out->show_clock   = 1;
    out->show_battery = 1;
    if (!text) return;

    while (i < len) {
        size_t start = i, eq = 0, end;
        int    have_eq = 0;

        while (i < len && text[i] != '\n') {
            if (!have_eq && text[i] == '=') {
                eq      = i;
                have_eq = 1;
            }
            i++;
        }
        end = i;
        if (end > start && text[end - 1] == '\r') end--;
        if (i < len) i++; /* past the \n */

        if (have_eq && eq < end) config_take(text + start, eq - start, text + eq + 1, end - eq - 1, out);
    }
}

size_t jf_config_format(const jf_config *c, char *out, size_t cap) {
    buf b = {out, cap, 0, 0};

    if (!out || cap == 0) return 0;
    out[0] = 0;
    add(&b, "device_id=");
    add(&b, c->device_id);
    add(&b, "\nuser_id=");
    add(&b, c->user_id);
    add(&b, "\ntoken=");
    add(&b, c->token);
    add(&b, "\n");
    /* Only when set, so the file shows a person a key worth adding rather
     * than an empty one that looks like it matters. */
    if (c->server[0]) {
        add(&b, "server=");
        add(&b, c->server);
        add(&b, "\n");
    }
    add(&b, c->show_clock ? "clock=on\n" : "clock=off\n");
    add(&b, c->show_battery ? "battery=on\n" : "battery=off\n");
    return finish(&b);
}
