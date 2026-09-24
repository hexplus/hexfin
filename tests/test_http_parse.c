/* See net/http_parse.h.
 *
 * The socket cannot run here -- it is a PSP radio, and there isn't one -- but
 * every byte that comes off it can be examined here, and that is the only
 * reason the parsing lives in a file of its own rather than inside
 * net/http.c. The same split, for the same reason, as media/h264_au.c against
 * media/video_psp.c.
 *
 * TWO CHECKS MATTER MORE THAN THE REST. The first is the one that feeds a
 * response a byte at a time: a socket read boundary falls wherever the
 * network puts it, including between the CR and the LF that end the header
 * block, and a parser that is only ever handed whole responses is a parser
 * whose one real input has never been tried. The second is the chunk size
 * that overflows -- eighteen hex digits is all it takes to make an unguarded
 * accumulator wrap to a small number and hand a bounded-looking length to
 * code that then copies without one. */

#include "net/http_parse.h"

#include "test.h"

#include <stdio.h>
#include <string.h>

/* ---- helpers ---------------------------------------------------------- */

/* Feeds a whole response header block in one go. */
static http_parse_err feed_all(http_headers *h, const char *text, uint32_t *used) {
    http_headers_init(h);
    return http_headers_feed(h, (const uint8_t *)text, (uint32_t)strlen(text), used);
}

/* ---- the URL ---------------------------------------------------------- */

static int t_url_splits(char *note, unsigned n) {
    http_url u;

    if (http_url_parse("http://192.168.100.78:8096/Videos/1/stream.mp4?static=true", &u) != HTTP_PARSE_OK) {
        snprintf(note, n, "a plain Jellyfin URL was refused");
        return 1;
    }
    if (strcmp(u.host, "192.168.100.78") != 0) {
        snprintf(note, n, "host came out as \"%s\"", u.host);
        return 1;
    }
    if (u.port != 8096) {
        snprintf(note, n, "port came out as %u", (unsigned)u.port);
        return 1;
    }
    if (strcmp(u.path, "/Videos/1/stream.mp4?static=true") != 0) {
        snprintf(note, n, "path came out as \"%s\"", u.path);
        return 1;
    }
    return 0;
}

static int t_url_defaults(char *note, unsigned n) {
    http_url u;

    if (http_url_parse("http://example.test", &u) != HTTP_PARSE_OK) {
        snprintf(note, n, "a URL with no path was refused");
        return 1;
    }
    if (u.port != 80 || strcmp(u.path, "/") != 0) {
        snprintf(note, n, "expected port 80 and path \"/\", got %u and \"%s\"", (unsigned)u.port, u.path);
        return 1;
    }
    /* A query with no path is still a request for the root. */
    if (http_url_parse("http://example.test?a=1", &u) != HTTP_PARSE_OK || strcmp(u.path, "/?a=1") != 0) {
        snprintf(note, n, "a query with no path became \"%s\"", u.path);
        return 1;
    }
    return 0;
}

static int t_url_refuses_https_and_userinfo(char *note, unsigned n) {
    http_url u;

    if (http_url_parse("https://example.test/x", &u) != HTTP_PARSE_MALFORMED) {
        snprintf(note, n, "an https URL was not refused -- it must never be silently downgraded");
        return 1;
    }
    if (http_url_parse("http://user@evil.test/x", &u) != HTTP_PARSE_MALFORMED) {
        snprintf(note, n, "a URL with userinfo was accepted");
        return 1;
    }
    if (http_url_parse("ftp://example.test/x", &u) != HTTP_PARSE_MALFORMED) {
        snprintf(note, n, "a non-http scheme was accepted");
        return 1;
    }
    if (http_url_parse("http:///x", &u) != HTTP_PARSE_MALFORMED) {
        snprintf(note, n, "a URL with an empty host was accepted");
        return 1;
    }
    return 0;
}

