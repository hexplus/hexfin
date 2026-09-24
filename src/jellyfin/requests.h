/* The text of every request this client sends Jellyfin, and the file it
 * remembers its sign-in in.
 *
 * All of it is text built from values the server handed us -- a token, a
 * user id, a URL -- and text that goes straight into an HTTP header or a
 * request line. So it lives here, where the host build can check it, rather
 * than inside jellyfin/jellyfin.c next to the sockets. No PSP headers, no
 * allocation.
 *
 * AN ID IS CHECKED BEFORE IT IS USED. A token or user id with a quote, a
 * CR or an LF in it would become a second header or a broken JSON body; a
 * Jellyfin id is hex and a token is hex, so anything outside
 * [A-Za-z0-9_-] is refused rather than escaped. */
#ifndef JELLYFIN_REQUESTS_H
#define JELLYFIN_REQUESTS_H

#include <stddef.h>

/* Room for every id this client handles: 32 hex digits for a token or a
 * user id, and a Quick Connect secret, which may be twice that. */
#define JF_ID_MAX 128

#define JF_CLIENT_NAME "Hexfin"
#define JF_DEVICE_NAME "PSP"

/* 1 when `s` is 1..JF_ID_MAX-1 characters of [A-Za-z0-9_-]. */
int jf_id_is_safe(const char *s);

/* The Authorization header line, CRLF included, that Jellyfin 12.1 wants
 * (X-Emby-Authorization answers 400 there, docs/PHASE0_FINDINGS.md section
 * 5). `token` may be NULL or "" before signing in. Returns the length, or 0
 * when an id is unsafe or the line does not fit. */
size_t jf_auth_header(char *out, size_t cap, const char *version, const char *device_id, const char *token);

/* The most a stream's video and audio together may use, in bits a second.
 * The fixture that played on hardware was 470 kbps of video (docs/RESEARCH.md
 * section 10); this leaves the same again of headroom on the radio. */
#define JF_MAX_STREAMING_BITRATE 600000u

/* The body of POST /Items/{id}/PlaybackInfo: the device profile that makes
 * the server transcode to what the Media Engine was proven to decode, and
 * nothing else, starting `start_ticks` into the item. See the comment in
 * requests.c for each condition's reason. Returns the length, or 0 when the
 * user id is unsafe or it does not fit. */
size_t jf_playback_info_body(char *out, size_t cap, const char *user_id, unsigned long long start_ticks);

/* `server` ("http://host:port", with or without a trailing slash) joined to
 * a TranscodingUrl ("/videos/..."), with StartTimeTicks added when
 * `start_ticks` is not 0 and the URL does not already carry it: that is what
 * makes the server's transcode begin there (measured 2026-09-24: its
 * timestamps then start again at 0). Refuses a TranscodingUrl that is not a
 * path, or holds anything but printable ASCII other than a space -- it is
 * about to become a request line. Returns 1 on success. */
int jf_stream_url(char *out, size_t cap, const char *server, const char *transcoding_url,
                  unsigned long long start_ticks);

/* The body of a playback report, which is what makes Continue Watching,
 * the resume position and "watched" work:
 *
 *   JF_REPORT_START    POST /Sessions/Playing           when a stream starts
 *   JF_REPORT_PROGRESS POST /Sessions/Playing/Progress  every few seconds, and on pause/resume
 *   JF_REPORT_STOPPED  POST /Sessions/Playing/Stopped   when it ends, stops, or is left to seek
 *
 * `position_ticks` is where in the item playback is. `paused` goes with
 * start and progress; `event` ("timeupdate", "pause", "unpause") with
 * progress only, NULL for none. Returns the length, or 0 when an id is
 * unsafe or it does not fit. */
typedef enum { JF_REPORT_START, JF_REPORT_PROGRESS, JF_REPORT_STOPPED } jf_report_kind;

size_t jf_report_body(char *out, size_t cap, jf_report_kind kind, const char *item_id, const char *media_source_id,
                      const char *play_session_id, unsigned long long position_ticks, int paused, const char *event);

#define JF_SERVER_MAX 128

/* What this client keeps between runs, in jellyfin.cfg next to the EBOOT:
 * the sign-in, so Quick Connect is needed once, not every time, and the
 * settings a person can change. */
typedef struct {
    char device_id[JF_ID_MAX];
    char user_id[JF_ID_MAX];
    char token[JF_ID_MAX];
    char server[JF_SERVER_MAX]; /* "" means the built-in default */
    int  show_clock;            /* the time of day in the corner; on unless turned off */
    int  show_battery;          /* the battery in the corner; on unless turned off */
} jf_config;

/* 1 when `s` is an http:// address this client can use as its server:
 * printable, no spaces, no quotes, and short enough to keep. */
int jf_server_is_safe(const char *s);

/* Reads "key=value" lines. Unknown keys are ignored; a known key whose
 * value is not safe for it is dropped, so a damaged file costs a new
 * sign-in or a default setting and nothing worse. Everything absent comes
 * back "" or 0. */
void jf_config_parse(const char *text, size_t len, jf_config *out);

/* Writes it back in the same form. Returns the length, or 0 if it does not
 * fit. */
size_t jf_config_format(const jf_config *c, char *out, size_t cap);

#endif /* JELLYFIN_REQUESTS_H */
