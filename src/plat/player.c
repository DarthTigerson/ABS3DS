#include "player.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abs_client.h"
#include "downloads.h"
#include "engine.h"
#include "httpsrc.h"
#include "strutil.h"
#include "sys.h"
#include "timeline.h"
#include "worker.h"

static Config *g_cfg;
static ProgressQueue *g_pq;
static void (*g_save_queue)(void);

static struct {
    LightLock lock;
    volatile PlayerPhase phase;
    char error[96];
    BookDetail book;        /* owned, valid when phase == PL_READY */
    bool has_book;
    bool local;
    char session[48];       /* ABS play session id ("" when offline) */
    SyncPolicy sync;
    SleepTimer sleep;
    float speed;
    double last_queue_save;
    bool was_playing;
    bool paused_by_lid;
    bool lid_was_closed;
    int open_gen;           /* ignores stale open results */
    u64 last_flush;
} P;

/* ---- open ------------------------------------------------------------------ */

typedef struct {
    int gen;
    BookSummary hint;
} OpenJob;

typedef struct {
    int gen;
    int rc;
    char msg[96];
    BookDetail book;
    char session[48];
    double start;
    bool local;
} OpenResult;

static OpenResult *g_open_result;
static struct { int gen; char id[48]; bool ready; } g_late_session;

static void auth_cb(char *out, unsigned n) { absc_auth_header(out, n); }

static double resume_position(const char *id, double server_t, uint64_t server_updated) {
    const QEntry *q = pq_get(g_pq, id);
    if (q && (q->updated_ms > server_updated || server_updated == 0) && (q->pending || server_t <= 0)) return q->current_time;
    return server_t;
}

static void open_job(void *ud) {
    OpenJob *j = ud;
    OpenResult *r = calloc(1, sizeof *r);
    if (!r) { free(j); return; }
    r->gen = j->gen;
    const char *id = j->hint.id;
    bool downloaded = dl_is_done(id);
    PlaySession s;
    int prc = ABS_ERR_NET;
    if (downloaded) {
        /* play from the SD card at once; the server session is attached later */
        r->local = true;
        if (dl_load_meta(id, &r->book) != 0) { r->rc = -1; str_copy(r->msg, sizeof r->msg, "Download is damaged"); }
        r->start = resume_position(id, j->hint.current_time, j->hint.progress_updated);
        if (r->rc == 0 && r->book.duration > 0 && r->start > r->book.duration - 5) r->start = 0;
        int gen = r->gen;
        LightLock_Lock(&P.lock);
        if (g_open_result) { book_detail_free(&g_open_result->book); free(g_open_result); }
        g_open_result = r;
        LightLock_Unlock(&P.lock);
        if (absc_have_session() && absc_play(id, sys_model(), &s) == ABS_OK) {
            LightLock_Lock(&P.lock);
            g_late_session.gen = gen;
            str_copy(g_late_session.id, sizeof g_late_session.id, s.id);
            g_late_session.ready = true;
            LightLock_Unlock(&P.lock);
            book_detail_free(&s.book);
        }
        free(j);
        return;
    }
    if (absc_have_session()) prc = absc_play(id, sys_model(), &s);
    if (prc == ABS_OK) {
        r->book = s.book;
        str_copy(r->session, sizeof r->session, s.id);
        r->start = resume_position(id, s.current_time, j->hint.progress_updated);
    } else {
        r->rc = prc;
        str_copy(r->msg, sizeof r->msg, prc == ABS_ERR_AUTH ? "Please log in again" :
                 prc == ABS_ERR_NET ? "Can't reach the server" : "Server error");
    }
    if (r->rc == 0 && r->book.duration > 0 && r->start > r->book.duration - 5) r->start = 0; /* finished -> restart */
    LightLock_Lock(&P.lock);
    if (g_open_result) { book_detail_free(&g_open_result->book); free(g_open_result); }
    g_open_result = r;
    LightLock_Unlock(&P.lock);
    free(j);
}

static void close_session_async(void);

void player_open(const BookSummary *b) {
    if (P.has_book && !strcmp(P.book.id, b->id) && P.phase == PL_READY) return; /* already open */
    if (P.has_book) {
        engine_stop();
        close_session_async();
    }
    OpenJob *j = calloc(1, sizeof *j);
    if (!j) return;
    j->gen = ++P.open_gen;
    j->hint = *b;
    P.phase = PL_OPENING;
    P.error[0] = 0;
    worker_post(g_api, open_job, j);
}

