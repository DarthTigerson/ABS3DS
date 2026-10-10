#include "httpsrc.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dbg.h"
#include "http.h"

#define RING_SIZE (1u << 20)
#define KEEP_BEHIND (96u * 1024)     /* bytes kept before the read point */
#define AHEAD_REACH (256u * 1024)    /* reads this far past the fill point wait */
#define CHUNK (32u * 1024)
#define NO_WANT UINT64_MAX

struct HttpSrc {
    char url[512];
    void (*auth)(char *, unsigned);
    uint64_t size;
    uint8_t *ring;
    uint64_t win_start, win_end; /* valid file range held in the ring */
    uint64_t rd;                 /* last read position */
    uint64_t want;               /* restart request (or NO_WANT) */
    volatile int quit, interrupted, state;
    LightLock lock;
    CondVar cv;
    Thread th;
    uint8_t chunk[CHUNK];
};

static void make_auth(void (*auth)(char *, unsigned), char *buf, unsigned n, const char **hdrs) {
    buf[0] = 0;
    if (auth) auth(buf, n);
    hdrs[0] = buf[0] ? buf : NULL;
    hdrs[1] = NULL;
}

static void fetch_thread(void *arg) {
    HttpSrc *s = arg;
    HttpStream *st = NULL;
    uint64_t pos = 0;
    int backoff = 0, stalls = 0;
    char auth[1100];
    const char *hdrs[2];
    while (!s->quit) {
        LightLock_Lock(&s->lock);
        if (s->want != NO_WANT) {
            pos = s->want;
            s->want = NO_WANT;
            s->win_start = s->win_end = pos;
            s->rd = pos;
            LightLock_Unlock(&s->lock);
            if (st) { http_stream_close(st); st = NULL; }
            backoff = 0;
            continue;
        }
        /* drop data far behind the reader to make room */
        uint64_t keep_from = s->rd > KEEP_BEHIND ? s->rd - KEEP_BEHIND : 0;
        if (keep_from > s->win_start) s->win_start = keep_from < s->win_end ? keep_from : s->win_end;
        uint64_t used = s->win_end - s->win_start;
        uint32_t space = RING_SIZE - (uint32_t)used;
        int at_end = s->size && s->win_end >= s->size;
        if (at_end || space < 4096) {
            if (at_end && s->state != HSRC_FAILED) s->state = HSRC_OK;
            CondVar_WaitTimeout(&s->cv, &s->lock, 50000000LL);
            LightLock_Unlock(&s->lock);
            if (at_end && st) { http_stream_close(st); st = NULL; }
            continue;
        }
        pos = s->win_end;
        LightLock_Unlock(&s->lock);

        if (!st) {
            int status = 0;
            uint64_t clen = 0;
            make_auth(s->auth, auth, sizeof auth, hdrs);
            if (http_stream_open(s->url, hdrs, pos, 0, &st, &status, &clen) != 0 || (status != 206 && status != 200) ||
                (status == 200 && pos != 0)) {
                if (st) { http_stream_close(st); st = NULL; }
                dbg("stream: open at %llu -> %d (%s)", (unsigned long long)pos, status, http_last_error());
                s->state = (status == 401 || status == 403 || status == 404) && backoff > 3 ? HSRC_FAILED : HSRC_RECONNECTING;
                int wait_ms = backoff < 4 ? 1000 << backoff : 10000;
                backoff++;
                for (int t = 0; t < wait_ms && !s->quit && s->want == NO_WANT; t += 50) svcSleepThread(50000000LL);
                continue;
            }
            if (!s->size && clen) s->size = (status == 206 ? pos : 0) + clen;
        }
        uint32_t n = space < CHUNK ? space : CHUNK;
        int k = http_stream_read(st, s->chunk, n, 4000);
        if (k == -2 && ++stalls >= 3) {
            /* ~12 s without a byte: the connection is dead even if it didn't say so */
            k = -1;
        }
        if (k > 0) stalls = 0;
        if (k < 0 && k != -2) {
            stalls = 0;
            http_stream_close(st);
            st = NULL;
            s->state = HSRC_RECONNECTING;
            continue; /* reopen at win_end on the next pass */
        }
        if (k == 0 && (!s->size || pos < s->size)) {
            /* server closed early: reconnect from where we are */
            http_stream_close(st);
            st = NULL;
            if (!s->size) { s->size = pos; }
            continue;
        }
        if (k <= 0) continue;
        backoff = 0;
        LightLock_Lock(&s->lock);
        if (s->want == NO_WANT && s->win_end == pos) {
            uint32_t at = (uint32_t)(pos % RING_SIZE);
            uint32_t first = RING_SIZE - at < (uint32_t)k ? RING_SIZE - at : (uint32_t)k;
            memcpy(s->ring + at, s->chunk, first);
            if ((uint32_t)k > first) memcpy(s->ring, s->chunk + first, k - first);
            s->win_end += (uint32_t)k;
            s->state = HSRC_OK;
            CondVar_Broadcast(&s->cv);
        }
        LightLock_Unlock(&s->lock);
    }
    if (st) http_stream_close(st);
}

