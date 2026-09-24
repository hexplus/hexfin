/* Every byte of HTTP this project reads, parsed where a host can fuzz it.
 *
 * net/http.c owns the socket; this file owns the bytes that come off it, and
 * they are split apart for exactly the reason media/h264_au.c is split out of
 * media/video_psp.c: a response arrives from a server we do not control
 * (PROMPT.md section 35), the defect class that matters is a one-byte
 * over-read, and the only place that defect can be caught cheaply is a host
 * build with AddressSanitizer. Nothing in here includes a PSP header, calls
 * a socket, or allocates.
 *
 * WHAT IS REFUSED RATHER THAN HANDLED, and why, because each one is a
 * decision and not an omission:
 *
 *   - HTTPS. PROMPT.md section 16 puts it outside the MVP, and a URL that
 *     asks for it is refused by name rather than quietly downgraded to
 *     cleartext -- a client that silently drops the "s" is worse than one
 *     that says it cannot.
 *   - Userinfo in the authority (`http://user@host/`). It makes the host
 *     ambiguous to anything that parses left to right, which is the whole
 *     mechanism behind a URL that reads as one server to a person and
 *     another to a program.
 *   - Obsolete line folding (a header continued on an indented next line).
 *     RFC 7230 deprecated it; every server this project will meet emits one
 *     header per line, and accepting folds means a header value that can be
 *     split to hide from a scan of it.
 *   - Content-Length together with Transfer-Encoding, and two
 *     Content-Length headers that disagree. Both are the classic shapes of a
 *     response whose length two parsers read differently.
 *   - A Transfer-Encoding that is anything but `chunked`. There is no
 *     decompressor here, so `gzip` would be handed to the fMP4 reader as if
 *     it were video.
 *
 * WHAT IS HANDLED: chunked framing, because Jellyfin must use it. The
 * transcode is a live FFmpeg pipe of unknown length (docs/PHASE0_FINDINGS.md
 * section 7), so there is no Content-Length to send, and an HTTP/1.1 response
 * of unknown length is chunked -- refusing it would mean refusing the only
 * response this project exists to read. */
#ifndef NET_HTTP_PARSE_H
#define NET_HTTP_PARSE_H

#include <stdint.h>

/* A URL long enough for a Jellyfin TranscodingUrl, which carries the whole
 * negotiated profile as query parameters. It ran to roughly 400 characters
 * in docs/PHASE0_FINDINGS.md section 7 without the session token, the play
 * session id, the media source id and the subtitle and stream-index
 * parameters a real PlaybackInfo answer adds; 1536 is room for all of those
 * with margin, and a URL that is still longer is refused rather than cut. */
#define HTTP_HOST_MAX 128
#define HTTP_PATH_MAX 1536

/* The cap on a whole response header block.
 *
 * A peer that sends header bytes forever must be refused rather than
 * followed (PROMPT.md section 35, "oversized response attacks"), and on a
 * console with a 24 MB partition "refused" has to mean a fixed buffer, not a
 * growing one. 4096 is the conventional server-side limit -- it is what
 * nginx and Apache bound a request's headers to -- and it is roughly ten
 * times what Jellyfin's transcode response actually sends. A block larger
 * than this is not a response this client has any use for. */
#define HTTP_HEADER_MAX 4096

/* A Location is a URL, so it gets the URL budget rather than a smaller one
 * of its own: a redirect truncated into a different URL would be reported to
 * a person as somewhere the server never pointed. */
#define HTTP_LOCATION_MAX 512

/* A chunk size line is at most 16 hex digits; the rest of this is room for a
 * chunk extension, which nothing here reads but which a conformant server is
 * allowed to send. A line longer than this is refused rather than skipped --
 * an unbounded line is the same "send bytes forever" attack the header cap
 * exists to stop, one layer down. */
#define HTTP_CHUNK_LINE_MAX 32

/* A ceiling on one chunk's declared size.
 *
 * NOT a buffer size -- a chunk is streamed through, never held, so this does
 * not have to fit anywhere. It is here so `remaining` is a number with some
 * relationship to reality, and so the hex accumulator has something to
 * overflow-check against. 16 MB is far past any chunk a transcode emits
 * (Jellyfin's are kilobytes) and comfortably below the 2^64-1 a malicious
 * one would claim. */
#define HTTP_CHUNK_MAX (16u * 1024u * 1024u)

