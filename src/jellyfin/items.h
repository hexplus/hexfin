/* Library items: one page of a Jellyfin list answer, read into fixed
 * records, and the line of text each one is shown as.
 *
 * Pure, like jellyfin/json.c: the answer is a server's, so it is read with
 * every bound checked, and the host build is where that is tested. */
#ifndef JELLYFIN_ITEMS_H
#define JELLYFIN_ITEMS_H

#include <stddef.h>
#include <stdint.h>

/* One page of a list. Small enough that the answer for it (about 1 kB an
 * item for Continue Watching, 450 bytes for a library) always fits the API
 * buffer, large enough that paging is not the whole experience. */
#define JF_PAGE_SIZE 50

#define JF_ITEM_ID_MAX   40
#define JF_ITEM_NAME_MAX 128
#define JF_ITEM_TYPE_MAX 24

typedef struct {
    char     id[JF_ITEM_ID_MAX];
    char     name[JF_ITEM_NAME_MAX];      /* UTF-8, as the server wrote it */
    char     series[JF_ITEM_NAME_MAX];    /* "" unless an episode */
    char     type[JF_ITEM_TYPE_MAX];      /* "Movie", "Episode", "Series", "CollectionFolder", ... */
    char     collection[JF_ITEM_TYPE_MAX]; /* a library's kind: "movies", "tvshows", ... or "" */
    int      is_folder;
    int      season;  /* ParentIndexNumber, -1 when absent */
    int      episode; /* IndexNumber, -1 when absent */
    int      year;    /* ProductionYear, 0 when absent */
    int      played;
    uint64_t runtime_ticks;  /* 10 000 000 a second; 0 when unknown */
    uint64_t position_ticks; /* where the user stopped; 0 from the start */
} jf_item;

/* Reads the "Items" array of a list answer into `out`, at most `max`, and
 * TotalRecordCount into `*total` (or the count read, when the answer has
 * none). Items without a usable id are skipped. Returns 0, or -1 when the
 * answer has no Items array to read. */
int jf_parse_items(const char *js, size_t len, jf_item *out, int max, int *count, int *total);

/* 1 for what this client can play, 0 for what it browses into or cannot
 * use at all. */
int jf_item_playable(const jf_item *it);
int jf_item_browsable(const jf_item *it);

/* 1 for a library this client has something to show from: video, and the
 * kinds of folder that may hold video. Music, books and photos are not. */
int jf_view_has_video(const jf_item *it);

/* The item's line in a list, UTF-8. `with_series` puts an episode's series
 * in front of it, for lists that mix series (Continue Watching). */
void jf_item_label(const jf_item *it, int with_series, char *out, size_t cap);

#endif /* JELLYFIN_ITEMS_H */
