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
    volatile int idle;           /* nothing fetched until the first read says where */
    volatile int hold, held;     /* paused (connection closed) for another request */
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
        /* paused for another request (the 3DS's HTTP service starves concurrent
         * transfers), or not started yet: no connection open */
        if (s->hold || (s->idle && s->want == NO_WANT)) {
            LightLock_Unlock(&s->lock);
            if (st) { http_stream_close(st); st = NULL; }
            s->held = s->hold;
            LightLock_Lock(&s->lock);
            CondVar_WaitTimeout(&s->cv, &s->lock, 50000000LL);
            LightLock_Unlock(&s->lock);
            continue;
        }
        s->held = 0;
        if (s->want != NO_WANT) {
            s->idle = 0;
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
            dbg("stream: reading from %llu (%d)", (unsigned long long)pos, status);
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
        int near = !s->idle && off >= s->win_start && off <= s->win_end + AHEAD_REACH;
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
    s->idle = 1;
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

void httpsrc_hold(HttpSrc *s, int on) {
    if (!s) return;
    LightLock_Lock(&s->lock);
    s->hold = on;
    CondVar_Broadcast(&s->cv);
    LightLock_Unlock(&s->lock);
    /* wait (briefly) until the fetch thread has closed its connection */
    for (int i = 0; on && i < 150 && !s->held && !s->quit; i++) svcSleepThread(20000000LL);
}

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