typedef enum {
    HTTP_PARSE_OK = 0,
    HTTP_PARSE_AGAIN,     /* not an error: more bytes are needed to decide */
    HTTP_PARSE_ARG,       /* the caller's arguments cannot be right */
    HTTP_PARSE_MALFORMED, /* the bytes are not HTTP, or are HTTP this client refuses */
    HTTP_PARSE_TOOBIG     /* a bound was reached: the header cap, a length, a line */
} http_parse_err;

/* ---- the URL --------------------------------------------------------- */

typedef struct {
    char     host[HTTP_HOST_MAX]; /* no brackets, no userinfo, no port */
    char     path[HTTP_PATH_MAX]; /* always begins with '/', query included */
    uint16_t port;                /* 80 when the URL gave none */
} http_url;

/* Splits `url` into the three pieces a socket needs. Refuses anything whose
 * scheme is not `http`, anything with userinfo, an empty host, a port
 * outside 1..65535, and a host or path that would not fit above. */
http_parse_err http_url_parse(const char *url, http_url *out);

/* Recognises a dotted-quad address and returns it in host byte order.
 *
 * Deliberately strict -- four parts, one to three digits each, nothing else
 * -- because the permissive forms the C library accepts (`0x7f.1`, `2130706433`,
 * a trailing dot) are how one string is read as two different addresses by
 * two different resolvers. Returns 1 when `host` is an address and 0 when it
 * is a name to be resolved; `out` is only written on 1. */
int http_ipv4_parse(const char *host, uint32_t *out_host_order);

/* ---- the response header block --------------------------------------- */

/* Accumulates header bytes across however many reads the socket happens to
 * split them into, which is the whole reason this is a struct and not a
 * function over one buffer: a read boundary falls wherever the network puts
 * it, including between the CR and the LF that end the block. */
typedef struct {
    uint8_t  buf[HTTP_HEADER_MAX];
    uint32_t len;
    int      complete;
} http_headers;

void http_headers_init(http_headers *h);

/* Takes bytes from `in` until the CRLFCRLF that ends the block, and reports
 * through `in_used` how many it took -- everything after that is body and
 * belongs to the caller.
 *
 * Returns HTTP_PARSE_OK once the block is complete, HTTP_PARSE_AGAIN when
 * more bytes are needed, HTTP_PARSE_TOOBIG when HTTP_HEADER_MAX was reached
 * without a terminator.
 *
 * Only CRLF is accepted, never a bare LF. Servers send CRLF; accepting both
 * means two parsers can disagree about where a header ends, which is the
 * entire mechanism of response splitting. */
http_parse_err http_headers_feed(http_headers *h, const uint8_t *in, uint32_t in_len, uint32_t *in_used);

typedef struct {
    int      status;
    int      chunked;
    int      have_content_length;
    uint64_t content_length;
    char     location[HTTP_LOCATION_MAX]; /* "" when the response carried none */
} http_response;

/* Reads the status line and the handful of headers this client acts on out
 * of a completed block. Everything else in the block is ignored, not
 * refused: a server is allowed its own headers. */
http_parse_err http_response_parse(const http_headers *h, http_response *out);

/* ---- chunked framing -------------------------------------------------- */

typedef struct {
    int      state;
    uint64_t remaining;                 /* body bytes left in the current chunk */
    uint32_t line_len;
    char     line[HTTP_CHUNK_LINE_MAX];
} http_chunked;

void http_chunked_init(http_chunked *c);

/* Pulls body bytes out of a chunked stream: consumes up to `in_len` bytes of
 * wire data and writes up to `out_cap` bytes of body, reporting both. Either
 * side may stop first, and stopping is not an error -- the caller loops.
 *
 * `out_done` is set once the terminating zero-length chunk has been seen.
 * Nothing after it is consumed: a trailer is a thing this client has no use
 * for, the socket closes immediately afterwards, and a trailer parser would
 * be state that nothing could ever exercise.
 *
 * Returns HTTP_PARSE_OK when it made what progress it could, or
 * HTTP_PARSE_MALFORMED / HTTP_PARSE_TOOBIG for a stream it refuses. */
http_parse_err http_chunked_pull(http_chunked *c, const uint8_t *in, uint32_t in_len, uint32_t *in_used, uint8_t *out,
                                 uint32_t out_cap, uint32_t *out_used, int *out_done);

#endif /* NET_HTTP_PARSE_H */
