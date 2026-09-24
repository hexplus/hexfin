/* See net/http_parse.h. */

#include "net/http_parse.h"

#include <string.h>

/* ---- small helpers ---------------------------------------------------- */

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

/* `name` is the lowercase spelling we are looking for; `b` is however the
 * server happened to capitalise it. Length is compared first so a prefix can
 * never match a longer header -- `Content-Length-Hint` is not
 * `Content-Length`. */
static int name_is(const char *name, const uint8_t *b, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (name[i] == 0) return 0;
        if (lower((char)b[i]) != name[i]) return 0;
    }
    return name[n] == 0;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int is_ows(uint8_t c) { return c == ' ' || c == '\t'; }

/* ---- the URL ---------------------------------------------------------- */

http_parse_err http_url_parse(const char *url, http_url *out) {
    uint32_t i = 0;
    uint32_t host_len = 0;
    uint32_t path_len = 0;
    uint32_t port     = 0;
    int      have_port = 0;

    if (!url || !out) return HTTP_PARSE_ARG;

    out->host[0] = 0;
    out->path[0] = 0;
    out->port    = 80;

    /* The scheme, compared case-insensitively because RFC 3986 says it is,
     * and refused by name when it is https rather than downgraded -- see the
     * header's list of what this refuses. */
    {
        static const char http_scheme[]  = "http://";
        static const char https_scheme[] = "https://";
        uint32_t          k;

        for (k = 0; https_scheme[k]; k++)
            if (lower(url[k]) != https_scheme[k]) break;
        if (https_scheme[k] == 0) return HTTP_PARSE_MALFORMED;

        for (k = 0; http_scheme[k]; k++)
            if (lower(url[k]) != http_scheme[k]) return HTTP_PARSE_MALFORMED;
        i = k;
    }

    /* The authority, up to the first '/', '?' or '#'. */
    while (url[i] && url[i] != '/' && url[i] != '?' && url[i] != '#') {
        char c = url[i];

        if (c == '@') return HTTP_PARSE_MALFORMED; /* userinfo -- see the header */

        if (c == ':') {
            i++;
            have_port = 1;
            if (url[i] < '0' || url[i] > '9') return HTTP_PARSE_MALFORMED;
            while (url[i] >= '0' && url[i] <= '9') {
                uint32_t d = (uint32_t)(url[i] - '0');
                /* Divided rather than multiplied out and then checked: the
                 * product is what would wrap, and a check on a wrapped
                 * product passes. Same idiom as media/fmp4.c's next_box. */
                if (port > (65535u - d) / 10u) return HTTP_PARSE_MALFORMED;
                port = port * 10u + d;
                i++;
            }
            if (url[i] && url[i] != '/' && url[i] != '?' && url[i] != '#') return HTTP_PARSE_MALFORMED;
            break;
        }

        if (host_len + 1 >= HTTP_HOST_MAX) return HTTP_PARSE_TOOBIG;
        out->host[host_len++] = c;
        i++;
    }
    out->host[host_len] = 0;

    if (host_len == 0) return HTTP_PARSE_MALFORMED;
    if (have_port && port == 0) return HTTP_PARSE_MALFORMED;
    if (have_port) out->port = (uint16_t)port;

    /* A fragment is the client's own business and never goes on the wire,
     * so it is cut here rather than sent as part of the request target. */
    if (url[i] == 0 || url[i] == '#') {
        out->path[0] = '/';
        out->path[1] = 0;
        return HTTP_PARSE_OK;
    }

    if (url[i] == '?') {
        if (path_len + 1 >= HTTP_PATH_MAX) return HTTP_PARSE_TOOBIG;
        out->path[path_len++] = '/';
    }
    while (url[i] && url[i] != '#') {
        if (path_len + 1 >= HTTP_PATH_MAX) return HTTP_PARSE_TOOBIG;
        out->path[path_len++] = url[i++];
    }
    out->path[path_len] = 0;
    return HTTP_PARSE_OK;
}

int http_ipv4_parse(const char *host, uint32_t *out_host_order) {
    uint32_t addr = 0;
    uint32_t i    = 0;
    int      part;

    if (!host || !out_host_order) return 0;

    for (part = 0; part < 4; part++) {
        uint32_t v      = 0;
        uint32_t digits = 0;

        while (host[i] >= '0' && host[i] <= '9') {
            if (digits == 3) return 0; /* 4+ digits is not a dotted quad */
            v = v * 10u + (uint32_t)(host[i] - '0');
            digits++;
            i++;
        }
        if (digits == 0 || v > 255u) return 0;
        addr = (addr << 8) | v;

        if (part < 3) {
            if (host[i] != '.') return 0;
            i++;
        }
    }
    if (host[i] != 0) return 0; /* a trailing dot, or anything else */

    *out_host_order = addr;
    return 1;
}

/* ---- the response header block ---------------------------------------- */

void http_headers_init(http_headers *h) {
    if (!h) return;
    h->len      = 0;
    h->complete = 0;
}

