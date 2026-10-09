#include "downloads.h"

#include <3ds.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "abs_client.h"
#include "abs_json.h"
#include "http.h"
#include "strutil.h"
#include "worker.h"

#define BOOKS_DIR "sdmc:/3ds/shelf3ds/books"
#define MAXDL 128

typedef struct {
    DlInfo info;
    BookDetail *job; /* tracks to fetch while queued/active */
} Entry;

static Entry g_e[MAXDL];
static int g_n;
static LightLock g_lock;
static Worker *g_dl;
static volatile bool g_cancel_flag;
static char g_cancel_id[40];

static void book_dir(const char *id, char *out, size_t n) { snprintf(out, n, BOOKS_DIR "/%s", id); }

void dl_track_path(const char *id, int i, const char *ext, char *out, size_t n) {
    snprintf(out, n, BOOKS_DIR "/%s/%02d%s", id, i + 1, ext);
}

static Entry *find(const char *id) {
    for (int i = 0; i < g_n; i++)
        if (!strcmp(g_e[i].info.id, id)) return &g_e[i];
    return NULL;
}

static uint64_t file_size(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 ? (uint64_t)st.st_size : 0;
}

static bool exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = n > 0 ? malloc(n + 1) : NULL;
    if (s && fread(s, 1, n, f) == (size_t)n) s[n] = 0;
    else { free(s); s = NULL; }
    fclose(f);
    return s;
}

int dl_load_meta(const char *id, BookDetail *out) {
    char p[160];
    snprintf(p, sizeof p, BOOKS_DIR "/%s/meta.json", id);
    char *j = slurp(p);
    if (!j) return -1;
    int rc = abs_parse_item(j, out);
    free(j);
    return rc;
}

static void scan(void) {
    DIR *d = opendir(BOOKS_DIR);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && g_n < MAXDL) {
        if (de->d_name[0] == '.') continue;
        BookDetail b;
        if (dl_load_meta(de->d_name, &b) != 0) continue;
        Entry *e = &g_e[g_n++];
        memset(e, 0, sizeof *e);
        str_copy(e->info.id, sizeof e->info.id, b.id[0] ? b.id : de->d_name);
        str_copy(e->info.title, sizeof e->info.title, b.title);
        str_copy(e->info.author, sizeof e->info.author, b.author);
        char p[192];
        snprintf(p, sizeof p, BOOKS_DIR "/%s/.done", de->d_name);
        e->info.state = exists(p) ? DL_DONE : DL_PAUSED;
        for (int i = 0; i < b.ntracks; i++) {
            e->info.total_bytes += b.tracks[i].size;
            char tp[192];
            dl_track_path(e->info.id, i, b.tracks[i].ext, tp, sizeof tp);
            uint64_t s = file_size(tp);
            if (!s) {
                char pp[200];
                snprintf(pp, sizeof pp, "%s.part", tp);
                s = file_size(pp);
            }
            e->info.done_bytes += s;
        }
        book_detail_free(&b);
    }
    closedir(d);
}

void dl_init(void) {
    LightLock_Init(&g_lock);
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/shelf3ds", 0777);
    mkdir(BOOKS_DIR, 0777);
    scan();
    g_dl = worker_create(0x31, 1, 32 * 1024);
}

void dl_exit(void) {
    g_cancel_flag = true;
    str_copy(g_cancel_id, sizeof g_cancel_id, "*");
    worker_destroy(g_dl);
    for (int i = 0; i < g_n; i++)
        if (g_e[i].job) { book_detail_free(g_e[i].job); free(g_e[i].job); }
}

/* ---- the download job -------------------------------------------------------- */

static void set_err(Entry *e, const char *msg) {
    LightLock_Lock(&g_lock);
    str_copy(e->info.error, sizeof e->info.error, msg);
    e->info.state = DL_PAUSED;
    LightLock_Unlock(&g_lock);
}

static bool cancelled(const char *id) {
    return g_cancel_flag && (!strcmp(g_cancel_id, id) || !strcmp(g_cancel_id, "*"));
}

