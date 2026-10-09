#include "engine.h"

#include <3ds.h>
#include <stdlib.h>
#include <string.h>

#include "httpsrc.h"
#include "sonic.h"
#include "timeline.h"

#define CH 0
#define NBUF 8
#define BUF_FRAMES 4096

typedef enum { C_LOAD, C_STOP, C_SEEK, C_SPEED } CmdType;
typedef struct { CmdType type; double arg; bool autoplay; } Cmd;

static struct {
    /* thread + command queue */
    Thread th;
    volatile bool quit;
    LightLock lock;
    Cmd q[16];
    int qn;
    PlayableBook pending_book;
    bool has_pending_book;
    void (*auth)(char *, unsigned);

    /* published state (written by the engine thread, read by the UI) */
    volatile EngineState state;
    volatile double pos;
    volatile float cpu;
    volatile int error;
    volatile bool want_play; /* user intent */
    volatile float speed;

    /* engine-thread private */
    PlayableBook book;
    int cur;            /* current track */
    Stream *st;
    int rate, ch;       /* NDSP channel format */
    sonicStream son;
    float son_speed;
    double anchor_t;    /* book time of the first output frame since anchor */
    double out_frames;  /* output frames written since anchor */
    bool end_of_book, flushed;
    int16_t *bufdata[NBUF];
    ndspWaveBuf wb[NBUF];
    double wb_t0[NBUF];
    double wb_step[NBUF];
    int16_t dec[STREAM_MAX_FRAMES * 2];
    int dec_frames, dec_pos, dec_ch, dec_rate; /* decoded PCM not yet consumed */
    double last_end_t;  /* time at the end of the last submitted buffer */
    u64 busy, window_start;
} E;

/* ---- helpers --------------------------------------------------------------- */

static void set_state(EngineState s) { E.state = s; }

static void close_stream(void) {
    LightLock_Lock(&E.lock);
    Stream *s = E.st;
    E.st = NULL;
    LightLock_Unlock(&E.lock);
    stream_close(s);
}

static void reset_output(void) {
    ndspChnWaveBufClear(CH);
    for (int i = 0; i < NBUF; i++) E.wb[i].status = NDSP_WBUF_FREE;
    if (E.son) { sonicDestroyStream(E.son); E.son = NULL; }
    E.dec_frames = E.dec_pos = 0;
    E.end_of_book = E.flushed = false;
}

static void configure_channel(int rate, int ch) {
    ndspChnReset(CH);
    ndspChnSetInterp(CH, NDSP_INTERP_POLYPHASE);
    ndspChnSetRate(CH, (float)rate);
    ndspChnSetFormat(CH, ch == 1 ? NDSP_FORMAT_MONO_PCM16 : NDSP_FORMAT_STEREO_PCM16);
    float mix[12] = {0};
    mix[0] = mix[1] = 1.0f;
    ndspChnSetMix(CH, mix);
    E.rate = rate;
    E.ch = ch;
    ndspChnSetPaused(CH, !E.want_play);
}

static int buffers_queued(void) {
    int n = 0;
    for (int i = 0; i < NBUF; i++)
        if (E.wb[i].status == NDSP_WBUF_QUEUED || E.wb[i].status == NDSP_WBUF_PLAYING) n++;
    return n;
}

/* Opens track i and seeks to local time t. Updates the anchor. */
static int open_track(int i, double t) {
    close_stream();
    int err = 0;
    Stream *s = stream_open(&E.book.sources[i], E.auth, &err);
    if (!s) return err ? err : STREAM_ERR;
    double actual = t;
    if (t > 0.05 && stream_seek(s, t, &actual) != 0) actual = 0;
    LightLock_Lock(&E.lock);
    E.st = s;
    LightLock_Unlock(&E.lock);
    E.cur = i;
    E.anchor_t = E.book.tracks[i].start_offset + actual;
    E.out_frames = 0;
    E.end_of_book = false;
    return 0;
}

static void reanchor_at_cursor(void) {
    if (E.rate) E.anchor_t += E.out_frames * E.son_speed / E.rate;
    E.out_frames = 0;
}

static void ensure_sonic(void) {
    float sp = E.speed;
    if (sp > 0.99f && sp < 1.01f) {
        if (E.son && E.son_speed != 1.0f) { reanchor_at_cursor(); }
        if (E.son) { sonicDestroyStream(E.son); E.son = NULL; }
        E.son_speed = 1.0f;
        return;
    }
    if (!E.son || E.son_speed != sp) {
        reanchor_at_cursor();
        if (!E.son) E.son = sonicCreateStream(E.rate, E.ch);
        if (E.son) sonicSetSpeed(E.son, sp);
        E.son_speed = sp;
    }
}