static int t_url_port_overflow_refused(char *note, unsigned n) {
    http_url u;

    /* 655360 wraps a 16-bit port, and 99999999999 wraps a 32-bit
     * accumulator. Both must be refused, not folded into something inside
     * the range. */
    if (http_url_parse("http://h:655360/", &u) != HTTP_PARSE_MALFORMED) {
        snprintf(note, n, "port 655360 was accepted as %u", (unsigned)u.port);
        return 1;
    }
    if (http_url_parse("http://h:99999999999/", &u) != HTTP_PARSE_MALFORMED) {
        snprintf(note, n, "a port that overflows the accumulator was accepted as %u", (unsigned)u.port);
        return 1;
    }
    if (http_url_parse("http://h:0/", &u) != HTTP_PARSE_MALFORMED) {
        snprintf(note, n, "port 0 was accepted");
        return 1;
    }
    return 0;
}

static int t_url_too_long_refused(char *note, unsigned n) {
    static char    url[HTTP_PATH_MAX + 64];
    http_url       u;
    unsigned       i;
    http_parse_err e;

    memcpy(url, "http://h/", 9);
    for (i = 0; i < HTTP_PATH_MAX + 8; i++) url[9 + i] = 'a';
    url[9 + i] = 0;

    e = http_url_parse(url, &u);
    if (e != HTTP_PARSE_TOOBIG) {
        snprintf(note, n, "a path past the cap returned %d instead of being refused", (int)e);
        return 1;
    }
    return 0;
}

static int t_ipv4_strict(char *note, unsigned n) {
    uint32_t a = 0;

    if (!http_ipv4_parse("192.168.100.78", &a) || a != 0xC0A8644Eu) {
        snprintf(note, n, "192.168.100.78 came out as 0x%08X", (unsigned)a);
        return 1;
    }
    if (http_ipv4_parse("jellyfin.local", &a)) {
        snprintf(note, n, "a host name was mistaken for an address");
        return 1;
    }
    /* The permissive forms a C library would accept, every one of which is a
     * way for two resolvers to read one string as two addresses. */
    if (http_ipv4_parse("192.168.100.256", &a) || http_ipv4_parse("192.168.100.78.", &a) ||
        http_ipv4_parse("192.168.100", &a) || http_ipv4_parse("0x7f.0.0.1", &a) || http_ipv4_parse("2130706433", &a)) {
        snprintf(note, n, "a non-canonical address form was accepted");
        return 1;
    }
    return 0;
}

/* ---- the response header block ---------------------------------------- */

static const char k_ok_response[] = "HTTP/1.1 200 OK\r\n"
                                    "Server: Kestrel\r\n"
                                    "Content-Type: video/mp4\r\n"
                                    "Transfer-Encoding: chunked\r\n"
                                    "\r\n";

static int t_status_line_and_headers(char *note, unsigned n) {
    http_headers  h;
    http_response r;
    uint32_t      used = 0;

    if (feed_all(&h, k_ok_response, &used) != HTTP_PARSE_OK) {
        snprintf(note, n, "a well-formed header block was not accepted");
        return 1;
    }
    if (used != strlen(k_ok_response)) {
        snprintf(note, n, "the header block consumed %u of %u bytes", (unsigned)used, (unsigned)strlen(k_ok_response));
        return 1;
    }
    if (http_response_parse(&h, &r) != HTTP_PARSE_OK) {
        snprintf(note, n, "a well-formed header block would not parse");
        return 1;
    }
    if (r.status != 200 || !r.chunked || r.have_content_length) {
        snprintf(note, n, "status %d, chunked %d, content-length %d", r.status, r.chunked, r.have_content_length);
        return 1;
    }
    return 0;
}

static int t_body_after_headers_is_kept(char *note, unsigned n) {
    http_headers h;
    char         wire[256];
    uint32_t     used = 0;

    /* The server's first packet carries the header block AND the first bytes
     * of the film. Those bytes cannot be put back on the socket, so the
     * parser has to say exactly where the header stopped. */
    snprintf(wire, sizeof wire, "HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\n\r\nFTYP");
    if (feed_all(&h, wire, &used) != HTTP_PARSE_OK) {
        snprintf(note, n, "a header block followed by body was not accepted");
        return 1;
    }
    if (used != strlen(wire) - 4) {
        snprintf(note, n, "the header claimed %u bytes, leaving %u of body instead of 4", (unsigned)used,
                 (unsigned)(strlen(wire) - used));
        return 1;
    }
    return 0;
}

