#include "http.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UA "ABS3DS/" ABS3DS_VERSION " (Nintendo 3DS)"

struct HttpStream {
    httpcContext ctx;
    bool done;
};

static volatile long long g_clock_offset;
static volatile bool g_clock_known;
static volatile bool g_clock_fresh; /* learned during this run */

long long http_clock_offset(bool *known) {
    if (known) *known = g_clock_known;
    return g_clock_offset;
}

void http_set_clock_offset(long long ms) {
    g_clock_offset = ms;
    g_clock_known = true;
}

/* "Fri, 09 Oct 2026 17:01:02 GMT" -> unix seconds (0 on failure) */
static long long parse_http_date(const char *s) {
    static const char *mon = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char m[4] = {0};
    int d, y, hh, mm, ss;
    const char *comma = strchr(s, ',');
    if (!comma || sscanf(comma + 1, " %d %3s %d %d:%d:%d", &d, m, &y, &hh, &mm, &ss) != 6) return 0;
    const char *f = strstr(mon, m);
    if (!f) return 0;
    int mo = (int)(f - mon) / 3 + 1;
    /* days from civil (Howard Hinnant) */
    y -= mo <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + (long long)doe - 719468;
    return days * 86400 + hh * 3600 + mm * 60 + ss;
}

static void learn_clock(httpcContext *c) {
    if (g_clock_fresh) return;
    char date[64] = {0};
    if (R_FAILED(httpcGetResponseHeader(c, "Date", date, sizeof date)) || !date[0]) return;
    long long server = parse_http_date(date);
    if (!server) return;
    long long local = (long long)osGetTime() - 2208988800000LL;
    g_clock_offset = server * 1000 - local;
    g_clock_known = true;
    g_clock_fresh = true;
}

int http_init(void) {
    return R_SUCCEEDED(httpcInit(0)) ? 0 : -1;
}

void http_exit(void) {
    httpcExit();
}

static int add_headers(httpcContext *c, const char *const *headers) {
    httpcAddRequestHeaderField(c, "User-Agent", UA);
    httpcAddRequestHeaderField(c, "Connection", "Keep-Alive");
    for (int i = 0; headers && headers[i]; i++) {
        const char *colon = strchr(headers[i], ':');
        if (!colon) continue;
        char name[64];
        size_t nl = (size_t)(colon - headers[i]);
        if (nl >= sizeof name) continue;
        memcpy(name, headers[i], nl);
        name[nl] = 0;
        const char *v = colon + 1;
        while (*v == ' ') v++;
        httpcAddRequestHeaderField(c, name, v);
    }
    return 0;
}

/* Opens + sends a request; on success the context is open and *status set. */
static int begin(httpcContext *c, HTTPC_RequestMethod m, const char *url, const char *const *headers,
                 const void *body, uint32_t body_len, const char *range, u32 *status) {
    if (R_FAILED(httpcOpenContext(c, m, url, 0))) return -1;
    httpcSetSSLOpt(c, SSLCOPT_DisableVerify);
    httpcSetKeepAlive(c, HTTPC_KEEPALIVE_ENABLED);
    add_headers(c, headers);
    if (range) httpcAddRequestHeaderField(c, "Range", range);
    if (body && body_len) {
        httpcAddRequestHeaderField(c, "Content-Type", "application/json");
        /* httpcAddPostDataRaw wants a u32-aligned buffer */
        u32 *aligned = malloc((body_len + 3) & ~3u);
        if (!aligned) { httpcCloseContext(c); return -1; }
        memcpy(aligned, body, body_len);
        Result r = httpcAddPostDataRaw(c, aligned, body_len);
        free(aligned);
        if (R_FAILED(r)) { httpcCloseContext(c); return -1; }
    }
    if (R_FAILED(httpcBeginRequest(c)) || R_FAILED(httpcGetResponseStatusCodeTimeout(c, status, 15000000000ULL))) {
        httpcCloseContext(c);
        return -1;
    }
    return 0;
}

static HTTPC_RequestMethod method_of(const char *m) {
    if (!strcmp(m, "POST")) return HTTPC_METHOD_POST;
    if (!strcmp(m, "PUT")) return HTTPC_METHOD_PUT;
    if (!strcmp(m, "DELETE")) return HTTPC_METHOD_DELETE;
    if (!strcmp(m, "HEAD")) return HTTPC_METHOD_HEAD;
    return HTTPC_METHOD_GET;
}