/* 0 ok, -1 network, -2 disk full/write error, -3 cancelled */
static int fetch_track(Entry *e, const Track *t, int idx, uint8_t *buf, uint32_t bufsz) {
    char path[192], part[200], url[512], auth[1100];
    dl_track_path(e->info.id, idx, t->ext, path, sizeof path);
    if (exists(path)) return 0;
    snprintf(part, sizeof part, "%s.part", path);
    uint64_t have = file_size(part);
    if (absc_url(t->content_url, url, sizeof url) < 0) return -1;
    absc_auth_header(auth, sizeof auth);
    const char *hdrs[] = {auth, NULL};
    HttpStream *st;
    int status;
    uint64_t clen;
    if (http_stream_open(url, hdrs, have, 0, &st, &status, &clen) != 0) return -1;
    if (status == 401) {
        /* refresh the token through a cheap authenticated call, then retry once */
        http_stream_close(st);
        BookSummary dummy;
        absc_fetch_progress(&dummy, 0);
        absc_auth_header(auth, sizeof auth);
        if (http_stream_open(url, hdrs, have, 0, &st, &status, &clen) != 0) return -1;
    }
    if (status == 200) have = 0; /* server ignored Range: start over */
    else if (status != 206) { http_stream_close(st); return -1; }
    FILE *f = fopen(part, have ? "ab" : "wb");
    if (!f) { http_stream_close(st); return -2; }
    setvbuf(f, NULL, _IOFBF, 64 * 1024);
    int rc = 0;
    for (;;) {
        if (cancelled(e->info.id)) { rc = -3; break; }
        int k = http_stream_read(st, buf, bufsz, 10000);
        if (k == 0) break;
        if (k < 0) { rc = -1; break; }
        if (fwrite(buf, 1, k, f) != (size_t)k) { rc = -2; break; }
        LightLock_Lock(&g_lock);
        e->info.done_bytes += (uint64_t)k;
        LightLock_Unlock(&g_lock);
    }
    if (fclose(f) != 0 && rc == 0) rc = -2;
    http_stream_close(st);
    if (rc) return rc;
    if (t->size && file_size(part) < t->size) return -1; /* truncated */
    return rename(part, path) == 0 ? 0 : -2;
}

static void download_job(void *ud) {
    char *id = ud;
    LightLock_Lock(&g_lock);
    Entry *e = find(id);
    BookDetail *b = e ? e->job : NULL;
    if (e) { e->info.state = DL_ACTIVE; e->info.error[0] = 0; }
    LightLock_Unlock(&g_lock);
    if (!e || !b) { free(id); return; }
    uint8_t *buf = malloc(64 * 1024);
    int rc = buf ? 0 : -2;
    for (int i = 0; i < b->ntracks && rc == 0; i++) rc = fetch_track(e, &b->tracks[i], i, buf, 64 * 1024);
    free(buf);
    if (rc == 0) {
        char p[192];
        snprintf(p, sizeof p, BOOKS_DIR "/%s/.done", id);
        FILE *f = fopen(p, "w");
        if (f) fclose(f);
        LightLock_Lock(&g_lock);
        e->info.state = DL_DONE;
        e->info.done_bytes = e->info.total_bytes;
        LightLock_Unlock(&g_lock);
    } else if (rc == -3) {
        set_err(e, "Paused");
    } else if (rc == -2) {
        set_err(e, "SD card full or write error");
    } else {
        set_err(e, "Network error, retry to resume");
    }
    if (g_cancel_flag && !strcmp(g_cancel_id, id)) g_cancel_flag = false;
    LightLock_Lock(&g_lock);
    book_detail_free(b);
    free(b);
    e->job = NULL;
    LightLock_Unlock(&g_lock);
    free(id);
}

