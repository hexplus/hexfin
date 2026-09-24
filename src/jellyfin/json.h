/* Reading the few values this client needs out of a Jellyfin answer.
 *
 * Not a JSON library. Nothing is built: no tree, no allocation, no copy of
 * the document. A lookup walks the text once, skipping every value that is
 * not on its path, and copies out the one it was asked for. A PlaybackInfo
 * answer is several kilobytes of media-stream detail of which this client
 * wants two strings, and a parser that built all of it would spend most of
 * the memory it used on things nobody reads.
 *
 * A PATH is keys joined by dots, with a number to index an array:
 * "MediaSources.0.TranscodingUrl", "User.Id". Keys match exactly -- Jellyfin
 * writes PascalCase and nothing else. The first member with a key wins.
 *
 * The text is a server's answer, so it is treated as hostile (PROMPT.md
 * section 35): every read is bounded by `len`, nesting is bounded by
 * JSON_MAX_DEPTH, and malformed input is refused rather than guessed at.
 * Nothing here includes a PSP header, which is what lets the host build fuzz
 * it under AddressSanitizer. */
#ifndef JELLYFIN_JSON_H
#define JELLYFIN_JSON_H

#include <stddef.h>

/* Deeper than any Jellyfin answer nests (about 6), shallow enough that the
 * recursion skipping a value cannot take the stack with it. */
#define JSON_MAX_DEPTH 32

typedef enum {
    JSON_OK = 0,
    JSON_NOT_FOUND, /* the document is fine; the path is not in it */
    JSON_TYPE,      /* the path is there, and holds another kind of value */
    JSON_TOOBIG,    /* the string does not fit in the caller's buffer */
    JSON_MALFORMED  /* the text is not JSON, or nests past JSON_MAX_DEPTH */
} json_err;

/* The string at `path`, unescaped (\uXXXX becomes UTF-8), NUL-terminated.
 * On anything but JSON_OK, out is "". */
json_err json_get_string(const char *js, size_t len, const char *path, char *out, size_t cap);

/* The true or false at `path`, as 1 or 0. */
json_err json_get_bool(const char *js, size_t len, const char *path, int *out);

/* The integer at `path`. Refused as JSON_TYPE when it has a fraction or an
 * exponent, and as JSON_TOOBIG past the range of long long. */
json_err json_get_int(const char *js, size_t len, const char *path, long long *out);

/* A value's own text, still in the document: what the lookups above take
 * as `js` to read inside it. */
typedef struct {
    const char *p;
    size_t      len;
} json_span;

/* The value at `path` ("" for the whole document), as a span. */
json_err json_get_span(const char *js, size_t len, const char *path, json_span *out);

/* Walks an array one element at a time, so a list of N items costs one
 * pass rather than N lookups that each start from the top. `arr` is the
 * array's span; `*pos` starts at 0 and is advanced by each call. Returns
 * JSON_OK with the next element in `elem`, JSON_NOT_FOUND after the last,
 * JSON_TYPE when `arr` is not an array. */
json_err json_array_next(const json_span *arr, size_t *pos, json_span *elem);

#endif /* JELLYFIN_JSON_H */