static void build_sources(const OpenResult *r, PlayableBook *pb) {
    pb->ntracks = r->book.ntracks;
    pb->tracks = r->book.tracks;
    pb->duration = r->book.duration;
    pb->sources = calloc(r->book.ntracks, sizeof(TrackSource));
    for (int i = 0; i < r->book.ntracks && pb->sources; i++) {
        const Track *t = &r->book.tracks[i];
        TrackSource *s = &pb->sources[i];
        s->local = r->local;
        s->size = t->size;
        str_copy(s->ext, sizeof s->ext, t->ext);
        str_copy(s->codec, sizeof s->codec, t->codec);
        snprintf(s->cache_key, sizeof s->cache_key, "%s_%s", r->book.id, t->ino);
        if (r->local) dl_track_path(r->book.id, i, t->ext, s->path, sizeof s->path);
        else absc_url(t->content_url, s->url, sizeof s->url);
    }
}

static void take_late_session(void) {
    LightLock_Lock(&P.lock);
    if (g_late_session.ready && !g_open_result) {
        g_late_session.ready = false;
        if (g_late_session.gen == P.open_gen && P.has_book) str_copy(P.session, sizeof P.session, g_late_session.id);
    }
    LightLock_Unlock(&P.lock);
}

static void take_open_result(void) {
    LightLock_Lock(&P.lock);
    OpenResult *r = g_open_result;
    g_open_result = NULL;
    LightLock_Unlock(&P.lock);
    if (!r) return;
    if (r->gen != P.open_gen) { book_detail_free(&r->book); free(r); return; }
    if (r->rc != 0) {
        P.phase = PL_FAILED;
        str_copy(P.error, sizeof P.error, r->msg);
        free(r);
        return;
    }
    if (!engine_available()) {
        P.phase = PL_FAILED;
        str_copy(P.error, sizeof P.error, "No audio: run DSP1 once to dump the DSP firmware");
        book_detail_free(&r->book);
        free(r);
        return;
    }
    bool supported = r->book.ntracks > 0;
    for (int i = 0; i < r->book.ntracks; i++)
        if (!stream_supported(r->book.tracks[i].ext, r->book.tracks[i].codec)) supported = false;
    if (!supported) {
        P.phase = PL_FAILED;
        str_copy(P.error, sizeof P.error, "Format not supported");
        book_detail_free(&r->book);
        free(r);
        return;
    }
    if (P.has_book) book_detail_free(&P.book);
    P.book = r->book;
    P.has_book = true;
    P.local = r->local;
    str_copy(P.session, sizeof P.session, r->session);
    sync_init(&P.sync, g_cfg->sync_interval);
    sleep_set(&P.sleep, SLEEP_OFF, 0, 0);
    PlayableBook pb;
    build_sources(r, &pb);
    if (pb.sources) {
        engine_set_speed(P.speed);
        engine_load(&pb, r->start, true);
        free(pb.sources);
        P.phase = PL_READY;
    } else {
        P.phase = PL_FAILED;
        str_copy(P.error, sizeof P.error, "Out of memory");
    }
    free(r);
}

/* ---- progress ------------------------------------------------------------------ */

typedef struct {
    char session[48];
    QEntry e;
    double listened;
    bool close;
} SyncJob;

static void sync_job(void *ud) {
    SyncJob *j = ud;
    int rc = ABS_ERR_NET;
    if (j->session[0]) {
        rc = j->close ? absc_close(j->session, j->e.current_time, j->listened, j->e.duration)
                      : absc_sync(j->session, j->e.current_time, j->listened, j->e.duration);
    }
    if (rc != ABS_OK && !j->session[0] && absc_have_session()) rc = absc_sync_local(&j->e, j->listened);
    if (rc == ABS_OK) {
        LightLock_Lock(&P.lock);
        pq_mark_synced(g_pq, j->e.item_id, j->e.updated_ms);
        LightLock_Unlock(&P.lock);
    }
    free(j);
}