/* Gets more decoded PCM into E.dec. 1 ok, 0 nothing (EOF of book / wait), <0 error */
static int pull_decoded(void) {
    while (E.st) {
        int ch, rate;
        int n = stream_read(E.st, E.dec, STREAM_MAX_FRAMES, &ch, &rate);
        if (n > 0) {
            E.dec_frames = n;
            E.dec_pos = 0;
            E.dec_ch = ch;
            E.dec_rate = rate;
            return 1;
        }
        if (n == STREAM_EOF) {
            if (E.cur + 1 < E.book.ntracks) {
                int rc = open_track(E.cur + 1, 0);
                if (rc) return rc;
                return 0; /* new anchor: let the caller submit what it has first */
            }
            E.end_of_book = true;
            return 0;
        }
        if (n == STREAM_IO || n == STREAM_INTERRUPTED) return 0; /* retry later */
        return n;
    }
    return 0;
}

/* Fills buffer i. Returns frames written (0 if nothing available). */
static int fill(int i) {
    int16_t *dst = E.bufdata[i];
    int frames = 0;
    double t0 = E.anchor_t + (E.rate ? E.out_frames * E.son_speed / E.rate : 0);
    while (frames < BUF_FRAMES) {
        if (E.son) {
            int k = sonicReadShortFromStream(E.son, dst + frames * E.ch, BUF_FRAMES - frames);
            if (k > 0) { frames += k; continue; }
        }
        if (E.dec_pos < E.dec_frames) {
            if (E.dec_ch != E.ch || E.dec_rate != E.rate) {
                /* format change (e.g. next track differs): drain first */
                if (frames > 0 || buffers_queued() > 0) break;
                configure_channel(E.dec_rate, E.dec_ch);
                if (E.son) { sonicDestroyStream(E.son); E.son = NULL; }
                E.son_speed = 1.0f;
                t0 = E.anchor_t + E.out_frames / E.rate;
            }
            ensure_sonic();
            int n = E.dec_frames - E.dec_pos;
            if (E.son) {
                sonicWriteShortToStream(E.son, E.dec + E.dec_pos * E.ch, n);
                E.dec_pos = E.dec_frames;
            } else {
                if (n > BUF_FRAMES - frames) n = BUF_FRAMES - frames;
                memcpy(dst + frames * E.ch, E.dec + E.dec_pos * E.ch, (size_t)n * E.ch * 2);
                E.dec_pos += n;
                frames += n;
            }
            continue;
        }
        if (E.end_of_book) {
            if (E.son && !E.flushed) { sonicFlushStream(E.son); E.flushed = true; continue; }
            break;
        }
        int cur_before = E.cur;
        int rc = pull_decoded();
        if (rc < 0) { E.error = rc; set_state(ENG_ERROR); break; }
        if (rc == 0) {
            if (E.cur != cur_before && frames == 0) {
                t0 = E.anchor_t; /* fresh track, fresh anchor */
                continue;
            }
            break;
        }
    }
    if (frames == 0) return 0;
    E.wb_t0[i] = t0;
    E.wb_step[i] = (double)E.son_speed / E.rate;
    E.out_frames += frames;
    E.last_end_t = t0 + frames * E.wb_step[i];
    E.wb[i].data_pcm16 = dst;
    E.wb[i].nsamples = (u32)frames;
    DSP_FlushDataCache(dst, (u32)frames * E.ch * 2);
    ndspChnWaveBufAdd(CH, &E.wb[i]);
    return frames;
}

static void update_position(void) {
    u16 seq = ndspChnGetWaveBufSeq(CH);
    u32 sp = ndspChnGetSamplePos(CH);
    for (int i = 0; i < NBUF; i++) {
        if (E.wb[i].status == NDSP_WBUF_PLAYING || (E.wb[i].status == NDSP_WBUF_QUEUED && E.wb[i].sequence_id == seq)) {
            if (E.wb[i].sequence_id == seq) {
                E.pos = E.wb_t0[i] + sp * E.wb_step[i];
                return;
            }
        }
    }
    if (!buffers_queued() && E.state != ENG_LOADING) {
        /* nothing queued: we're at the end of what was submitted */
        if (E.last_end_t > 0) E.pos = E.last_end_t;
    }
}

/* ---- commands ---------------------------------------------------------------- */