http_parse_err http_headers_feed(http_headers *h, const uint8_t *in, uint32_t in_len, uint32_t *in_used) {
    uint32_t i = 0;

    if (in_used) *in_used = 0;
    if (!h || (!in && in_len > 0)) return HTTP_PARSE_ARG;
    if (h->complete) return HTTP_PARSE_OK; /* took nothing: the rest is body */

    while (i < in_len) {
        if (h->len >= HTTP_HEADER_MAX) {
            if (in_used) *in_used = i;
            return HTTP_PARSE_TOOBIG;
        }
        h->buf[h->len++] = in[i++];

        if (h->len >= 4 && h->buf[h->len - 4] == '\r' && h->buf[h->len - 3] == '\n' && h->buf[h->len - 2] == '\r' &&
            h->buf[h->len - 1] == '\n') {
            h->complete = 1;
            if (in_used) *in_used = i;
            return HTTP_PARSE_OK;
        }
    }

    if (in_used) *in_used = i;
    /* A block that has filled the cap with no terminator can never gain one,
     * so this is refused now rather than on the next read -- otherwise a peer
     * that stops sending after exactly HTTP_HEADER_MAX bytes leaves the
     * caller waiting on a decision that has already been made. */
    if (h->len >= HTTP_HEADER_MAX) return HTTP_PARSE_TOOBIG;
    return HTTP_PARSE_AGAIN;
}

/* Decimal into a 64-bit value, refusing anything that is not all digits and
 * anything that would wrap. */
static http_parse_err parse_u64(const uint8_t *s, uint32_t n, uint64_t *out) {
    uint64_t v = 0;
    uint32_t i;

    if (n == 0) return HTTP_PARSE_MALFORMED;
    for (i = 0; i < n; i++) {
        uint64_t d;
        if (s[i] < '0' || s[i] > '9') return HTTP_PARSE_MALFORMED;
        d = (uint64_t)(s[i] - '0');
        if (v > (UINT64_MAX - d) / 10u) return HTTP_PARSE_TOOBIG;
        v = v * 10u + d;
    }
    *out = v;
    return HTTP_PARSE_OK;
}

http_parse_err http_response_parse(const http_headers *h, http_response *out) {
    uint32_t pos;

    if (!h || !out) return HTTP_PARSE_ARG;
    if (!h->complete) return HTTP_PARSE_AGAIN;

    out->status              = 0;
    out->chunked             = 0;
    out->have_content_length = 0;
    out->content_length      = 0;
    out->location[0]         = 0;

    /* The status line, checked shape-first. "HTTP/1.x SPACE ddd" is 12
     * bytes; anything shorter is not a status line however it is read. */
    if (h->len < 12) return HTTP_PARSE_MALFORMED;
    if (memcmp(h->buf, "HTTP/1.", 7) != 0) return HTTP_PARSE_MALFORMED;
    if (h->buf[7] != '0' && h->buf[7] != '1') return HTTP_PARSE_MALFORMED;
    if (h->buf[8] != ' ') return HTTP_PARSE_MALFORMED;
    {
        int k;
        for (k = 9; k < 12; k++)
            if (h->buf[k] < '0' || h->buf[k] > '9') return HTTP_PARSE_MALFORMED;
        /* A three-digit code must be followed by a space (a reason phrase)
         * or the end of the line. A fourth digit would mean the code this
         * client acts on is not the code the server sent. */
        if (h->buf[12] != ' ' && h->buf[12] != '\r') return HTTP_PARSE_MALFORMED;
        out->status = (h->buf[9] - '0') * 100 + (h->buf[10] - '0') * 10 + (h->buf[11] - '0');
    }

    /* Past the status line's own CRLF. */
    pos = 12;
    while (pos + 1 < h->len && !(h->buf[pos] == '\r' && h->buf[pos + 1] == '\n')) pos++;
    if (pos + 1 >= h->len) return HTTP_PARSE_MALFORMED;
    pos += 2;

    while (pos < h->len) {
        uint32_t line_start = pos;
        uint32_t colon      = 0;
        uint32_t vs, ve;

        if (pos + 1 < h->len && h->buf[pos] == '\r' && h->buf[pos + 1] == '\n') break; /* end of the block */

        /* Obsolete line folding: a header line that begins with whitespace
         * is a continuation of the one above. Refused -- see the header. */
        if (is_ows(h->buf[pos])) return HTTP_PARSE_MALFORMED;

        while (pos + 1 < h->len && !(h->buf[pos] == '\r' && h->buf[pos + 1] == '\n')) {
            if (h->buf[pos] == ':' && colon == 0) colon = pos;
            pos++;
        }
        if (pos + 1 >= h->len) return HTTP_PARSE_MALFORMED;
        if (colon == 0) return HTTP_PARSE_MALFORMED; /* a line that is not name: value */

        vs = colon + 1;
        ve = pos;
        while (vs < ve && is_ows(h->buf[vs])) vs++;
        while (ve > vs && is_ows(h->buf[ve - 1])) ve--;

        if (name_is("content-length", h->buf + line_start, colon - line_start)) {
            uint64_t       v;
            http_parse_err e = parse_u64(h->buf + vs, ve - vs, &v);
            if (e != HTTP_PARSE_OK) return e;
            /* Two Content-Length headers that disagree are how two parsers
             * end up reading two different bodies out of one response. */
            if (out->have_content_length && out->content_length != v) return HTTP_PARSE_MALFORMED;
            out->have_content_length = 1;
            out->content_length      = v;
        } else if (name_is("transfer-encoding", h->buf + line_start, colon - line_start)) {
            if (!name_is("chunked", h->buf + vs, ve - vs)) return HTTP_PARSE_MALFORMED;
            out->chunked = 1;
        } else if (name_is("location", h->buf + line_start, colon - line_start)) {
            uint32_t n = ve - vs;
            if (n + 1 > HTTP_LOCATION_MAX) return HTTP_PARSE_TOOBIG;
            memcpy(out->location, h->buf + vs, n);
            out->location[n] = 0;
        }

        pos += 2;
    }

    /* Length declared twice, two different ways. */
    if (out->chunked && out->have_content_length) return HTTP_PARSE_MALFORMED;
    return HTTP_PARSE_OK;
}