static BookDetail *clone_detail(const BookDetail *b) {
    BookDetail *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    *c = *b;
    c->description = NULL;
    c->tracks = malloc(sizeof(Track) * b->ntracks);
    c->chapters = NULL;
    c->nchapters = 0;
    if (!c->tracks) { free(c); return NULL; }
    memcpy(c->tracks, b->tracks, sizeof(Track) * b->ntracks);
    return c;
}

int dl_start(const BookDetail *b) {
    if (!b->ntracks) return -1;
    char dir[128];
    book_dir(b->id, dir, sizeof dir);
    mkdir(dir, 0777);
    char *json = abs_detail_to_json(b);
    if (!json) return -1;
    char p[160];
    snprintf(p, sizeof p, "%s/meta.json", dir);
    FILE *f = fopen(p, "wb");
    int ok = f && fputs(json, f) >= 0;
    if (f) fclose(f);
    free(json);
    if (!ok) return -2;
    LightLock_Lock(&g_lock);
    Entry *e = find(b->id);
    if (!e && g_n < MAXDL) {
        e = &g_e[g_n++];
        memset(e, 0, sizeof *e);
    }
    if (!e) { LightLock_Unlock(&g_lock); return -1; }
    if (e->info.state == DL_QUEUED || e->info.state == DL_ACTIVE) { LightLock_Unlock(&g_lock); return 0; }
    str_copy(e->info.id, sizeof e->info.id, b->id);
    str_copy(e->info.title, sizeof e->info.title, b->title);
    str_copy(e->info.author, sizeof e->info.author, b->author);
    e->info.total_bytes = 0;
    for (int i = 0; i < b->ntracks; i++) e->info.total_bytes += b->tracks[i].size;
    e->info.state = DL_QUEUED;
    e->info.error[0] = 0;
    e->job = clone_detail(b);
    LightLock_Unlock(&g_lock);
    if (!e->job) return -1;
    return worker_post(g_dl, download_job, strdup(b->id)) ? 0 : -1;
}

void dl_cancel(const char *id) {
    str_copy(g_cancel_id, sizeof g_cancel_id, id);
    g_cancel_flag = true;
}

static void rm_tree(const char *dir) {
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char p[300];
            snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
            remove(p);
        }
        closedir(d);
    }
    rmdir(dir);
}

int dl_delete(const char *id) {
    LightLock_Lock(&g_lock);
    Entry *e = find(id);
    bool busy = e && (e->info.state == DL_ACTIVE || e->info.state == DL_QUEUED);
    LightLock_Unlock(&g_lock);
    if (busy) {
        dl_cancel(id);
        for (int i = 0; i < 100 && dl_state(id, NULL) == DL_ACTIVE; i++) svcSleepThread(20000000LL);
    }
    char dir[128];
    book_dir(id, dir, sizeof dir);
    rm_tree(dir);
    LightLock_Lock(&g_lock);
    e = find(id);
    if (e && !e->job) {
        int i = (int)(e - g_e);
        memmove(&g_e[i], &g_e[i + 1], sizeof(Entry) * (g_n - i - 1));
        g_n--;
    }
    LightLock_Unlock(&g_lock);
    return 0;
}

DlState dl_state(const char *id, float *pct) {
    LightLock_Lock(&g_lock);
    Entry *e = find(id);
    DlState s = e ? e->info.state : DL_NONE;
    if (pct) *pct = e && e->info.total_bytes ? (float)((double)e->info.done_bytes / e->info.total_bytes) : 0;
    LightLock_Unlock(&g_lock);
    return s;
}

bool dl_is_done(const char *id) { return dl_state(id, NULL) == DL_DONE; }

int dl_list(DlInfo *out, int max) {
    LightLock_Lock(&g_lock);
    int n = g_n < max ? g_n : max;
    for (int i = 0; i < n; i++) out[i] = g_e[i].info;
    LightLock_Unlock(&g_lock);
    return n;
}

uint64_t dl_total_bytes(void) {
    LightLock_Lock(&g_lock);
    uint64_t t = 0;
    for (int i = 0; i < g_n; i++) t += g_e[i].info.done_bytes;
    LightLock_Unlock(&g_lock);
    return t;
}
