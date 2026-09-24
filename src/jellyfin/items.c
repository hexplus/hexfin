/* See jellyfin/items.h. */
#include "jellyfin/items.h"

#include "jellyfin/json.h"
#include "jellyfin/requests.h"

#include <stdio.h>
#include <string.h>

static int get_int(const json_span *s, const char *path, int dflt) {
    long long v;
    if (json_get_int(s->p, s->len, path, &v) != JSON_OK || v < -1000000 || v > 1000000) return dflt;
    return (int)v;
}

static uint64_t get_ticks(const json_span *s, const char *path) {
    long long v;
    if (json_get_int(s->p, s->len, path, &v) != JSON_OK || v < 0) return 0;
    return (uint64_t)v;
}

/* A string that is too long is cut rather than dropped: a name is for a
 * person to read, and most of one is better than none. json_get_string
 * refuses instead of cutting, so the cut happens here, on a character
 * boundary. */
static void get_text(const json_span *s, const char *path, char *out, size_t cap) {
    static char big[1024];

    if (json_get_string(s->p, s->len, path, out, cap) == JSON_OK) return;
    out[0] = 0;
    if (json_get_string(s->p, s->len, path, big, sizeof big) == JSON_OK) {
        size_t n = cap - 1;
        while (n > 0 && ((unsigned char)big[n] & 0xC0) == 0x80) n--; /* not inside a UTF-8 sequence */
        memcpy(out, big, n);
        out[n] = 0;
    }
}

int jf_parse_items(const char *js, size_t len, jf_item *out, int max, int *count, int *total) {
    json_span arr, el;
    size_t    pos = 0;
    int       n   = 0;
    long long t;

    *count = 0;
    *total = 0;
    if (json_get_span(js, len, "Items", &arr) != JSON_OK) return -1;

    while (n < max && json_array_next(&arr, &pos, &el) == JSON_OK) {
        jf_item *it = &out[n];
        int      b  = 0;

        memset(it, 0, sizeof *it);
        if (json_get_string(el.p, el.len, "Id", it->id, sizeof it->id) != JSON_OK || !jf_id_is_safe(it->id)) continue;
        get_text(&el, "Name", it->name, sizeof it->name);
        get_text(&el, "SeriesName", it->series, sizeof it->series);
        json_get_string(el.p, el.len, "Type", it->type, sizeof it->type);
        json_get_string(el.p, el.len, "CollectionType", it->collection, sizeof it->collection);
        if (json_get_bool(el.p, el.len, "IsFolder", &b) == JSON_OK) it->is_folder = b;
        it->season         = get_int(&el, "ParentIndexNumber", -1);
        it->episode        = get_int(&el, "IndexNumber", -1);
        it->year           = get_int(&el, "ProductionYear", 0);
        it->runtime_ticks  = get_ticks(&el, "RunTimeTicks");
        it->position_ticks = get_ticks(&el, "UserData.PlaybackPositionTicks");
        if (json_get_bool(el.p, el.len, "UserData.Played", &b) == JSON_OK) it->played = b;
        n++;
    }

    *count = n;
    *total = (json_get_int(js, len, "TotalRecordCount", &t) == JSON_OK && t >= n && t < 10000000) ? (int)t : n;
    return 0;
}

static int type_is(const jf_item *it, const char *t) { return strcmp(it->type, t) == 0; }

int jf_item_playable(const jf_item *it) {
    return !it->is_folder && (type_is(it, "Movie") || type_is(it, "Episode") || type_is(it, "Video") ||
                              type_is(it, "MusicVideo") || type_is(it, "Trailer"));
}

int jf_item_browsable(const jf_item *it) { return it->is_folder; }

int jf_view_has_video(const jf_item *it) {
    static const char *const no[] = {"music", "books", "photos", "playlists", "livetv", "audiobooks"};
    unsigned i;
    for (i = 0; i < sizeof no / sizeof no[0]; i++)
        if (strcmp(it->collection, no[i]) == 0) return 0;
    return 1;
}

void jf_item_label(const jf_item *it, int with_series, char *out, size_t cap) {
    if (!cap) return;
    if (type_is(it, "Episode")) {
        char num[24] = "";
        if (it->season >= 0 && it->episode >= 0) snprintf(num, sizeof num, "S%dE%d ", it->season, it->episode);
        else if (it->episode >= 0) snprintf(num, sizeof num, "E%d ", it->episode);
        if (with_series && it->series[0]) snprintf(out, cap, "%s - %s%s", it->series, num, it->name);
        else snprintf(out, cap, "%s%s", num, it->name);
    } else if ((type_is(it, "Movie") || type_is(it, "Series")) && it->year > 0) {
        snprintf(out, cap, "%s (%d)", it->name, it->year);
    } else {
        snprintf(out, cap, "%s", it->name[0] ? it->name : "(untitled)");
    }
}