/* ---- chunked framing --------------------------------------------------- */

enum { CH_SIZE = 0, CH_DATA, CH_CR, CH_LF, CH_DONE };

void http_chunked_init(http_chunked *c) {
    if (!c) return;
    c->state     = CH_SIZE;
    c->remaining = 0;
    c->line_len  = 0;
}

/* The size line: hex digits, then optionally a ';' and an extension nothing
 * here reads. */
static http_parse_err parse_chunk_size(const char *s, uint32_t n, uint64_t *out) {
    uint64_t v      = 0;
    uint32_t digits = 0;
    uint32_t i      = 0;

    for (; i < n; i++) {
        int d = hexval(s[i]);
        if (d < 0) break;
        /* Divided rather than multiplied: v * 16 + d computed first wraps
         * and then passes any check made on the result. A size line of
         * eighteen 'f's is enough to reach it. */
        if (v > (HTTP_CHUNK_MAX - (uint64_t)d) / 16u) return HTTP_PARSE_TOOBIG;
        v = v * 16u + (uint64_t)d;
        digits++;
    }
    if (digits == 0) return HTTP_PARSE_MALFORMED;
    if (i < n && s[i] != ';') return HTTP_PARSE_MALFORMED;

    *out = v;
    return HTTP_PARSE_OK;
}

http_parse_err http_chunked_pull(http_chunked *c, const uint8_t *in, uint32_t in_len, uint32_t *in_used, uint8_t *out,
                                 uint32_t out_cap, uint32_t *out_used, int *out_done) {
    uint32_t ci = 0;
    uint32_t co = 0;

    if (in_used) *in_used = 0;
    if (out_used) *out_used = 0;
    if (out_done) *out_done = 0;
    if (!c || !in_used || !out_used || !out_done) return HTTP_PARSE_ARG;
    if ((!in && in_len > 0) || (!out && out_cap > 0)) return HTTP_PARSE_ARG;

    for (;;) {
        if (c->state == CH_DONE) break;

        if (c->state == CH_DATA) {
            uint32_t n;

            if (c->remaining == 0) {
                c->state = CH_CR;
                continue;
            }
            if (ci >= in_len || co >= out_cap) break;

            /* Every limit taken by subtraction from what is left, never by
             * adding an offset to a length off the wire. */
            n = in_len - ci;
            if (out_cap - co < n) n = out_cap - co;
            if (c->remaining < (uint64_t)n) n = (uint32_t)c->remaining;

            memcpy(out + co, in + ci, n);
            ci += n;
            co += n;
            c->remaining -= n;
            continue;
        }

        if (ci >= in_len) break;

        {
            uint8_t ch = in[ci++];

            if (c->state == CH_SIZE) {
                if (ch == '\n') {
                    uint64_t       size;
                    http_parse_err e;

                    if (c->line_len == 0 || c->line[c->line_len - 1] != '\r') return HTTP_PARSE_MALFORMED;
                    c->line_len--; /* drop the CR */

                    e = parse_chunk_size(c->line, c->line_len, &size);
                    if (e != HTTP_PARSE_OK) return e;

                    c->line_len = 0;
                    if (size == 0) {
                        c->state = CH_DONE;
                    } else {
                        c->remaining = size;
                        c->state     = CH_DATA;
                    }
                    continue;
                }
                if (c->line_len >= HTTP_CHUNK_LINE_MAX) return HTTP_PARSE_TOOBIG;
                c->line[c->line_len++] = (char)ch;
                continue;
            }

            if (c->state == CH_CR) {
                if (ch != '\r') return HTTP_PARSE_MALFORMED;
                c->state = CH_LF;
                continue;
            }

            /* CH_LF */
            if (ch != '\n') return HTTP_PARSE_MALFORMED;
            c->state    = CH_SIZE;
            c->line_len = 0;
        }
    }

    *in_used  = ci;
    *out_used = co;
    *out_done = (c->state == CH_DONE);
    return HTTP_PARSE_OK;
}
