/* See net/http.h. The socket half; net/http_parse.c is the byte half.
 *
 * Only compiled for the console. scripts/test.sh compiles every .c under
 * src/net, and there are no sockets there -- the guard is the one
 * media/video_psp.c uses, for the same reason. */
#if defined(__PSP__)

#include "net/http.h"

#include "net/http_parse.h"
#include "platform/psp_platform.h"

#include <netinet/in.h>
#include <pspkernel.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <string.h>
#include <sys/socket.h>

/* ---------------------------------------------------------------- limits */

/* One socket read. At the bitrate this project negotiated -- 470 kbps video
 * and 128 kbps audio, docs/PHASE0_FINDINGS.md section 7 -- 16 kB is about a
 * fifth of a second of stream: large enough that the per-read cost
 * disappears, small enough that it is noise beside the probe's 384 kB ring.
 * It is a staging buffer, not a play-out buffer; PROMPT.md section 24's
 * 256-512 kB figure is the ring's budget and this does not spend from it. */
#define HTTP_RAW_CAP 16384u

/* A hand-built request line plus the fixed headers below and the caller's
 * own (HTTP_EXTRA_HEADERS_MAX, net/http.h), against the URL budgets in
 * net/http_parse.h. Sized against those rather than guessed; a request that
 * still does not fit is refused by build_request, never sent cut short. */
#define HTTP_REQ_MAX (HTTP_PATH_MAX + HTTP_HOST_MAX + HTTP_EXTRA_HEADERS_MAX + 256u)

#define HTTP_ERRLEN 256

/* Every blocking call is bounded, and bounded in SLICES.
 *
 * The slice is what makes cancellation work without a second thread: a poll
 * that waits 100 ms at a time returns to the loop ten times a second, and the
 * loop looks at the cancel flag each time (PROMPT.md section 29 -- never hard-
 * lock the console because a socket is blocked). http_cancel()'s shutdown is
 * the faster path; this is the one that works even if the shutdown does not.
 *
 * The totals: 8 s to establish a TCP connection on a LAN is already
 * generous -- a server that is there answers in milliseconds and one that is
 * not answers with a reset. 10 s of complete silence mid-stream is a stall,
 * not a slow link: at 600 kbps the server is producing this stream faster
 * than real time, so ten seconds of nothing means the transcode died. */
#define HTTP_POLL_SLICE_MS 100
#define HTTP_CONNECT_TIMEOUT_MS 8000
#define HTTP_SEND_TIMEOUT_MS 8000
#define HTTP_HEADER_TIMEOUT_MS 10000
#define HTTP_RECV_TIMEOUT_MS 10000

/* Name resolution gets its own budget: 2 s a try, three retries -- the
 * figures the SDK's own samples use. One retry of 3 s failed on hardware
 * (2026-09-23), before the error said why; a first query lost while the
 * gateway's address is still being learned is one explanation, and more
 * tries cost nothing when the server does answer. */
#define HTTP_RESOLVE_TIMEOUT_S 2
#define HTTP_RESOLVE_RETRIES 3

/* The resolver wants scratch space of its own. The SDK documents no required
 * size; 1 kB is what leaves room for one answer and is a fixed static like
 * everything else here (PROMPT.md section 2). */
#define HTTP_RESOLVER_BUF 1024u

/* ----------------------------------------------------------------- state */

/* Everything one connection needs. Two exist: the stream, read by the
 * player's reader thread, and the API calls -- sign-in, lists, and the
 * progress reports sent while a stream is playing. They used to share one
 * set of statics, when nothing talked to the server during playback; a
 * progress report now does, from its own thread, on its own connection. */
typedef struct {
    int sock;

    int eof;    /* the body is over: peer closed, or the last chunk arrived */
    int ready;  /* the headers were accepted and the body may be read */
    int status; /* the last response's status code, 0 before one arrives */

    http_headers headers;
    http_chunked chunk;
    int          is_chunked;

    uint8_t  raw[HTTP_RAW_CAP];
    uint32_t raw_len;
    uint32_t raw_pos;

    char req[HTTP_REQ_MAX];
    int  req_overflow;
    char resolver_buf[HTTP_RESOLVER_BUF];

    /* Static rather than on the stack: at HTTP_PATH_MAX it is over 1.6 kB. */
    http_url url;

    char     err[HTTP_ERRLEN];
    uint32_t err_len;
} conn;

