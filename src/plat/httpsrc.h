/* Streaming HTTP byte source: a ring buffer filled by a background thread
 * with Range requests. Reads near the fill point wait; far reads restart the
 * request at the new offset. Survives Wi-Fi drops with retry/backoff. */
#ifndef SHELF3DS_HTTPSRC_H
#define SHELF3DS_HTTPSRC_H
#include <stdint.h>

#include "reader.h"

enum { HSRC_OK = 0, HSRC_BUFFERING = 1, HSRC_RECONNECTING = 2, HSRC_FAILED = 3 };
#define HSRC_INTERRUPTED (-3)

typedef struct HttpSrc HttpSrc;

/* url: absolute; size: total file size (0 = learn it from the response).
 * auth: callback that writes the current "Authorization: ..." header. */
HttpSrc *httpsrc_open(const char *url, uint64_t size, void (*auth)(char *out, unsigned n));
Reader httpsrc_reader(HttpSrc *s);
void httpsrc_close(HttpSrc *s);
/* Makes blocked/future reads return HSRC_INTERRUPTED until cleared. */
void httpsrc_interrupt(HttpSrc *s, int on);
int httpsrc_state(HttpSrc *s);
/* Fraction of the read-ahead ring that is filled, 0..1 */
float httpsrc_fill(HttpSrc *s);

/* Reader over individual Range requests with a 4 KB cache: cheap probing of
 * remote files (e.g. walking MP4 top-level atoms to find the moov). */
typedef struct {
    char url[512];
    void (*auth)(char *out, unsigned n);
    uint8_t cache[4096];
    uint64_t cstart;
    uint32_t clen;
} RangeReaderCtx;
int range_reader_open(Reader *r, RangeReaderCtx *c, const char *url, uint64_t size, void (*auth)(char *, unsigned));
/* Downloads bytes [from, from+len) to path. 0 ok. */
int http_fetch_range_to_file(const char *url, void (*auth)(char *, unsigned), uint64_t from, uint64_t len, const char *path);

#endif