static void do_load(PlayableBook *b, double t, bool autoplay) {
    set_state(ENG_LOADING);
    close_stream();
    reset_output();
    free(E.book.tracks);
    free(E.book.sources);
    E.book = *b;
    E.want_play = autoplay;
    E.pos = t;
    E.last_end_t = t;
    E.error = 0;
    double local;
    int i = tl_locate(E.book.tracks, E.book.ntracks, t, &local);
    int rc = i < 0 ? STREAM_ERR : open_track(i, local);
    if (rc) { E.error = rc; set_state(ENG_ERROR); return; }
    E.rate = 0; /* force channel configuration from the first decoded frame */
    E.ch = 0;
    set_state(autoplay ? ENG_BUFFERING : ENG_PAUSED);
}

static void do_seek(double t) {
    if (!E.book.ntracks) return;
    if (t < 0) t = 0;
    if (t > E.book.duration - 0.5) t = E.book.duration - 0.5;
    if (t < 0) t = 0;
    reset_output();
    double local;
    int i = tl_locate(E.book.tracks, E.book.ntracks, t, &local);
    int rc;
    if (i == E.cur && E.st) {
        double actual = local;
        rc = stream_seek(E.st, local, &actual);
        E.anchor_t = E.book.tracks[i].start_offset + actual;
        E.out_frames = 0;
    } else {
        rc = open_track(i, local);
    }
    if (rc) { E.error = rc; set_state(ENG_ERROR); return; }
    E.pos = E.anchor_t;
    E.last_end_t = E.anchor_t;
    E.son_speed = 1.0f;
    if (E.state != ENG_ERROR) set_state(E.want_play ? ENG_BUFFERING : ENG_PAUSED);
}

static void process_commands(void) {
    for (;;) {
        LightLock_Lock(&E.lock);
        if (!E.qn) { LightLock_Unlock(&E.lock); return; }
        Cmd c = E.q[0];
        memmove(E.q, E.q + 1, sizeof(Cmd) * (--E.qn));
        PlayableBook b = E.pending_book;
        bool hb = E.has_pending_book;
        if (c.type == C_LOAD) E.has_pending_book = false;
        LightLock_Unlock(&E.lock);
        if (E.st) stream_interrupt(E.st, 0);
        switch (c.type) {
        case C_LOAD: if (hb) do_load(&b, c.arg, c.autoplay); break;
        case C_STOP:
            close_stream();
            reset_output();
            set_state(ENG_IDLE);
            break;
        case C_SEEK: do_seek(c.arg); break;
        case C_SPEED: break; /* applied lazily by ensure_sonic() at the next fill */
        }
    }
}

/* ---- thread -------------------------------------------------------------------- */

static void engine_thread(void *arg) {
    (void)arg;
    E.window_start = svcGetSystemTick();
    while (!E.quit) {
        process_commands();
        EngineState s = E.state;
        if (s == ENG_PLAYING || s == ENG_BUFFERING || s == ENG_PAUSED) {
            u64 t0 = svcGetSystemTick();
            /* first decode configures the channel */
            if (!E.rate && E.st) {
                if (pull_decoded() > 0) configure_channel(E.dec_rate, E.dec_ch);
            }
            if (E.rate) {
                for (int i = 0; i < NBUF; i++) {
                    if (E.wb[i].status == NDSP_WBUF_FREE || E.wb[i].status == NDSP_WBUF_DONE) {
                        if (E.qn) break; /* respond to commands quickly */
                        if (fill(i) == 0) break;
                    }
                }
            }
            E.busy += svcGetSystemTick() - t0;
            update_position();
            int queued = buffers_queued();
            if (E.state != ENG_ERROR) {
                if (E.end_of_book && !queued && E.dec_pos >= E.dec_frames) set_state(ENG_ENDED);
                else if (!E.want_play) set_state(ENG_PAUSED);
                else if (queued == 0) set_state(ENG_BUFFERING);
                else if (queued >= 2 || E.end_of_book) set_state(ENG_PLAYING);
            }
            /* hold playback until a little audio is buffered, to avoid stutter */
            ndspChnSetPaused(CH, !(E.want_play && (E.state == ENG_PLAYING)));
        }
        u64 now = svcGetSystemTick();
        if (now - E.window_start > SYSCLOCK_ARM11) {
            E.cpu = (float)((double)E.busy / (double)(now - E.window_start));
            E.busy = 0;
            E.window_start = now;
        }
        svcSleepThread(8000000LL); /* 8 ms: well under one 93 ms buffer */
    }
    close_stream();
    reset_output();
}

static bool g_ndsp_ok;
bool engine_available(void) { return g_ndsp_ok; }