/* Records the position locally and sends it (background). */
static void report(bool close) {
    if (!P.has_book) return;
    QEntry e = {0};
    str_copy(e.item_id, sizeof e.item_id, P.book.id);
    e.current_time = engine_position();
    e.duration = P.book.duration;
    e.updated_ms = sys_now_ms();
    e.finished = e.duration > 0 && e.current_time >= e.duration - 1;
    e.pending = true;
    LightLock_Lock(&P.lock);
    pq_put(g_pq, &e);
    LightLock_Unlock(&P.lock);
    if (g_save_queue) g_save_queue();
    SyncJob *j = calloc(1, sizeof *j);
    if (!j) return;
    str_copy(j->session, sizeof j->session, P.session);
    j->e = e;
    j->listened = sync_take_listened(&P.sync);
    j->close = close;
    if (!worker_post(g_sync, sync_job, j)) free(j);
}

static void close_session_async(void) {
    report(true);
    P.session[0] = 0;
}

static void flush_job(void *ud) {
    (void)ud;
    QEntry pending[PQ_MAX];
    int n = 0;
    LightLock_Lock(&P.lock);
    for (int i = 0; i < g_pq->n; i++)
        if (g_pq->e[i].pending) pending[n++] = g_pq->e[i];
    LightLock_Unlock(&P.lock);
    for (int i = 0; i < n; i++) {
        /* the open book is synced by its own session */
        if (P.has_book && P.session[0] && !strcmp(pending[i].item_id, P.book.id)) continue;
        if (absc_sync_local(&pending[i], 0) == ABS_OK) {
            LightLock_Lock(&P.lock);
            pq_mark_synced(g_pq, pending[i].item_id, pending[i].updated_ms);
            LightLock_Unlock(&P.lock);
        } else {
            break; /* still offline */
        }
    }
    if (g_save_queue) g_save_queue();
}

void player_flush_queue(void) {
    P.last_flush = osGetTime();
    if (worker_pending(g_sync) < 4) worker_post(g_sync, flush_job, NULL);
}

/* ---- public -------------------------------------------------------------------- */

void player_init(Config *cfg, ProgressQueue *pq, void (*save_queue)(void)) {
    memset(&P, 0, sizeof P);
    LightLock_Init(&P.lock);
    g_cfg = cfg;
    g_pq = pq;
    g_save_queue = save_queue;
    P.speed = cfg->speed;
    engine_set_auth(auth_cb);
}

void player_exit(void) {
    if (P.has_book) {
        engine_pause();
        report(true);
        /* give the close request a moment to go out */
        for (int i = 0; i < 150 && worker_pending(g_sync); i++) svcSleepThread(20000000LL);
        engine_stop();
        book_detail_free(&P.book);
        P.has_book = false;
    }
}

PlayerPhase player_phase(void) { return P.phase; }
const char *player_error(void) { return P.error; }
bool player_has_book(void) { return P.has_book && P.phase == PL_READY; }
const BookDetail *player_book(void) { return P.has_book ? &P.book : NULL; }
const char *player_book_id(void) { return P.has_book ? P.book.id : ""; }
bool player_is_local(void) { return P.local; }
bool player_playing(void) { return P.has_book && engine_is_playing(); }
double player_position(void) { return engine_position(); }
double player_duration(void) { return P.has_book ? P.book.duration : 0; }
float player_speed(void) { return P.speed; }
const SleepTimer *player_sleep(void) { return &P.sleep; }

int player_chapter_index(void) {
    return P.has_book ? tl_chapter_at(P.book.chapters, P.book.nchapters, engine_position()) : -1;
}

void player_pause(void) {
    if (!P.has_book || !engine_is_playing()) return;
    engine_pause();
    report(false);
}

void player_toggle(void) {
    if (!P.has_book) return;
    if (engine_is_playing()) player_pause();
    else {
        P.paused_by_lid = false;
        engine_play();
    }
}

void player_jump(double t) {
    if (!P.has_book) return;
    if (t < 0) t = 0;
    if (t > P.book.duration) t = P.book.duration;
    engine_seek(t);
    report(false);
}

void player_skip(double seconds) { player_jump(engine_position() + seconds); }

void player_chapter(int dir) {
    if (!P.has_book) return;
    double t = engine_position();
    double target = dir > 0 ? tl_next_chapter(P.book.chapters, P.book.nchapters, t)
                            : tl_prev_chapter(P.book.chapters, P.book.nchapters, t);
    if (dir > 0 && target < 0) return;
    player_jump(target);
}

