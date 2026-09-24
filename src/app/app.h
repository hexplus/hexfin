/* The player app: Wi-Fi, then sign-in, then the library as a list, and
 * playback of what is picked from it.
 *
 * SIGN-IN COMES FIRST. Nothing of the library is shown until the server has
 * accepted this console: with a kept token that is one request; without
 * one, the Quick Connect code fills the screen until someone enters it in a
 * signed-in Jellyfin app (jellyfin/jellyfin.h). A failure at either step
 * gets a screen that says what went wrong and offers to try again.
 *
 * THE LISTS: Continue Watching and the video libraries on the home screen,
 * folders opened with X and left with O, 50 entries to a page with L and R
 * to turn it. X on something playable plays it; during playback X or START
 * pauses and O stops, back to the same place in the list. HOME leaves from
 * anywhere.
 *
 * TEXT ONLY, by decision (2026-09-24): no cover images. Decoding pictures
 * would need their own buffers in a 24 MB partition whose largest free
 * block decides whether a film can start at all (docs/RESEARCH.md
 * section 6), and a list of names does the job.
 *
 * Only compiled for the console. */
#ifndef APP_APP_H
#define APP_APP_H

typedef struct {
    const char *version;
    const char *default_server; /* used when jellyfin.cfg names none */
    const char *cfg_path;
    int         ap_config;      /* net/wifi.h: 0 is the first stored profile */
    unsigned    wifi_timeout_ms;
} app_env;

/* Runs until HOME is pressed. The caller tears down afterwards. */
void app_run(const app_env *env);

#endif /* APP_APP_H */