int engine_init(void) {
    memset(&E, 0, sizeof E);
    if (R_FAILED(ndspInit())) return -1;
    g_ndsp_ok = true;
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    LightLock_Init(&E.lock);
    for (int i = 0; i < NBUF; i++) {
        E.bufdata[i] = linearAlloc(BUF_FRAMES * 2 * sizeof(int16_t));
        if (!E.bufdata[i]) return -1;
    }
    E.speed = 1.0f;
    E.son_speed = 1.0f;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    E.th = threadCreate(engine_thread, NULL, 48 * 1024, prio - 4 > 0x18 ? prio - 4 : 0x18, 0, false);
    return E.th ? 0 : -1;
}

void engine_exit(void) {
    if (!E.th) { if (g_ndsp_ok) ndspExit(); g_ndsp_ok = false; return; }
    if (E.st) stream_interrupt(E.st, 1);
    E.quit = true;
    threadJoin(E.th, U64_MAX);
    threadFree(E.th);
    E.th = NULL;
    for (int i = 0; i < NBUF; i++) linearFree(E.bufdata[i]);
    free(E.book.tracks);
    free(E.book.sources);
    ndspExit();
}

void engine_set_auth(void (*auth)(char *, unsigned)) { E.auth = auth; }

static void push(Cmd c) {
    LightLock_Lock(&E.lock);
    /* collapse repeated seeks/speed changes */
    if (E.qn && E.q[E.qn - 1].type == c.type && (c.type == C_SEEK || c.type == C_SPEED)) E.q[E.qn - 1] = c;
    else if (E.qn < 16) E.q[E.qn++] = c;
    if (E.st) stream_interrupt(E.st, 1); /* unblock a waiting network read */
    LightLock_Unlock(&E.lock);
}

void engine_load(const PlayableBook *b, double start_t, bool autoplay) {
    PlayableBook copy = *b;
    copy.tracks = malloc(sizeof(Track) * b->ntracks);
    copy.sources = malloc(sizeof(TrackSource) * b->ntracks);
    if (!copy.tracks || !copy.sources) { free(copy.tracks); free(copy.sources); return; }
    memcpy(copy.tracks, b->tracks, sizeof(Track) * b->ntracks);
    memcpy(copy.sources, b->sources, sizeof(TrackSource) * b->ntracks);
    if (copy.duration <= 0) copy.duration = tl_duration(copy.tracks, copy.ntracks);
    LightLock_Lock(&E.lock);
    if (E.has_pending_book) { free(E.pending_book.tracks); free(E.pending_book.sources); }
    E.pending_book = copy;
    E.has_pending_book = true;
    E.state = ENG_LOADING;
    E.pos = start_t;
    E.want_play = autoplay;
    LightLock_Unlock(&E.lock);
    E.book.duration = copy.duration;
    push((Cmd){C_LOAD, start_t, autoplay});
}

void engine_stop(void) {
    E.want_play = false;
    ndspChnSetPaused(CH, true);
    push((Cmd){C_STOP, 0, false});
}

void engine_play(void) {
    if (E.state == ENG_ENDED) { engine_seek(0); }
    E.want_play = true;
}

void engine_pause(void) {
    E.want_play = false;
    ndspChnSetPaused(CH, true); /* immediate, even if the engine thread is busy */
}

void engine_seek(double t) {
    E.pos = t < 0 ? 0 : t;
    push((Cmd){C_SEEK, t, false});
}

void engine_set_speed(float speed) {
    if (speed < 0.5f) speed = 0.5f;
    if (speed > 3.0f) speed = 3.0f;
    E.speed = speed;
    push((Cmd){C_SPEED, speed, false});
}

void engine_set_volume(float v) {
    float mix[12] = {0};
    mix[0] = mix[1] = v < 0 ? 0 : v > 1 ? 1 : v;
    ndspChnSetMix(CH, mix);
}

EngineState engine_state(void) { return E.state; }
bool engine_is_playing(void) { return E.want_play && (E.state == ENG_PLAYING || E.state == ENG_BUFFERING || E.state == ENG_LOADING); }
double engine_position(void) { return E.pos; }
double engine_duration(void) { return E.book.duration; }
float engine_cpu_load(void) { return E.cpu; }
int engine_error(void) { return E.error; }
float engine_speed(void) { return E.speed; }

int engine_net_state(void) {
    LightLock_Lock(&E.lock);
    int s = E.st ? stream_net_state(E.st) : 0;
    LightLock_Unlock(&E.lock);
    return s;
}
