/* Talking to the Jellyfin server: signing in, listing the library, and
 * turning an item into a stream URL the rest of the pipeline can play.
 *
 * SIGNING IN is by Quick Connect, because a PSP has no keyboard worth typing
 * a password on (PROMPT.md section 14), and this server has it enabled
 * (docs/PHASE0_FINDINGS.md section 5). The PSP asks for a code, shows it,
 * and waits while a person enters it in any Jellyfin app they are already
 * signed in to. The token that comes back is kept in jellyfin.cfg next to
 * the EBOOT, so that happens once, not every run; a kept token the server
 * no longer accepts is thrown away and Quick Connect runs again.
 *
 * PLAYING asks POST /Items/{id}/PlaybackInfo with this client's device
 * profile (jellyfin/requests.c) and takes the TranscodingUrl it answers
 * with: progressive fragmented MP4, Constrained Baseline, AAC-LC, at most
 * 480x272 -- the shape the player plays.
 *
 * Only compiled for the console. Needs the radio up (net/wifi.h) and uses
 * http_fetch, so none of it may run while a stream is open. */
#ifndef JELLYFIN_JELLYFIN_H
#define JELLYFIN_JELLYFIN_H

#include "jellyfin/items.h"
#include "jellyfin/requests.h"

#include <stddef.h>

/* Called with a line or two for the screen while signing in: the Quick
 * Connect code, above all, which the person has to read off it. */
typedef void (*jf_show_fn)(const char *line1, const char *line2);

/* Reads jellyfin.cfg from `path`, which later saves go back to. A console
 * with no device id yet is given one here, and it is saved at once. */
void jf_config_load(const char *path);

/* The settings and sign-in as loaded; change them, then jf_config_save. */
jf_config *jf_config_get(void);
void       jf_config_save(void);

/* The server this client talks to: the config's, or `fallback` when the
 * config names none. */
const char *jf_server(const char *fallback);

/* Signs in to `server` ("http://host:port"), from the kept token when it
 * still works and by Quick Connect when it does not. Gives up when HOME is
 * pressed, and after five minutes of waiting for the code. Returns 0 when
 * signed in. */
int jf_sign_in(const char *server, const char *version, jf_show_fn show);

/* The signed-in user's name, or "". */
const char *jf_user_name(void);

/* Forgets the kept token -- telling the server, when it can be reached, so
 * the session goes from its list of devices too. The next jf_sign_in runs
 * Quick Connect. */
void jf_sign_out(void);

typedef enum {
    JF_LIST_VIEWS,    /* the user's libraries */
    JF_LIST_RESUME,   /* Continue Watching */
    JF_LIST_CHILDREN  /* what is inside `parent_id` */
} jf_list_kind;

/* One page of a list: at most `max` items from `start`, and the total the
 * server has. Returns 0 on success. */
int jf_list(jf_list_kind kind, const char *parent_id, int start, int max, jf_item *out, int *count, int *total);

/* Asks for item `item_id` in the PSP's shape, starting `start_ticks` into
 * it (0 for the beginning), and writes the URL to stream it from into
 * `url`. Also sets the Authorization header on source_http's
 * requests, so the stream is fetched as the signed-in user. The item's name
 * is left in jf_item_name() when the server gives one. Returns 0 on
 * success. */
int jf_open_item(const char *item_id, unsigned long long start_ticks, char *url, size_t cap);

/* Tells the server the last item opened is no longer being watched, so it
 * stops transcoding it. Best effort: a failure is only traced. */
void jf_stop_item(void);

/* Reports playback of the last item opened: started, still going (with
 * `event` "timeupdate", "pause" or "unpause"), or stopped at
 * `position_ticks`. This is what keeps Continue Watching and the resume
 * position up to date, and what marks an item watched. Returns 0 when the
 * server took it; a failure is traced and otherwise ignored -- a report is
 * never a reason to stop playing.
 *
 * Safe to call from a thread of its own during playback: it has its own
 * connection (net/http.h). Not from two threads at once. */
int jf_report(jf_report_kind kind, unsigned long long position_ticks, int paused, const char *event);

/* The item's name, or "". */
const char *jf_item_name(void);

/* The reason for the last failure, in words a person can act on. */
const char *jf_error(void);

#endif /* JELLYFIN_JELLYFIN_H */