/* Zero-filled, not initialised: an initialiser would put both structs --
 * their 16 kB read buffers included -- into the EBOOT file itself. The one
 * field that must not start at 0, the socket, is set by conns_init() before
 * anything looks at it. */
static conn g_stream;
static conn g_api;
static int  g_conns_ready;

static void conns_init(void) {
    if (g_conns_ready) return;
    g_stream.sock = -1;
    g_api.sock    = -1;
    g_conns_ready = 1;
}

/* Volatile because http_cancel() writes it from the exit callback's thread
 * while the read loops are reading it. One for both connections: HOME
 * stops everything. */
static volatile int g_cancel;

/* Added to every request source_http.open() makes. A copy, so the caller's
 * buffer can go away. */
static char g_stream_headers[HTTP_EXTRA_HEADERS_MAX];

/* ---------------------------------------------------------------- reasons
 *
 * Built by appending rather than with snprintf, for the reason
 * media/video_psp.c gives at length: newlib's printf is a large dependency
 * to pull into a module that needs to print a number and a sentence. */

static void err_reset(conn *c) {
    c->err_len = 0;
    c->err[0]  = 0;
}

static void err_add(conn *c, const char *s) {
    while (*s && c->err_len + 1 < HTTP_ERRLEN) c->err[c->err_len++] = *s++;
    c->err[c->err_len] = 0;
}

static void err_add_hex(conn *c, uint32_t v) {
    static const char hex[] = "0123456789ABCDEF";
    int               i;
    for (i = 28; i >= 0 && c->err_len + 1 < HTTP_ERRLEN; i -= 4) c->err[c->err_len++] = hex[(v >> i) & 0xFu];
    c->err[c->err_len] = 0;
}

static void err_add_dec(conn *c, long v) {
    char     tmp[24];
    unsigned n = 0;
    unsigned long u;

    if (v < 0) {
        if (c->err_len + 1 < HTTP_ERRLEN) c->err[c->err_len++] = '-';
        u = (unsigned long)(-v);
    } else {
        u = (unsigned long)v;
    }
    do {
        tmp[n++] = (char)('0' + (u % 10u));
        u /= 10u;
    } while (u > 0 && n < sizeof tmp);
    while (n > 0 && c->err_len + 1 < HTTP_ERRLEN) c->err[c->err_len++] = tmp[--n];
    c->err[c->err_len] = 0;
}

/* The network stack's own errno, appended to whatever went wrong.
 *
 * Reported as a number rather than branched on: PSPSDK declares
 * sceNetInetGetErrno but not the values it returns, and a client that guessed
 * at them would be guessing about the one thing it is supposed to be
 * reporting accurately. The number is still the difference between "the
 * server is not there" and "the server hung up", and PROMPT.md section 45
 * wants that on the screen rather than in somebody's head.
 *
 * It also earns its place in the LINK, which is not a joke: newlib's socket
 * glue (libcglue) references this same import, and because that archive is
 * scanned after ours, its reference used to drag a twelfth sceNetInet stub in
 * on a second pass and land it a hundred bytes away from the other eleven.
 * psp-fixup-imports refuses a library whose stubs are not contiguous -- the
 * exact "stubs out of order" hazard docs/RESEARCH.md section 1b describes --
 * and calling it here is what pulls that stub in with its siblings. */
static void err_add_socket_errno(conn *c) {
    err_add(c, " (socket error ");
    err_add_dec(c, (long)sceNetInetGetErrno());
    err_add(c, ")");
}

/* ------------------------------------------------------------ the socket */

static void sock_close(conn *c) {
    if (c->sock >= 0) {
        int s = c->sock;
        c->sock = -1; /* cleared FIRST, so a concurrent cancel cannot shut down a reused descriptor */
        sceNetInetClose(s);
    }
}

/* Waits for `events` on the socket, in slices, and gives up either when the
 * total is spent or when a stop was requested.
 *
 * Returns 1 ready, 0 timed out, -1 cancelled, -2 the poll itself failed. */
