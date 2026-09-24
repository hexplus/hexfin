/* A streaming HTTP GET, as a media source.
 *
 * WHY RAW SOCKETS AND NOT sceHttp: the firmware has its own client, and it
 * was evaluated first as PROMPT.md section 15 asks. The reasoning, the
 * measurements behind it and what sceHttp would have given us is written up
 * in docs/RESEARCH.md section 9, because it is the kind of decision somebody
 * will reopen in six months and a one-line comment would not survive the
 * argument.
 *
 * The response body is a film. It is read incrementally into whatever buffer
 * the caller offers and never sized, never buffered whole, never written to
 * the memory stick (PROMPT.md Rule 3, section 24, section 30). There is no
 * call here that answers "how big is it", on purpose: Jellyfin's transcode is
 * a live FFmpeg pipe and sends no Content-Length at all
 * (docs/PHASE0_FINDINGS.md section 7), so a caller written against such an
 * answer would work against a file and fail against the server.
 *
 * Only compiled for the console. The parsing this does is in
 * net/http_parse.c, which is not, and that is where the checks live. */
#ifndef NET_HTTP_H
#define NET_HTTP_H

#include "media/source.h"

#include <stdint.h>

/* The most header text a caller may add to a request: room for Jellyfin's
 * Authorization header, which names the client, the device and a token. */
#define HTTP_EXTRA_HEADERS_MAX 512u

/* One HTTP GET, streamed. `location` is an absolute http:// URL. */
extern const media_source source_http;

/* Header lines added to every request source_http.open() makes from now on
 * -- Jellyfin's Authorization, so a stream URL does not have to carry the
 * token itself. Each line ends in CRLF; NULL or "" clears them. Copied. */
void http_set_stream_headers(const char *headers);

/* One small request whose whole answer is wanted: the Jellyfin API calls
 * made before playback. */
typedef struct {
    const char *method;       /* "GET", "POST", "DELETE" */
    const char *url;          /* absolute http:// */
    const char *headers;      /* extra lines, each ending in CRLF, or NULL */
    const char *body;         /* NULL for none */
    uint32_t    body_len;
    const char *content_type; /* for the body; NULL means application/json */
} http_request;

/* Sends `r` and reads the whole answer into `out`, NUL-terminated, on its
 * own connection, which is closed again before this returns. Fails rather
 * than truncates when the answer does not fit in cap - 1 bytes. Any 2xx is
 * success; anything else fails with the code in http_status() -- 401 is how
 * an expired token looks. The reason is in http_error().
 *
 * Shares its state with source_http: never call it while a stream is open. */
int http_fetch(const http_request *r, char *out, uint32_t cap, uint32_t *out_len);

/* The last response's status code, or 0 if none arrived. */
int http_status(void);

/* The reason for the last failure of either http_fetch or source_http. */
const char *http_error(void);

/* Interrupts a read that is ALREADY IN PROGRESS, and makes every later one
 * fail immediately.
 *
 * Design section 3.4 is explicit that a main thread sitting in an HTTP read
 * cannot be allowed to notice HOME only when the read returns, so this does
 * two things rather than one: it sets a flag, and it shuts the socket down,
 * which is what actually breaks a recv that is already blocked. Safe to call
 * from the exit callback -- that runs on an ordinary user thread (the one
 * parked in sceKernelSleepThreadCB, see platform/psp_platform.c), not in an
 * interrupt context, so a syscall there is legal. Safe to call more than
 * once, and safe before any connection exists. */
void http_cancel(void);

#endif /* NET_HTTP_H */