static int t_malformed_status_refused(char *note, unsigned n) {
    http_headers  h;
    http_response r;
    unsigned      i;
    static const char *const bad[] = {
        "HTTP/2.0 200 OK\r\n\r\n",       /* a version this client does not speak */
        "HTTP/1.1  200 OK\r\n\r\n",      /* two spaces: the code is not where it must be */
        "HTTP/1.1 20 OK\r\n\r\n",        /* two digits */
        "HTTP/1.1 2000 OK\r\n\r\n",      /* four digits: the code acted on is not the code sent */
        "HTTP/1.1 2O0 OK\r\n\r\n",       /* a letter among the digits */
        "200 OK\r\n\r\n",                /* no status line at all */
        "HTTP/1.1 200 OK\r\nno-colon-here\r\n\r\n",
        "HTTP/1.1 200 OK\r\nX: 1\r\n  folded\r\n\r\n", /* obsolete line folding */
    };

    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        uint32_t used = 0;
        if (feed_all(&h, bad[i], &used) != HTTP_PARSE_OK) {
            snprintf(note, n, "case %u never even terminated", i);
            return 1;
        }
        if (http_response_parse(&h, &r) != HTTP_PARSE_MALFORMED) {
            snprintf(note, n, "case %u (\"%.20s\") was accepted", i, bad[i]);
            return 1;
        }
    }
    snprintf(note, n, "%u malformed responses refused", (unsigned)(sizeof bad / sizeof bad[0]));
    return 0;
}

static int t_unterminated_headers_ask_for_more(char *note, unsigned n) {
    http_headers h;
    uint32_t     used = 0;

    /* Note what this must NOT do: reading one byte past what it was given to
     * decide there is no terminator. Under AddressSanitizer that is the
     * failure this check exists to catch. */
    if (feed_all(&h, "HTTP/1.1 200 OK\r\nServer: x\r\n", &used) != HTTP_PARSE_AGAIN) {
        snprintf(note, n, "an unterminated block was not reported as incomplete");
        return 1;
    }
    if (used != 28) {
        snprintf(note, n, "an unterminated block consumed %u of 28 bytes", (unsigned)used);
        return 1;
    }
    /* And the end of a block that stops between the CR and the LF. */
    if (feed_all(&h, "HTTP/1.1 200 OK\r\n\r", &used) != HTTP_PARSE_AGAIN) {
        snprintf(note, n, "a block cut between the CR and the LF was not reported as incomplete");
        return 1;
    }
    return 0;
}

static int t_oversized_headers_refused(char *note, unsigned n) {
    static uint8_t wire[HTTP_HEADER_MAX * 2];
    http_headers   h;
    uint32_t       i;
    uint32_t       used = 0;

    /* A peer that sends header bytes forever. It must be refused at the cap,
     * not followed, and not accumulated past the buffer. */
    memcpy(wire, "HTTP/1.1 200 OK\r\n", 17);
    for (i = 17; i < sizeof wire; i++) wire[i] = 'A';

    http_headers_init(&h);
    if (http_headers_feed(&h, wire, (uint32_t)sizeof wire, &used) != HTTP_PARSE_TOOBIG) {
        snprintf(note, n, "a header block twice the cap was not refused");
        return 1;
    }
    if (used > HTTP_HEADER_MAX) {
        snprintf(note, n, "the refusal still consumed %u bytes, past the %u cap", (unsigned)used,
                 (unsigned)HTTP_HEADER_MAX);
        return 1;
    }
    snprintf(note, n, "refused at the %u byte cap", (unsigned)HTTP_HEADER_MAX);
    return 0;
}

static int t_non_200_reported_as_itself(char *note, unsigned n) {
    http_headers  h;
    http_response r;
    uint32_t      used = 0;

    if (feed_all(&h, "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\n", &used) != HTTP_PARSE_OK ||
        http_response_parse(&h, &r) != HTTP_PARSE_OK) {
        snprintf(note, n, "a 404 response would not parse");
        return 1;
    }
    /* Parsing succeeds: the response IS well-formed HTTP. Deciding that 404
     * is not good enough belongs to net/http.c, and it can only decide that
     * if the code arrives here intact. */
    if (r.status != 404 || !r.have_content_length || r.content_length != 9) {
        snprintf(note, n, "status %d, content-length %d/%llu", r.status, r.have_content_length,
                 (unsigned long long)r.content_length);
        return 1;
    }

    if (feed_all(&h, "HTTP/1.1 302 Found\r\nLocation: http://elsewhere.test/x\r\n\r\n", &used) != HTTP_PARSE_OK ||
        http_response_parse(&h, &r) != HTTP_PARSE_OK) {
        snprintf(note, n, "a redirect would not parse");
        return 1;
    }
    if (r.status != 302 || strcmp(r.location, "http://elsewhere.test/x") != 0) {
        snprintf(note, n, "redirect came out as %d to \"%s\"", r.status, r.location);
        return 1;
    }
    return 0;
}