static int src_read(void *ctx, uint64_t off, void *buf, uint32_t n) {
    HttpSrc *s = ctx;
    if (s->size && off >= s->size) return 0;
    LightLock_Lock(&s->lock);
    int rc;
    for (;;) {
        if (s->interrupted) { rc = HSRC_INTERRUPTED; break; }
        if (s->state == HSRC_FAILED) { rc = -1; break; }
        if (off >= s->win_start && off < s->win_end) {
            uint32_t avail = (uint32_t)(s->win_end - off);
            if (n > avail) n = avail;
            uint32_t at = (uint32_t)(off % RING_SIZE);
            uint32_t first = RING_SIZE - at < n ? RING_SIZE - at : n;
            memcpy(buf, s->ring + at, first);
            if (n > first) memcpy((uint8_t *)buf + first, s->ring, n - first);
            s->rd = off + n;
            CondVar_Broadcast(&s->cv);
            rc = (int)n;
            break;
        }
        if (s->size && off >= s->size) { rc = 0; break; }
        int near = off >= s->win_start && off <= s->win_end + AHEAD_REACH;
        if (!near && s->want != off) {
            s->want = off;
            CondVar_Broadcast(&s->cv);
        }
        if (s->state == HSRC_OK) s->state = HSRC_BUFFERING;
        CondVar_WaitTimeout(&s->cv, &s->lock, 100000000LL);
    }
    LightLock_Unlock(&s->lock);
    return rc;
}

HttpSrc *httpsrc_open(const char *url, uint64_t size, void (*auth)(char *, unsigned)) {
    HttpSrc *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->ring = malloc(RING_SIZE);
    if (!s->ring) { free(s); return NULL; }
    snprintf(s->url, sizeof s->url, "%s", url);
    s->auth = auth;
    s->size = size;
    s->want = NO_WANT;
    s->state = HSRC_BUFFERING;
    LightLock_Init(&s->lock);
    CondVar_Init(&s->cv);
    /* network threads live on the system core when available */
    s->th = threadCreate(fetch_thread, s, 16 * 1024, 0x2C, 1, false);
    if (!s->th) s->th = threadCreate(fetch_thread, s, 16 * 1024, 0x2C, 0, false);
    if (!s->th) { free(s->ring); free(s); return NULL; }
    return s;
}

Reader httpsrc_reader(HttpSrc *s) {
    Reader r = {s, src_read, s->size};
    return r;
}

void httpsrc_interrupt(HttpSrc *s, int on) {
    LightLock_Lock(&s->lock);
    s->interrupted = on;
    CondVar_Broadcast(&s->cv);
    LightLock_Unlock(&s->lock);
}

int httpsrc_state(HttpSrc *s) { return s->state; }

float httpsrc_fill(HttpSrc *s) {
    uint64_t ahead = s->win_end > s->rd ? s->win_end - s->rd : 0;
    return (float)ahead / (RING_SIZE - KEEP_BEHIND);
}

void httpsrc_close(HttpSrc *s) {
    if (!s) return;
    s->quit = 1;
    LightLock_Lock(&s->lock);
    CondVar_Broadcast(&s->cv);
    LightLock_Unlock(&s->lock);
    threadJoin(s->th, U64_MAX);
    threadFree(s->th);
    free(s->ring);
    free(s);
}

/* ---- range reader ---------------------------------------------------------- */

static int range_read(void *ctx, uint64_t off, void *buf, uint32_t n) {
    RangeReaderCtx *c = ctx;
    if (!(off >= c->cstart && off + n <= c->cstart + c->clen)) {
        char auth[1100];
        const char *hdrs[2];
        make_auth(c->auth, auth, sizeof auth, hdrs);
        HttpStream *st;
        int status;
        uint64_t clen;
        uint32_t want = n > sizeof c->cache ? n : sizeof c->cache;
        if (want > sizeof c->cache) want = sizeof c->cache;
        if (http_stream_open(c->url, hdrs, off, off + want - 1, &st, &status, &clen) != 0) { dbg("range %llu: %s", (unsigned long long)off, http_last_error()); return -1; }
        if (status != 206) { dbg("range %llu -> %d", (unsigned long long)off, status); http_stream_close(st); return -1; }
        uint32_t got = 0;
        while (got < want) {
            int k = http_stream_read(st, c->cache + got, want - got, 8000);
            if (k <= 0) break;
            got += (uint32_t)k;
        }
        http_stream_close(st);
        c->cstart = off;
        c->clen = got;
        if (!got) return 0;
    }
    uint32_t avail = (uint32_t)(c->cstart + c->clen - off);
    if (n > avail) n = avail;
    memcpy(buf, c->cache + (off - c->cstart), n);
    return (int)n;
}

int range_reader_open(Reader *r, RangeReaderCtx *c, const char *url, uint64_t size, void (*auth)(char *, unsigned)) {
    memset(c, 0, sizeof *c);
    snprintf(c->url, sizeof c->url, "%s", url);
    c->auth = auth;
    r->ctx = c;
    r->read_at = range_read;
    r->size = size;
    return 0;
}

int http_fetch_range_to_file(const char *url, void (*auth)(char *, unsigned), uint64_t from, uint64_t len, const char *path) {
    char auth_buf[1100];
    const char *hdrs[2];
    make_auth(auth, auth_buf, sizeof auth_buf, hdrs);
    HttpStream *st;
    int status;
    uint64_t clen;
    if (http_stream_open(url, hdrs, from, from + len - 1, &st, &status, &clen) != 0) { dbg("fetch %llu+%llu: %s", (unsigned long long)from, (unsigned long long)len, http_last_error()); return -1; }
    if (status != 206) { dbg("fetch %llu+%llu -> %d", (unsigned long long)from, (unsigned long long)len, status); http_stream_close(st); return -1; }
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { http_stream_close(st); return -1; }
    uint8_t *buf = malloc(CHUNK);
    uint64_t got = 0;
    int ok = buf != NULL;
    while (ok && got < len) {
        int k = http_stream_read(st, buf, CHUNK, 8000);
        if (k <= 0) break;
        if (fwrite(buf, 1, k, f) != (size_t)k) ok = 0;
        got += (uint32_t)k;
    }
    free(buf);
    fclose(f);
    http_stream_close(st);
    if (!ok || got != len) { remove(tmp); return -1; }
    remove(path);
    return rename(tmp, path);
}
