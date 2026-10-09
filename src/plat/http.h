/* Plain-HTTP client over libctru httpc. */
#ifndef SHELF3DS_HTTP_H
#define SHELF3DS_HTTP_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int status;      /* HTTP status, or <0 for a transport error */
    char *data;      /* malloc'd, NUL-terminated */
    uint32_t len;
} HttpResp;

int http_init(void);
/* Offset (ms) to add to the console clock to get server/UTC time, learned
 * from HTTP Date headers. *known=false until the first response. */
long long http_clock_offset(bool *known);
void http_set_clock_offset(long long ms);
void http_exit(void);

/* headers: NULL-terminated array of "Name: value" strings (may be NULL).
 * Follows up to 3 redirects. Body capped at max_body (0 = 8 MB). Returns 0
 * when a response was received (check out->status), <0 on transport error. */
int http_request(const char *method, const char *url, const char *const *headers,
                 const void *body, uint32_t body_len, uint32_t max_body, HttpResp *out);
void http_resp_free(HttpResp *r);

/* Streaming GET with an optional Range starting at `from` (and ending at
 * `to` inclusive when to > from). */
typedef struct HttpStream HttpStream;
int http_stream_open(const char *url, const char *const *headers, uint64_t from, uint64_t to,
                     HttpStream **out, int *status, uint64_t *content_len);
/* Bytes read (>0), 0 at end of body, <0 on error. Waits at most timeout_ms. */
int http_stream_read(HttpStream *s, void *buf, uint32_t n, uint32_t timeout_ms);
void http_stream_close(HttpStream *s);

#endif