static int wait_for(conn *c, int events, int total_ms) {
    /* Measured on the clock, not by adding up slices: a poll that returns
     * early -- the emulator's returns at once when nothing is ready -- would
     * otherwise spend a 10 s budget in a tenth of a second, and a stream
     * whose transcode takes two seconds to start would time out
     * (2026-09-24, in the emulator). */
    unsigned long long started = platform_now_us();
    int                waited  = 0;

    while (waited < total_ms) {
        struct SceNetInetPollfd pfd;
        int                     rc;
        int                     slice = total_ms - waited;

        if (g_cancel) return -1;
        if (slice > HTTP_POLL_SLICE_MS) slice = HTTP_POLL_SLICE_MS;

        pfd.fd      = c->sock;
        pfd.events  = (short)events;
        pfd.revents = 0;

        rc = sceNetInetPoll(&pfd, 1, slice);
        if (rc < 0) return -2;
        if (rc > 0) {
            /* HUP and ERR come back whether or not they were asked for, and
             * both mean "stop waiting" -- the recv or the SO_ERROR check
             * that follows is what turns them into a reason. */
            if (pfd.revents != 0) return 1;
        }
        /* A poll that came back early with nothing is given a moment
         * rather than spun on. */
        if ((unsigned long long)(platform_now_us() - started) < (unsigned long long)(waited + slice) * 1000ull)
            sceKernelDelayThread(10 * 1000);
        waited = (int)((platform_now_us() - started) / 1000ull);
    }
    return 0;
}

/* Turns the four wait_for outcomes into one reason, so every caller reports
 * the same failure the same way. */
static int wait_failed(conn *c, int rc, const char *what) {
    if (rc == 1) return 0;
    err_reset(c);
    if (rc == -1) {
        err_add(c, "the connection was stopped while ");
        err_add(c, what);
        return SOURCE_ERR_CANCELLED;
    }
    if (rc == -2) {
        err_add(c, "the network stack refused to wait on the socket while ");
        err_add(c, what);
        return SOURCE_ERR_IO;
    }
    err_add(c, "timed out while ");
    err_add(c, what);
    return SOURCE_ERR_IO;
}

/* ------------------------------------------------------------- resolving */

static int resolve(conn *c, const char *host, uint32_t *out_host_order) {
    int            rid = -1;
    int            rc;
    struct in_addr addr;

    if (http_ipv4_parse(host, out_host_order)) return SOURCE_OK;

    rc = sceNetResolverCreate(&rid, c->resolver_buf, (SceSize)sizeof c->resolver_buf);
    if (rc < 0) {
        err_reset(c);
        err_add(c, "no name resolver could be created, so \"");
        err_add(c, host);
        err_add(c, "\" could not be looked up");
        return SOURCE_ERR_OPEN;
    }

    addr.s_addr = 0;
    rc          = sceNetResolverStartNtoA(rid, host, &addr, HTTP_RESOLVE_TIMEOUT_S, HTTP_RESOLVE_RETRIES);
    sceNetResolverDelete(rid);

    if (rc < 0) {
        err_reset(c);
        err_add(c, "the name \"");
        err_add(c, host);
        err_add(c, "\" could not be resolved (firmware returned 0x");
        err_add_hex(c, (uint32_t)rc);
        err_add(c, ")");
        return SOURCE_ERR_OPEN;
    }

    /* s_addr is network byte order; every caller below wants host order, so
     * the conversion happens once, here. */
    *out_host_order = ((uint32_t)ntohl(addr.s_addr));
    return SOURCE_OK;
}

/* --------------------------------------------------------- the request */

static uint32_t req_add(conn *c, uint32_t n, const char *s) {
    while (*s && n + 1 < HTTP_REQ_MAX) c->req[n++] = *s++;
    if (*s) c->req_overflow = 1;
    return n;
}

