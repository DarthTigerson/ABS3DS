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
/* Pauses fetching (closing the connection) while another request runs, and
 * resumes from where it was. Blocks up to ~3 s until the connection is closed. */
void httpsrc_hold(HttpSrc *s, int on);
/* Fraction of the read-ahead ring that is filled, 0..1 */
float httpsrc_fill(HttpSrc *s);

#endif