void player_set_speed(float s) {
    P.speed = s;
    g_cfg->speed = s;
    engine_set_speed(s);
}

void player_cycle_speed(void) { player_set_speed(cfg_next_speed(P.speed)); }

void player_cycle_sleep(void) {
    /* off -> 15 -> 30 -> 45 -> 60 min -> end of chapter -> off */
    static const int mins[] = {15, 30, 45, 60};
    SleepTimer *s = &P.sleep;
    if (s->mode == SLEEP_OFF) { sleep_set(s, SLEEP_MINUTES, mins[0], 0); return; }
    if (s->mode == SLEEP_MINUTES) {
        double cur = s->remaining / 60.0;
        for (int i = 0; i < 4; i++)
            if (cur < mins[i] - 0.5) { sleep_set(s, SLEEP_MINUTES, mins[i], 0); return; }
        int ci = player_chapter_index();
        double end = ci >= 0 ? P.book.chapters[ci].end : P.book.duration;
        sleep_set(s, SLEEP_END_CHAPTER, 0, end);
        return;
    }
    sleep_set(s, SLEEP_OFF, 0, 0);
    engine_set_volume(1.0f);
}

void player_sleep_label(char *out, int n) {
    const SleepTimer *s = &P.sleep;
    if (s->mode == SLEEP_OFF) snprintf(out, n, "Sleep");
    else if (s->mode == SLEEP_END_CHAPTER) snprintf(out, n, "End of ch.");
    else {
        char t[16];
        fmt_time(s->remaining, t, sizeof t);
        snprintf(out, n, "%s", t);
    }
}

const char *player_status(void) {
    if (P.phase == PL_OPENING) return "Opening\xE2\x80\xA6";
    if (P.phase == PL_FAILED) return P.error;
    if (!P.has_book) return NULL;
    EngineState s = engine_state();
    if (s == ENG_ERROR) return engine_error() == STREAM_UNSUPPORTED ? "Format not supported" : "Playback error";
    int ns = engine_net_state();
    if (ns == HSRC_RECONNECTING) return "Reconnecting\xE2\x80\xA6";
    if (ns == HSRC_FAILED) return "Stream failed";
    if (s == ENG_LOADING) return "Loading\xE2\x80\xA6";
    if (s == ENG_BUFFERING && engine_is_playing()) return "Buffering\xE2\x80\xA6";
    if (!P.session[0] && !P.local) return NULL;
    if (!P.session[0] && P.local) return "Offline \xC2\xB7 progress saved locally";
    return NULL;
}

void player_on_suspend(void) {
    if (P.has_book) report(false);
}

void player_tick(double dt) {
    take_open_result();
    take_late_session();
    if (!P.has_book || P.phase != PL_READY) return;
    bool playing = engine_is_playing();
    double pos = engine_position();

    /* sync policy */
    if (sync_tick(&P.sync, dt, playing && engine_state() == ENG_PLAYING)) report(false);
    if (P.was_playing && !playing && engine_state() == ENG_ENDED) report(false);
    P.was_playing = playing;

    /* sleep timer */
    if (playing && engine_state() == ENG_PLAYING) {
        int r = sleep_tick(&P.sleep, dt, pos);
        if (r == SLEEP_STOP) {
            player_pause();
            engine_set_volume(1.0f);
        } else if (P.sleep.mode != SLEEP_OFF) {
            engine_set_volume(sleep_volume(&P.sleep, pos, P.speed));
        }
    }

    /* lid policy (edge-triggered): keep playing into headphones, otherwise pause */
    sys_allow_sleep(!playing);
    bool closed = sys_lid_closed();
    if (closed && !P.lid_was_closed) {
        if (playing && !sys_headphones()) {
            player_pause();
            P.paused_by_lid = true;
        }
        sys_backlight(false);
    } else if (!closed && P.lid_was_closed) {
        sys_backlight(true);
        P.paused_by_lid = false;
    } else if (closed && playing && !sys_headphones()) {
        /* headphones pulled while the lid is shut */
        player_pause();
    }
    P.lid_was_closed = closed;

    /* retry the offline queue every 2 minutes */
    if (osGetTime() - P.last_flush > 120000) player_flush_queue();
}