static uint32_t req_add_dec(conn *c, uint32_t n, unsigned v) {
    char     tmp[12];
    unsigned k = 0;

    do {
        tmp[k++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v > 0 && k < sizeof tmp);
    while (k > 0 && n + 1 < HTTP_REQ_MAX) c->req[n++] = tmp[--k];
    if (k > 0) c->req_overflow = 1;
    return n;
}

/* Connection: close, deliberately.
 *
 * This is PROMPT.md section 15's STREAM mode: one request, one film, then the
 * socket is finished with. Keep-alive would only matter to the API mode that
 * does not exist yet, and asking for it here would leave a socket half-alive
 * through a teardown that design section 3.4 wants to be the one thing that
 * cannot break. It also lets the server choose to delimit the body by closing
 * the connection instead of chunking it, which is one fewer thing that has to
 * go right. */
/* The same holds for the Jellyfin API calls (http_fetch): each is a single
 * small request made before playback, never during it, so a connection per
 * call costs a round trip nobody is waiting on.
 *
 * Returns the request's length, or 0 when it does not fit in c->req. */
static uint32_t build_request(conn *c, const http_request *r, const http_url *u) {
    uint32_t n = 0;

    c->req_overflow = 0;
    n = req_add(c, n, r->method);
    n = req_add(c, n, " ");
    n = req_add(c, n, u->path);
    n = req_add(c, n, " HTTP/1.1\r\nHost: ");
    n = req_add(c, n, u->host);
    if (u->port != 80) {
        n = req_add(c, n, ":");
        n = req_add_dec(c, n, u->port);
    }
    n = req_add(c, n, "\r\nUser-Agent: Hexfin\r\nAccept: */*\r\nConnection: close\r\n");
    if (r->headers) n = req_add(c, n, r->headers);
    if (r->body) {
        n = req_add(c, n, "Content-Type: ");
        n = req_add(c, n, r->content_type ? r->content_type : "application/json");
        n = req_add(c, n, "\r\n");
    }
    /* A POST with no body says so: without a length, a server may wait for
     * a body that is never coming. */
    if (r->body || strcmp(r->method, "GET") != 0) {
        n = req_add(c, n, "Content-Length: ");
        n = req_add_dec(c, n, r->body ? (unsigned)r->body_len : 0u);
        n = req_add(c, n, "\r\n");
    }
    n = req_add(c, n, "\r\n");
    return c->req_overflow ? 0 : n;
}

/* Header lines come from this program, not from a server -- but a value
 * copied into one from a server's answer (a token, an id) could carry a CR
 * or LF, and with it a header of its own. So every line must be printable
 * ASCII and end in CRLF, and nothing else gets sent. */
static int headers_are_clean(const char *h) {
    const char *p;

    if (!h || !h[0]) return 1;
    for (p = h; *p; p++) {
        if (*p == '\r') {
            if (p[1] != '\n') return 0;
            p++;
            continue;
        }
        if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E) return 0;
    }
    return p - h >= 2 && p[-2] == '\r' && p[-1] == '\n';
}

static int send_all(conn *c, const char *data, uint32_t len) {
    uint32_t sent = 0;

    while (sent < len) {
        int n;
        int rc = wait_for(c, SCE_NET_INET_POLLOUT, HTTP_SEND_TIMEOUT_MS);
        if (rc != 1) return wait_failed(c, rc, "sending the request");

        /* Cast, because PSPSDK declares sceNetInetSend as returning size_t.
         * A failure comes back as -1 widened to an enormous unsigned value,
         * and a caller that compares it against 0 unsigned sees a gigantic
         * successful send. */
        n = (int)sceNetInetSend(c->sock, data + sent, (size_t)(len - sent), 0);
        if (n <= 0) {
            err_reset(c);
            err_add(c, "the request could not be sent -- the server closed the connection");
            err_add_socket_errno(c);
            return SOURCE_ERR_IO;
        }
        sent += (uint32_t)n;
    }
    return SOURCE_OK;
}

/* Reads once into the staging buffer. Returns SOURCE_OK with bytes waiting,
 * SOURCE_ERR_EOF when the peer closed, or a failure. */
