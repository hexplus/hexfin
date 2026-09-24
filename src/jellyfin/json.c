/* See jellyfin/json.h. */
#include "jellyfin/json.h"

#include <string.h>

/* Keys are compared after unescaping, so they are decoded into a buffer of
 * their own. A key longer than this cannot be one this client looks for; it
 * is still read to its end, and simply never matches. */
#define JSON_KEY_MAX 64

typedef struct {
    const char *p;
    const char *end;
} cursor;

static void skip_ws(cursor *c) {
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) c->p++;
}

static int hex4(const char *p, unsigned *out) {
    unsigned v = 0;
    int      i;

    for (i = 0; i < 4; i++) {
        char ch = p[i];
        v <<= 4;
        if (ch >= '0' && ch <= '9') v |= (unsigned)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') v |= (unsigned)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') v |= (unsigned)(ch - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

/* Appends one byte to out, remembering an overflow rather than stopping:
 * the string still has to be read to its end, so the cursor lands after it. */
typedef struct {
    char  *out;
    size_t cap;
    size_t len;
    int    overflow;
} sink;

static void put(sink *s, unsigned char b) {
    if (!s->out) return;
    if (s->len + 1 < s->cap) s->out[s->len++] = (char)b;
    else s->overflow = 1;
}

static void put_utf8(sink *s, unsigned cp) {
    if (cp < 0x80) {
        put(s, (unsigned char)cp);
    } else if (cp < 0x800) {
        put(s, (unsigned char)(0xC0 | (cp >> 6)));
        put(s, (unsigned char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        put(s, (unsigned char)(0xE0 | (cp >> 12)));
        put(s, (unsigned char)(0x80 | ((cp >> 6) & 0x3F)));
        put(s, (unsigned char)(0x80 | (cp & 0x3F)));
    } else {
        put(s, (unsigned char)(0xF0 | (cp >> 18)));
        put(s, (unsigned char)(0x80 | ((cp >> 12) & 0x3F)));
        put(s, (unsigned char)(0x80 | ((cp >> 6) & 0x3F)));
        put(s, (unsigned char)(0x80 | (cp & 0x3F)));
    }
}

/* At an opening quote. Reads the string to its closing quote, decoding into
 * `s` (which may have no buffer, to skip). */
static json_err read_string(cursor *c, sink *s) {
    if (c->p >= c->end || *c->p != '"') return JSON_MALFORMED;
    c->p++;

    while (c->p < c->end) {
        unsigned char ch = (unsigned char)*c->p++;

        if (ch == '"') {
            if (s->out) s->out[s->len] = 0;
            return JSON_OK;
        }
        if (ch < 0x20) return JSON_MALFORMED; /* a raw control character is not allowed in a string */
        if (ch != '\\') {
            put(s, ch);
            continue;
        }

        if (c->p >= c->end) return JSON_MALFORMED;
        ch = (unsigned char)*c->p++;
        switch (ch) {
            case '"': put(s, '"'); break;
            case '\\': put(s, '\\'); break;
            case '/': put(s, '/'); break;
            case 'b': put(s, '\b'); break;
            case 'f': put(s, '\f'); break;
            case 'n': put(s, '\n'); break;
            case 'r': put(s, '\r'); break;
            case 't': put(s, '\t'); break;
            case 'u': {
                /* This one matters here: Jellyfin's serializer escapes '&'
                 * as &, and a TranscodingUrl is full of them. */
                unsigned cp;
                if (c->end - c->p < 4 || !hex4(c->p, &cp)) return JSON_MALFORMED;
                c->p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    unsigned lo;
                    if (c->end - c->p >= 6 && c->p[0] == '\\' && c->p[1] == 'u' && hex4(c->p + 2, &lo) &&
                        lo >= 0xDC00 && lo <= 0xDFFF) {
                        c->p += 6;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        cp = 0xFFFD; /* a lone surrogate names no character */
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                put_utf8(s, cp);
                break;
            }
            default: return JSON_MALFORMED;
        }
    }
    return JSON_MALFORMED; /* ran out before the closing quote */
}

static int is_literal(cursor *c, const char *word) {
    size_t n = strlen(word);
    if ((size_t)(c->end - c->p) < n || memcmp(c->p, word, n) != 0) return 0;
    c->p += n;
    return 1;
}

static json_err skip_value(cursor *c, int depth);

/* At '{' or '['. */
static json_err skip_container(cursor *c, int depth) {
    char close = (*c->p == '{') ? '}' : ']';
    int  is_object = (close == '}');
    int  first     = 1;

    if (depth >= JSON_MAX_DEPTH) return JSON_MALFORMED;
    c->p++;

    for (;;) {
        json_err e;

        skip_ws(c);
        if (c->p >= c->end) return JSON_MALFORMED;
        if (*c->p == close && first) {
            c->p++;
            return JSON_OK;
        }
        if (is_object) {
            sink none = {0};
            e = read_string(c, &none);
            if (e != JSON_OK) return e;
            skip_ws(c);
            if (c->p >= c->end || *c->p != ':') return JSON_MALFORMED;
            c->p++;
        }
        e = skip_value(c, depth + 1);
        if (e != JSON_OK) return e;
        first = 0;

        skip_ws(c);
        if (c->p >= c->end) return JSON_MALFORMED;
        if (*c->p == ',') {
            c->p++;
            continue;
        }
        if (*c->p == close) {
            c->p++;
            return JSON_OK;
        }
        return JSON_MALFORMED;
    }
}

static json_err skip_number(cursor *c) {
    const char *start = c->p;
    while (c->p < c->end && ((*c->p >= '0' && *c->p <= '9') || *c->p == '-' || *c->p == '+' || *c->p == '.' ||
                             *c->p == 'e' || *c->p == 'E'))
        c->p++;
    return c->p > start ? JSON_OK : JSON_MALFORMED;
}

static json_err skip_value(cursor *c, int depth) {
    skip_ws(c);
    if (c->p >= c->end) return JSON_MALFORMED;
    switch (*c->p) {
        case '"': {
            sink none = {0};
            return read_string(c, &none);
        }
        case '{':
        case '[': return skip_container(c, depth);
        case 't': return is_literal(c, "true") ? JSON_OK : JSON_MALFORMED;
        case 'f': return is_literal(c, "false") ? JSON_OK : JSON_MALFORMED;
        case 'n': return is_literal(c, "null") ? JSON_OK : JSON_MALFORMED;
        default: return skip_number(c);
    }
}

/* Leaves the cursor at the start of the value `path` names. */
static json_err find(cursor *c, const char *path, int depth) {
    const char *seg_end;
    size_t      seg_len;
    int         index = -1;
    int         i;

    skip_ws(c);
    if (!*path) return c->p < c->end ? JSON_OK : JSON_MALFORMED;
    if (depth >= JSON_MAX_DEPTH || c->p >= c->end) return JSON_MALFORMED;

    seg_end = strchr(path, '.');
    if (!seg_end) seg_end = path + strlen(path);
    seg_len = (size_t)(seg_end - path);

    if (seg_len > 0 && seg_len <= 6) {
        size_t k;
        index = 0;
        for (k = 0; k < seg_len; k++) {
            if (path[k] < '0' || path[k] > '9') {
                index = -1;
                break;
            }
            index = index * 10 + (path[k] - '0');
        }
    }

    if (*c->p == '{') {
        c->p++;
        skip_ws(c);
        if (c->p < c->end && *c->p == '}') return JSON_NOT_FOUND;
        for (;;) {
            char     key[JSON_KEY_MAX];
            sink     s = {key, sizeof key, 0, 0};
            json_err e;

            skip_ws(c);
            e = read_string(c, &s);
            if (e != JSON_OK) return e;
            skip_ws(c);
            if (c->p >= c->end || *c->p != ':') return JSON_MALFORMED;
            c->p++;

            if (!s.overflow && s.len == seg_len && memcmp(key, path, seg_len) == 0)
                return find(c, *seg_end ? seg_end + 1 : seg_end, depth + 1);

            e = skip_value(c, depth + 1);
            if (e != JSON_OK) return e;
            skip_ws(c);
            if (c->p >= c->end) return JSON_MALFORMED;
            if (*c->p == ',') {
                c->p++;
                continue;
            }
            if (*c->p == '}') return JSON_NOT_FOUND;
            return JSON_MALFORMED;
        }
    }

    if (*c->p == '[') {
        if (index < 0) return JSON_TYPE;
        c->p++;
        skip_ws(c);
        if (c->p < c->end && *c->p == ']') return JSON_NOT_FOUND;
        for (i = 0;; i++) {
            json_err e;

            if (i == index) return find(c, *seg_end ? seg_end + 1 : seg_end, depth + 1);
            e = skip_value(c, depth + 1);
            if (e != JSON_OK) return e;
            skip_ws(c);
            if (c->p >= c->end) return JSON_MALFORMED;
            if (*c->p == ',') {
                c->p++;
                continue;
            }
            if (*c->p == ']') return JSON_NOT_FOUND;
            return JSON_MALFORMED;
        }
    }

    /* A scalar where the path wants to go deeper. */
    return JSON_TYPE;
}

json_err json_get_string(const char *js, size_t len, const char *path, char *out, size_t cap) {
    cursor   c;
    sink     s;
    json_err e;

    if (!out || cap == 0) return JSON_TOOBIG;
    out[0] = 0;
    if (!js || !path) return JSON_NOT_FOUND;

    c.p   = js;
    c.end = js + len;
    e     = find(&c, path, 0);
    if (e != JSON_OK) return e;
    if (*c.p != '"') return JSON_TYPE;

    s.out      = out;
    s.cap      = cap;
    s.len      = 0;
    s.overflow = 0;
    e          = read_string(&c, &s);
    if (e == JSON_OK && s.overflow) e = JSON_TOOBIG;
    if (e != JSON_OK) out[0] = 0;
    return e;
}

json_err json_get_bool(const char *js, size_t len, const char *path, int *out) {
    cursor   c;
    json_err e;

    if (!js || !path || !out) return JSON_NOT_FOUND;
    c.p   = js;
    c.end = js + len;
    e     = find(&c, path, 0);
    if (e != JSON_OK) return e;
    if (is_literal(&c, "true")) *out = 1;
    else if (is_literal(&c, "false")) *out = 0;
    else return JSON_TYPE;
    return JSON_OK;
}

json_err json_get_span(const char *js, size_t len, const char *path, json_span *out) {
    cursor      c;
    json_err    e;
    const char *start;

    if (!js || !path || !out) return JSON_NOT_FOUND;
    out->p   = NULL;
    out->len = 0;
    c.p      = js;
    c.end    = js + len;
    e        = find(&c, path, 0);
    if (e != JSON_OK) return e;
    start = c.p;
    e     = skip_value(&c, 0);
    if (e != JSON_OK) return e;
    out->p   = start;
    out->len = (size_t)(c.p - start);
    return JSON_OK;
}

json_err json_array_next(const json_span *arr, size_t *pos, json_span *elem) {
    cursor      c;
    json_err    e;
    const char *start;

    if (!arr || !arr->p || !pos || !elem) return JSON_NOT_FOUND;
    c.p   = arr->p + *pos;
    c.end = arr->p + arr->len;

    if (*pos == 0) {
        skip_ws(&c);
        if (c.p >= c.end || *c.p != '[') return JSON_TYPE;
        c.p++;
        skip_ws(&c);
        if (c.p < c.end && *c.p == ']') {
            *pos = (size_t)(c.end - arr->p);
            return JSON_NOT_FOUND;
        }
    } else {
        skip_ws(&c);
        if (c.p >= c.end) return JSON_NOT_FOUND;
        if (*c.p == ']') return JSON_NOT_FOUND;
        if (*c.p != ',') return JSON_MALFORMED;
        c.p++;
    }

    skip_ws(&c);
    start = c.p;
    e     = skip_value(&c, 1);
    if (e != JSON_OK) return e;
    elem->p   = start;
    elem->len = (size_t)(c.p - start);
    *pos      = (size_t)(c.p - arr->p);
    return JSON_OK;
}

json_err json_get_int(const char *js, size_t len, const char *path, long long *out) {
    cursor             c;
    json_err           e;
    int                neg = 0;
    unsigned long long v   = 0;
    const char        *digits;

    if (!js || !path || !out) return JSON_NOT_FOUND;
    c.p   = js;
    c.end = js + len;
    e     = find(&c, path, 0);
    if (e != JSON_OK) return e;

    if (*c.p == '-') {
        neg = 1;
        c.p++;
    }
    digits = c.p;
    while (c.p < c.end && *c.p >= '0' && *c.p <= '9') {
        unsigned d = (unsigned)(*c.p - '0');
        if (v > (9223372036854775807ull - d) / 10ull) return JSON_TOOBIG;
        v = v * 10ull + d;
        c.p++;
    }
    if (c.p == digits) return JSON_TYPE;
    if (c.p < c.end && (*c.p == '.' || *c.p == 'e' || *c.p == 'E')) return JSON_TYPE;
    *out = neg ? -(long long)v : (long long)v;
    return JSON_OK;
}