static int t_conflicting_lengths_refused(char *note, unsigned n) {
    http_headers  h;
    http_response r;
    uint32_t      used = 0;

    /* Both framings at once, and two lengths that disagree: the two shapes
     * of a response that two parsers read as two different bodies. */
    if (feed_all(&h, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\nTransfer-Encoding: chunked\r\n\r\n", &used) !=
            HTTP_PARSE_OK ||
        http_response_parse(&h, &r) != HTTP_PARSE_MALFORMED) {
        snprintf(note, n, "Content-Length together with Transfer-Encoding was accepted");
        return 1;
    }
    if (feed_all(&h, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\nContent-Length: 11\r\n\r\n", &used) != HTTP_PARSE_OK ||
        http_response_parse(&h, &r) != HTTP_PARSE_MALFORMED) {
        snprintf(note, n, "two disagreeing Content-Length headers were accepted");
        return 1;
    }
    if (feed_all(&h, "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n", &used) != HTTP_PARSE_OK ||
        http_response_parse(&h, &r) != HTTP_PARSE_MALFORMED) {
        snprintf(note, n, "a Transfer-Encoding this client cannot undo was accepted");
        return 1;
    }
    if (feed_all(&h, "HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999999999\r\n\r\n", &used) != HTTP_PARSE_OK ||
        http_response_parse(&h, &r) != HTTP_PARSE_TOOBIG) {
        snprintf(note, n, "a Content-Length that overflows 64 bits was accepted");
        return 1;
    }
    return 0;
}

static int t_header_split_across_reads(char *note, unsigned n) {
    http_headers  h;
    http_response r;
    uint32_t      i;
    uint32_t      total = (uint32_t)strlen(k_ok_response);

    /* One byte per read -- the worst split a socket can produce, and the one
     * that catches a parser holding state in a local. */
    http_headers_init(&h);
    for (i = 0; i < total; i++) {
        uint32_t       used = 0;
        http_parse_err e    = http_headers_feed(&h, (const uint8_t *)k_ok_response + i, 1, &used);

        if (i + 1 < total) {
            if (e != HTTP_PARSE_AGAIN) {
                snprintf(note, n, "byte %u of %u ended the block early", (unsigned)i, (unsigned)total);
                return 1;
            }
        } else if (e != HTTP_PARSE_OK) {
            snprintf(note, n, "the last byte did not complete the block");
            return 1;
        }
        if (used != 1) {
            snprintf(note, n, "byte %u reported %u bytes consumed", (unsigned)i, (unsigned)used);
            return 1;
        }
    }
    if (http_response_parse(&h, &r) != HTTP_PARSE_OK || r.status != 200 || !r.chunked) {
        snprintf(note, n, "the byte-at-a-time block parsed differently from the whole one");
        return 1;
    }
    snprintf(note, n, "%u reads of one byte", (unsigned)total);
    return 0;
}

/* ---- chunked framing --------------------------------------------------- */

/* Runs a chunked stream through the decoder in `step`-byte reads with an
 * `out_cap`-byte sink, and returns the body it produced. */
static http_parse_err run_chunked(const char *wire, uint32_t step, uint32_t out_cap, char *body, uint32_t body_cap,
                                  uint32_t *body_len, int *done) {
    http_chunked c;
    uint32_t     wlen = (uint32_t)strlen(wire);
    uint32_t     wpos = 0;

    http_chunked_init(&c);
    *body_len = 0;
    *done     = 0;

    while (wpos < wlen && !*done) {
        uint32_t take = wlen - wpos;
        uint32_t in_used, out_used;
        uint8_t  sink[64];
        uint32_t cap = (out_cap < sizeof sink) ? out_cap : (uint32_t)sizeof sink;

        if (take > step) take = step;

        for (;;) {
            http_parse_err e = http_chunked_pull(&c, (const uint8_t *)wire + wpos, take, &in_used, sink, cap, &out_used,
                                                 done);
            if (e != HTTP_PARSE_OK) return e;

            if (out_used > 0) {
                if (*body_len + out_used > body_cap) return HTTP_PARSE_TOOBIG;
                memcpy(body + *body_len, sink, out_used);
                *body_len += out_used;
            }
            wpos += in_used;
            take -= in_used;
            if (take == 0 || *done) break;
            if (in_used == 0 && out_used == 0) break; /* no progress: the sink is the limit, loop again */
        }
    }
    return HTTP_PARSE_OK;
}

static int t_chunked_reassembles(char *note, unsigned n) {
    static const char wire[] = "4\r\nWiki\r\n5\r\npedia\r\ne\r\n in\r\n\r\nchunks.\r\n0\r\n\r\n";
    char              body[128];
    uint32_t          len  = 0;
    int               done = 0;
    unsigned          step;

    /* Every read size from one byte to the whole thing: the boundary between
     * two reads has to be able to fall anywhere, including inside a chunk
     * size line and between a chunk's last byte and its trailing CRLF. */
    for (step = 1; step <= sizeof wire; step++) {
        if (run_chunked(wire, step, 64, body, sizeof body, &len, &done) != HTTP_PARSE_OK) {
            snprintf(note, n, "a valid chunked body was refused at a read size of %u", step);
            return 1;
        }
        if (!done) {
            snprintf(note, n, "the terminating chunk was missed at a read size of %u", step);
            return 1;
        }
        if (len != 23 || memcmp(body, "Wikipedia in\r\n\r\nchunks.", 23) != 0) {
            snprintf(note, n, "at a read size of %u the body came out as %u bytes", step, (unsigned)len);
            return 1;
        }
    }
    snprintf(note, n, "23 bytes identical across %u read sizes", (unsigned)sizeof wire);
    return 0;
}

static int t_chunked_small_sink(char *note, unsigned n) {
    static const char wire[] = "a\r\n0123456789\r\n0\r\n\r\n";
    char              body[64];
    uint32_t          len  = 0;
    int               done = 0;

    /* A sink smaller than one chunk. The caller's buffer filling up is not an
     * error and not an end -- it is just where this call stopped. */
    if (run_chunked(wire, 32, 3, body, sizeof body, &len, &done) != HTTP_PARSE_OK) {
        snprintf(note, n, "a chunk larger than the sink was treated as an error");
        return 1;
    }
    if (!done || len != 10 || memcmp(body, "0123456789", 10) != 0) {
        snprintf(note, n, "done %d, %u bytes out", done, (unsigned)len);
        return 1;
    }
    return 0;
}

static int t_chunk_size_overflow_refused(char *note, unsigned n) {
    char     body[64];
    uint32_t len  = 0;
    int      done = 0;

    /* Eighteen hex digits. Accumulated as v = v * 16 + d with no guard, this
     * wraps a 64-bit counter and lands on a small, plausible-looking size --
     * which is then trusted as a length. */
    if (run_chunked("ffffffffffffffffff\r\nx\r\n0\r\n\r\n", 32, 64, body, sizeof body, &len, &done) !=
        HTTP_PARSE_TOOBIG) {
        snprintf(note, n, "an eighteen-digit chunk size was not refused");
        return 1;
    }
    /* And a size that fits in 64 bits but is past what any real chunk is. */
    if (run_chunked("7fffffff\r\nx\r\n0\r\n\r\n", 32, 64, body, sizeof body, &len, &done) != HTTP_PARSE_TOOBIG) {
        snprintf(note, n, "a 2 GB chunk size was not refused");
        return 1;
    }
    return 0;
}

static int t_chunk_line_bounded(char *note, unsigned n) {
    static char wire[HTTP_CHUNK_LINE_MAX + 32];
    char        body[64];
    uint32_t    len  = 0;
    int         done = 0;
    unsigned    i;

    /* A chunk extension that never ends is the same "send bytes forever"
     * attack the header cap refuses, one layer down. */
    wire[0] = '4';
    wire[1] = ';';
    for (i = 2; i < sizeof wire - 1; i++) wire[i] = 'x';
    wire[sizeof wire - 1] = 0;

    if (run_chunked(wire, 16, 64, body, sizeof body, &len, &done) != HTTP_PARSE_TOOBIG) {
        snprintf(note, n, "an unbounded chunk extension was not refused");
        return 1;
    }
    snprintf(note, n, "refused past %u characters", (unsigned)HTTP_CHUNK_LINE_MAX);
    return 0;
}

static int t_chunk_framing_errors_refused(char *note, unsigned n) {
    char     body[64];
    uint32_t len  = 0;
    int      done = 0;
    unsigned i;
    static const char *const bad[] = {
        "4\r\nWiki--0\r\n\r\n",  /* a chunk not followed by CRLF */
        "\r\n4\r\nWiki\r\n",     /* an empty size line */
        "zz\r\nWiki\r\n",        /* a size that is not hex */
        "4\nWiki\r\n",           /* a bare LF where CRLF is required */
        "4\r\nWiki\r\rmore",     /* a CR with no LF after a chunk's data */
    };

    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        if (run_chunked(bad[i], 32, 64, body, sizeof body, &len, &done) == HTTP_PARSE_OK) {
            snprintf(note, n, "case %u (\"%.10s\") was accepted", i, bad[i]);
            return 1;
        }
    }
    snprintf(note, n, "%u malformed chunk streams refused", (unsigned)(sizeof bad / sizeof bad[0]));
    return 0;
}

static int t_chunked_stops_at_the_last_chunk(char *note, unsigned n) {
    http_chunked c;
    uint8_t      sink[32];
    uint32_t     in_used = 0, out_used = 0;
    int          done = 0;
    const char  *wire = "0\r\nX-Trailer: whatever\r\n\r\nGARBAGE";

    /* Nothing after the terminating chunk is consumed -- there is no trailer
     * parser here, on purpose, and the check is that the decoder stops dead
     * rather than trying to read one. */
    http_chunked_init(&c);
    if (http_chunked_pull(&c, (const uint8_t *)wire, (uint32_t)strlen(wire), &in_used, sink, sizeof sink, &out_used,
                          &done) != HTTP_PARSE_OK) {
        snprintf(note, n, "the terminating chunk was refused");
        return 1;
    }
    if (!done || out_used != 0) {
        snprintf(note, n, "done %d with %u body bytes", done, (unsigned)out_used);
        return 1;
    }
    if (in_used != 3) {
        snprintf(note, n, "consumed %u bytes past the terminating chunk's own 3", (unsigned)in_used);
        return 1;
    }
    return 0;
}

void test_http_parse_register(void) {
    test_add("http", "a Jellyfin URL splits into host, port and path", t_url_splits);
    test_add("http", "a URL with no port or path gets 80 and /", t_url_defaults);
    test_add("http", "https, userinfo and an empty host are refused", t_url_refuses_https_and_userinfo);
    test_add("http", "a port that would wrap the bounds check is refused", t_url_port_overflow_refused);
    test_add("http", "a path past the cap is refused, not truncated", t_url_too_long_refused);
    test_add("http", "only a canonical dotted quad is read as an address", t_ipv4_strict);
    test_add("http", "a normal status line and its headers are read", t_status_line_and_headers);
    test_add("http", "body bytes arriving with the header are not lost", t_body_after_headers_is_kept);
    test_add("http", "a malformed status line or header is refused", t_malformed_status_refused);
    test_add("http", "a block with no terminator asks for more", t_unterminated_headers_ask_for_more);
    test_add("http", "a header block past the cap is refused, not followed", t_oversized_headers_refused);
    test_add("http", "a status that is not 200 arrives intact to be judged", t_non_200_reported_as_itself);
    test_add("http", "two framings, or two lengths, at once are refused", t_conflicting_lengths_refused);
    test_add("http", "a header block split one byte per read parses the same", t_header_split_across_reads);
    test_add("http", "a chunked body survives every read boundary", t_chunked_reassembles);
    test_add("http", "a chunk larger than the caller's buffer is not an error", t_chunked_small_sink);
    test_add("http", "a chunk size that would wrap the accumulator is refused", t_chunk_size_overflow_refused);
    test_add("http", "a chunk size line that never ends is refused", t_chunk_line_bounded);
    test_add("http", "malformed chunk framing is refused", t_chunk_framing_errors_refused);
    test_add("http", "nothing after the terminating chunk is consumed", t_chunked_stops_at_the_last_chunk);
}