static int fill_raw(conn *c, int timeout_ms) {
    int rc;
    int n;

    c->raw_pos = 0;
    c->raw_len = 0;

    rc = wait_for(c, SCE_NET_INET_POLLIN, timeout_ms);
    if (rc != 1) return wait_failed(c, rc, "waiting for the server to send");

    n = (int)sceNetInetRecv(c->sock, c->raw, (size_t)HTTP_RAW_CAP, 0);
    if (n == 0) return SOURCE_ERR_EOF;
    if (n < 0) {
        /* A cancel shuts the socket down, and the read in progress fails
         * here rather than at the poll -- which is the whole point of the
         * shutdown, so it is reported as a stop and not as a fault. */
        if (g_cancel) {
            err_reset(c);
            err_add(c, "the connection was stopped during a read");
            return SOURCE_ERR_CANCELLED;
        }
        err_reset(c);
        err_add(c, "the connection failed during a read");
        err_add_socket_errno(c);
        return SOURCE_ERR_IO;
    }

    c->raw_len = (uint32_t)n;
    return SOURCE_OK;
}

/* ------------------------------------------------------------- open/read */

static int read_headers(conn *c) {
    http_response resp;
    unsigned long long started = platform_now_us();

    c->status = 0;
    http_headers_init(&c->headers);

    for (;;) {
        http_parse_err pe;
        uint32_t       used = 0;
        int            rc;

        /* The whole header block gets ONE budget, not one per read: a peer
         * that trickles a byte every nine seconds would otherwise hold the
         * probe open forever while never once timing out. */
        if ((unsigned long long)(platform_now_us() - started) > (unsigned long long)HTTP_HEADER_TIMEOUT_MS * 1000ull) {
            err_reset(c);
            err_add(c, "the server did not finish its response headers in time");
            return SOURCE_ERR_IO;
        }

        rc = fill_raw(c, HTTP_HEADER_TIMEOUT_MS);
        if (rc == SOURCE_ERR_EOF) {
            err_reset(c);
            err_add(c, "the server closed the connection before sending a complete response header");
            return SOURCE_ERR_OPEN;
        }
        if (rc != SOURCE_OK) return rc;

        pe = http_headers_feed(&c->headers, c->raw, c->raw_len, &used);
        if (pe == HTTP_PARSE_TOOBIG) {
            err_reset(c);
            err_add(c, "the server's response headers ran past ");
            err_add_dec(c, (long)HTTP_HEADER_MAX);
            err_add(c, " bytes and were refused");
            return SOURCE_ERR_OPEN;
        }
        if (pe == HTTP_PARSE_OK) {
            /* Whatever came in the same packet after the blank line is
             * already body, and is kept rather than re-read -- there is no
             * way to put it back on the socket. */
            c->raw_pos = used;
            break;
        }
        if (pe != HTTP_PARSE_AGAIN) {
            err_reset(c);
            err_add(c, "the server's response headers could not be parsed");
            return SOURCE_ERR_OPEN;
        }
    }

    if (http_response_parse(&c->headers, &resp) != HTTP_PARSE_OK) {
        err_reset(c);
        err_add(c, "the server's response is not HTTP this client will accept");
        return SOURCE_ERR_OPEN;
    }
    c->status = resp.status;

    /* Redirects are NOT followed, and this is a decision rather than an
     * omission.
     *
     * Jellyfin hands out a TranscodingUrl that is already final
     * (docs/PHASE0_FINDINGS.md section 7) -- no redirect has been observed
     * from this server on this path. Following one means resolving a second
     * name, opening a second connection and re-applying every bound in this
     * file to a destination the response chose rather than the operator, and
     * PROMPT.md section 35 lists redirects among the things to validate for
     * exactly that reason. The bound that costs nothing to get right is
     * zero. The Location is reported instead, so a person can point the
     * probe at it directly and see what happens; if a later phase needs to
     * follow one, the limit belongs here as a small count, not as a loop. */
    if (resp.status >= 300 && resp.status <= 399) {
        err_reset(c);
        err_add(c, "the server redirected (");
        err_add_dec(c, resp.status);
        err_add(c, ") to ");
        err_add(c, resp.location[0] ? resp.location : "somewhere it did not name");
        err_add(c, " -- this client does not follow redirects");
        return SOURCE_ERR_OPEN;
    }
    /* Any 2xx, not only 200: the Jellyfin calls that only acknowledge
     * answer 204. */
    if (resp.status < 200 || resp.status > 299) {
        err_reset(c);
        err_add(c, "the server answered ");
        err_add_dec(c, resp.status);
        err_add(c, resp.status == 401 ? " -- not signed in, or the sign-in has expired" : ", not 200");
        return SOURCE_ERR_OPEN;
    }

    c->is_chunked = resp.chunked;
    if (c->is_chunked) http_chunked_init(&c->chunk);
    return SOURCE_OK;
}

