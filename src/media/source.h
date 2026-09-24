/* Where the probe's bytes come from, with the answer left open.
 *
 * Until now the probe read a file on the memory stick, and sceIoOpen /
 * sceIoRead were written straight into main.c. That was fine while there was
 * one answer; there are now two, and they fail in completely different ways.
 * The file path is kept -- deliberately, permanently -- because it is the
 * only way to tell a decode regression apart from a network one: the same
 * fixture through the same parser and the same Media Engine, with the radio
 * out of the picture entirely. A network client that can only be tested
 * against a network is a client whose every failure looks the same.
 *
 * The interface is a vtable rather than the free functions the other seams
 * here use (video_decoder.h, audio_decoder.h), because unlike those, BOTH
 * implementations are linked into the same EBOOT and the choice is made at
 * run time. Each implementation still keeps its state in module statics and
 * supports one open stream at a time, exactly like those seams do -- the
 * vtable selects an implementation, it does not make them instantiable.
 *
 * No platform headers, so the host check build can compile a caller.
 *
 * Errors return a code and leave a readable reason behind (PROMPT.md section
 * 47), the same contract video_decoder_error() has: the code is what the
 * caller branches on, the string is what a person reads off the screen. */
#ifndef MEDIA_SOURCE_H
#define MEDIA_SOURCE_H

#include <stdint.h>

typedef enum {
    SOURCE_OK = 0,
    SOURCE_ERR_STATE,     /* called out of order: read before open, open twice */
    SOURCE_ERR_ARG,       /* the caller's arguments cannot be right */
    SOURCE_ERR_OPEN,      /* the stream could not be started at all */
    SOURCE_ERR_IO,        /* it was open and then something went wrong */
    SOURCE_ERR_EOF,       /* the stream ended cleanly. NOT a failure */
    SOURCE_ERR_CANCELLED  /* a stop request interrupted the read. NOT a failure either */
} source_err;

typedef struct {
    /* One word, for the screen: the probe reports which source it used
     * rather than leaving the reader to infer it from the location. */
    const char *name;

    /* `location` is whatever this implementation takes -- a path for the
     * file source, a URL for the HTTP one. Returns SOURCE_OK, or a code with
     * the reason in error(). */
    int (*open)(const char *location);

    /* Fills at most `cap` bytes and reports how many through `out_got`.
     *
     * A SHORT READ IS NORMAL AND IS NOT AN ERROR: a socket hands over
     * whatever arrived, and a caller that treats less than `cap` as a
     * failure would fail on nearly every packet. The contract that matters
     * is the other way round -- SOURCE_OK always means out_got > 0, so a
     * caller looping until it has enough bytes cannot spin forever against a
     * source that keeps saying "fine, nothing".
     *
     * SOURCE_ERR_EOF means the stream is over and nothing more will ever
     * arrive; SOURCE_ERR_CANCELLED means a stop was requested mid-read
     * (PROMPT.md section 29 -- a blocked socket must never hard-lock the
     * console). Neither is a fault, and both leave out_got at 0. */
    int (*read)(uint8_t *dst, uint32_t cap, uint32_t *out_got);

    /* Releases everything open() claimed. Safe when open failed or was never
     * called: design section 3.4's teardown runs after failures too. */
    void (*close)(void);

    /* The reason for the last failure, in words a person can act on, or ""
     * if nothing has failed. Valid until the next call into the source. */
    const char *(*error)(void);
} media_source;

/* The memory stick, through sceIoOpen/sceIoRead. media/source_file.c. */
extern const media_source source_file;

#endif /* MEDIA_SOURCE_H */