int http_request(const char *method, const char *url, const char *const *headers,
                 const void *body, uint32_t body_len, uint32_t max_body, HttpResp *out) {
    memset(out, 0, sizeof *out);
    if (!max_body) max_body = 8u << 20;
    char cur[512];
    snprintf(cur, sizeof cur, "%s", url);
    for (int hop = 0; hop < 4; hop++) {
        httpcContext c;
        u32 status = 0;
        if (begin(&c, method_of(method), cur, headers, body, body_len, NULL, &status) != 0) {
            out->status = -1;
            return -1;
        }
        if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) && hop < 3) {
            char loc[512] = {0};
            httpcGetResponseHeader(&c, "Location", loc, sizeof loc);
            httpcCloseContext(&c);
            if (!loc[0]) { out->status = (int)status; return 0; }
            if (loc[0] == '/') {
                /* absolute path: keep scheme://host:port */
                const char *p = strstr(cur, "://");
                const char *slash = p ? strchr(p + 3, '/') : NULL;
                size_t hl = slash ? (size_t)(slash - cur) : strlen(cur);
                char tmp[512];
                snprintf(tmp, sizeof tmp, "%.*s%s", (int)hl, cur, loc);
                snprintf(cur, sizeof cur, "%s", tmp);
            } else {
                snprintf(cur, sizeof cur, "%s", loc);
            }
            if (status == 303) { body = NULL; body_len = 0; method = "GET"; }
            continue;
        }
        learn_clock(&c);
        uint32_t cap = 16 * 1024, len = 0;
        char *buf = malloc(cap + 1);
        Result r = 0;
        while (buf) {
            if (len == cap) {
                if (cap >= max_body) break;
                cap = cap * 2 > max_body ? max_body : cap * 2;
                char *n = realloc(buf, cap + 1);
                if (!n) { free(buf); buf = NULL; break; }
                buf = n;
            }
            u32 before = 0, after = 0;
            httpcGetDownloadSizeState(&c, &before, NULL);
            r = httpcReceiveDataTimeout(&c, (u8 *)buf + len, cap - len, 15000000000ULL);
            httpcGetDownloadSizeState(&c, &after, NULL);
            len += after - before;
            if (r != (Result)HTTPC_RESULTCODE_DOWNLOADPENDING) break;
        }
        httpcCloseContext(&c);
        if (!buf || (R_FAILED(r) && r != (Result)HTTPC_RESULTCODE_DOWNLOADPENDING)) {
            free(buf);
            out->status = -2;
            return -1;
        }
        buf[len] = 0;
        out->status = (int)status;
        out->data = buf;
        out->len = len;
        return 0;
    }
    out->status = -3;
    return -1;
}

void http_resp_free(HttpResp *r) {
    free(r->data);
    r->data = NULL;
    r->len = 0;
}

int http_stream_open(const char *url, const char *const *headers, uint64_t from, uint64_t to,
                     HttpStream **out, int *status, uint64_t *content_len) {
    HttpStream *s = calloc(1, sizeof *s);
    if (!s) return -1;
    char range[64];
    const char *rp = NULL;
    if (to > from) { snprintf(range, sizeof range, "bytes=%llu-%llu", (unsigned long long)from, (unsigned long long)to); rp = range; }
    else if (from) { snprintf(range, sizeof range, "bytes=%llu-", (unsigned long long)from); rp = range; }
    u32 st = 0;
    if (begin(&s->ctx, HTTPC_METHOD_GET, url, headers, NULL, 0, rp, &st) != 0) {
        free(s);
        return -1;
    }
    *status = (int)st;
    if (content_len) {
        u32 total = 0;
        httpcGetDownloadSizeState(&s->ctx, NULL, &total);
        *content_len = total;
    }
    *out = s;
    return 0;
}

int http_stream_read(HttpStream *s, void *buf, uint32_t n, uint32_t timeout_ms) {
    if (s->done) return 0;
    u32 before = 0, after = 0;
    httpcGetDownloadSizeState(&s->ctx, &before, NULL);
    Result r = httpcReceiveDataTimeout(&s->ctx, buf, n, (u64)timeout_ms * 1000000ULL);
    httpcGetDownloadSizeState(&s->ctx, &after, NULL);
    int got = (int)(after - before);
    if (r == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING) return got;
    if (r == (Result)HTTPC_RESULTCODE_TIMEDOUT) return got > 0 ? got : -2;
    if (R_FAILED(r)) return got > 0 ? got : -1;
    s->done = true; /* finished: whatever arrived is the tail */
    return got;
}

void http_stream_close(HttpStream *s) {
    if (!s) return;
    httpcCloseContext(&s->ctx);
    free(s);
}