static int open_request(conn *c, const http_request *r) {
    http_url *u    = &c->url;
    uint32_t  addr = 0;
    int       rc;
    int       one = 1;

    err_reset(c);
    c->eof        = 0;
    c->ready      = 0;
    c->status     = 0;
    c->raw_len    = 0;
    c->raw_pos    = 0;
    c->is_chunked = 0;

    /* A stop, once requested, stays requested. The flag used to be cleared
     * here, when there was only ever one request; now that API calls come
     * before the stream, HOME during one of them has to stop the ones after
     * it as well. */
    if (g_cancel) {
        err_add(c, "the connection was stopped before it started");
        return SOURCE_ERR_CANCELLED;
    }
    if (!r || !r->url || !r->method) {
        err_add(c, "no URL was given to the HTTP source");
        return SOURCE_ERR_ARG;
    }
    if (!headers_are_clean(r->headers)) {
        err_add(c, "a request header was malformed, so the request was not sent");
        return SOURCE_ERR_ARG;
    }
    if (c->sock >= 0) {
        err_add(c, "the HTTP source was opened twice without a close in between");
        return SOURCE_ERR_STATE;
    }

    switch (http_url_parse(r->url, u)) {
        case HTTP_PARSE_OK: break;
        case HTTP_PARSE_TOOBIG:
            err_add(c, "the URL is longer than this client will hold");
            return SOURCE_ERR_ARG;
        default:
            err_add(c, "the URL is not an http:// address this client can use");
            return SOURCE_ERR_ARG;
    }

    rc = resolve(c, u->host, &addr);
    if (rc != SOURCE_OK) return rc;

    c->sock = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);
    if (c->sock < 0) {
        err_reset(c);
        err_add(c, "no socket could be created -- is Wi-Fi up?");
        return SOURCE_ERR_OPEN;
    }

    /* Non-blocking from here on, so that every wait below is a poll with a
     * timeout this file chose rather than a blocking call with whatever
     * timeout the TCP stack happens to use. A blocking connect to a host
     * that simply does not answer takes over a minute, and design section
     * 3.4 will not have a main thread parked that long. */
    if (sceNetInetSetsockopt(c->sock, SOL_SOCKET, SO_NONBLOCK, &one, (socklen_t)sizeof one) < 0) {
        err_reset(c);
        err_add(c, "the socket could not be put in non-blocking mode");
        sock_close(c);
        return SOURCE_ERR_OPEN;
    }

    {
        struct sockaddr_in sa;

        memset(&sa, 0, sizeof sa);
        sa.sin_family      = AF_INET;
        sa.sin_port        = htons(u->port);
        sa.sin_addr.s_addr = htonl(addr);

        if (sceNetInetConnect(c->sock, (struct sockaddr *)&sa, (socklen_t)sizeof sa) < 0) {
            /* Expected on a non-blocking socket: the connect is under way,
             * not refused. The poll below is what decides which it was, so
             * the errno is never consulted -- see the comment there. */
            int wrc = wait_for(c, SCE_NET_INET_POLLOUT, HTTP_CONNECT_TIMEOUT_MS);
            if (wrc != 1) {
                rc = wait_failed(c, wrc, "connecting to the server");
                sock_close(c);
                return rc == SOURCE_ERR_CANCELLED ? rc : SOURCE_ERR_OPEN;
            }

            /* Whether the connect succeeded is in SO_ERROR, not in the poll:
             * a refused connection also makes the socket writable. */
            {
                int       so_err = 0;
                socklen_t len    = (socklen_t)sizeof so_err;
                if (sceNetInetGetsockopt(c->sock, SOL_SOCKET, SO_ERROR, &so_err, &len) == 0 && so_err != 0) {
                    err_reset(c);
                    err_add(c, "the server refused the connection (socket error ");
                    err_add_dec(c, so_err);
                    err_add(c, ")");
                    sock_close(c);
                    return SOURCE_ERR_OPEN;
                }
            }
        }
    }

    {
        uint32_t len = build_request(c, r, u);
        if (len == 0) {
            err_reset(c);
            err_add(c, "the request is longer than this client will send");
            sock_close(c);
            return SOURCE_ERR_ARG;
        }
        rc = send_all(c, c->req, len);
        if (rc == SOURCE_OK && r->body && r->body_len > 0) rc = send_all(c, r->body, r->body_len);
        if (rc != SOURCE_OK) {
            sock_close(c);
            return rc;
        }
    }

    rc = read_headers(c);
    if (rc != SOURCE_OK) {
        sock_close(c);
        return rc;
    }

    c->ready = 1;
    err_reset(c);
    return SOURCE_OK;
}

/* Moves body bytes out of the staging buffer, de-chunking on the way when
 * the server framed it that way. Returns bytes written. */
static int drain_raw(conn *c, uint8_t *dst, uint32_t cap, uint32_t *out_got) {
    uint32_t avail = c->raw_len - c->raw_pos;

    *out_got = 0;
    if (avail == 0) return SOURCE_OK;

    if (!c->is_chunked) {
        uint32_t n = (cap < avail) ? cap : avail;
        memcpy(dst, c->raw + c->raw_pos, n);
        c->raw_pos += n;
        *out_got = n;
        return SOURCE_OK;
    }

    {
        uint32_t       in_used  = 0;
        uint32_t       out_used = 0;
        int            done     = 0;
        http_parse_err pe = http_chunked_pull(&c->chunk, c->raw + c->raw_pos, avail, &in_used, dst, cap, &out_used, &done);

        if (pe != HTTP_PARSE_OK) {
            err_reset(c);
            err_add(c, pe == HTTP_PARSE_TOOBIG ? "the server sent a chunk header this client refuses as too large"
                                            : "the server's chunked framing is malformed");
            return SOURCE_ERR_IO;
        }
        c->raw_pos += in_used;
        *out_got = out_used;
        if (done) c->eof = 1;
        return SOURCE_OK;
    }
}

static int conn_read(conn *c, uint8_t *dst, uint32_t cap, uint32_t *out_got) {
    if (out_got) *out_got = 0;
    if (!dst || cap == 0 || !out_got) {
        err_reset(c);
        err_add(c, "the HTTP source was asked to read into nothing");
        return SOURCE_ERR_ARG;
    }
    if (!c->ready) {
        err_reset(c);
        err_add(c, "the HTTP source was read before it was opened");
        return SOURCE_ERR_STATE;
    }

    for (;;) {
        int rc;

        if (g_cancel) {
            err_reset(c);
            err_add(c, "the connection was stopped");
            return SOURCE_ERR_CANCELLED;
        }

        rc = drain_raw(c, dst, cap, out_got);
        if (rc != SOURCE_OK) return rc;
        if (*out_got > 0) return SOURCE_OK;

        /* Nothing came out of the staging buffer. Either the body is over,
         * or every byte in it was chunk framing rather than film -- in which
         * case the loop goes round and reads more, which is why this is a
         * loop and not a single pass. */
        if (c->eof) return SOURCE_ERR_EOF;

        rc = fill_raw(c, HTTP_RECV_TIMEOUT_MS);
        if (rc == SOURCE_ERR_EOF) {
            /* A close IS the end of the body when the server delimited it
             * that way; it is a truncation when the server promised chunks
             * and never sent the last one, and the two are reported
             * differently because only one of them is a fault. */
            c->eof = 1;
            if (c->is_chunked) {
                err_reset(c);
                err_add(c, "the server closed the connection in the middle of a chunked response");
                return SOURCE_ERR_IO;
            }
            return SOURCE_ERR_EOF;
        }
        if (rc != SOURCE_OK) return rc;
    }
}

static void conn_close(conn *c) {
    sock_close(c);
    c->ready      = 0;
    c->eof        = 0;
    c->raw_len    = 0;
    c->raw_pos    = 0;
    c->is_chunked = 0;
}

/* ---------------------------------------------------- the stream: source_http */

static int stream_open(const char *location) {
    http_request r;

    conns_init();
    memset(&r, 0, sizeof r);
    r.method  = "GET";
    r.url     = location;
    r.headers = g_stream_headers[0] ? g_stream_headers : NULL;
    return open_request(&g_stream, &r);
}

static int stream_read(uint8_t *dst, uint32_t cap, uint32_t *out_got) { return conn_read(&g_stream, dst, cap, out_got); }
static void stream_close(void) {
    conns_init();
    conn_close(&g_stream);
}
static const char *stream_error(void) { return g_stream.err; }

void http_set_stream_headers(const char *headers) {
    uint32_t n = 0;

    if (headers)
        while (headers[n] && n + 1 < HTTP_EXTRA_HEADERS_MAX) {
            g_stream_headers[n] = headers[n];
            n++;
        }
    /* Cut short it would be a header line with no CRLF, which open_request
     * refuses; dropping it whole says so at the first request instead. */
    if (headers && headers[n]) n = 0;
    g_stream_headers[n] = 0;
}

const media_source source_http = {"http", stream_open, stream_read, stream_close, stream_error};

/* -------------------------------------------------- API calls: http_fetch */

int http_fetch(const http_request *r, char *out, uint32_t cap, uint32_t *out_len) {
    conn    *c   = &g_api;
    uint32_t len = 0;
    int      rc;

    conns_init();
    if (out_len) *out_len = 0;
    if (!out || cap < 2) {
        err_reset(c);
        err_add(c, "an HTTP call was given nowhere to put the answer");
        return SOURCE_ERR_ARG;
    }
    out[0] = 0;

    rc = open_request(c, r);
    if (rc != SOURCE_OK) {
        conn_close(c);
        return rc;
    }

    for (;;) {
        uint32_t got = 0;

        if (len + 1 >= cap) {
            /* Full. One more byte decides whether that is the end or an
             * overflow: an answer is only any use whole, and a JSON document
             * cut short is a different document. */
            uint8_t extra;
            rc = conn_read(c, &extra, 1, &got);
            if (rc == SOURCE_ERR_EOF) break;
            conn_close(c);
            if (rc != SOURCE_OK) return rc;
            err_reset(c);
            err_add(c, "the server's answer was larger than the ");
            err_add_dec(c, (long)cap);
            err_add(c, " bytes set aside for it");
            return SOURCE_ERR_IO;
        }
        rc = conn_read(c, (uint8_t *)out + len, cap - 1 - len, &got);
        if (rc == SOURCE_ERR_EOF) break;
        if (rc != SOURCE_OK) {
            conn_close(c);
            return rc;
        }
        len += got;
    }

    conn_close(c);
    out[len] = 0;
    if (out_len) *out_len = len;
    err_reset(c);
    return SOURCE_OK;
}

int http_status(void) { return g_api.status; }

const char *http_error(void) { return g_api.err; }

/* ------------------------------------------------------------- stopping */

void http_cancel(void) {
    int s1, s2;

    conns_init();
    s1 = g_stream.sock;
    s2 = g_api.sock;
    g_cancel = 1;
    /* shutdown rather than close: close would free the descriptor while the
     * read loop is still holding it, and the number could be handed to
     * something else before that loop noticed. A shutdown socket stays a
     * socket, and every call on it fails immediately, which is what breaks a
     * recv that is already blocked.
     *
     * UNVERIFIED -- REQUIRES REAL PSP TEST: that a shutdown from the
     * callback's thread interrupts a recv in progress is how BSD sockets
     * behave and how design section 3.4 assumes they behave here, but it has
     * not been seen on hardware. The sliced poll in wait_for() is the
     * fallback that does not depend on it. */
    if (s1 >= 0) sceNetInetShutdown(s1, SHUT_RDWR);
    if (s2 >= 0) sceNetInetShutdown(s2, SHUT_RDWR);
}

#endif /* __PSP__ */
