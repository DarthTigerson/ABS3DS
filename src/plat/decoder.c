#include "decoder.h"

#include <3ds.h>
#include <mpg123.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "aacstream.h"
#include "blockcache.h"
#include "dbg.h"
#include "http.h"
#include "httpsrc.h"
#include "mp3.h"
#include "mp4.h"
#include "reader.h"


enum { K_AAC, K_MP3, K_MP4MP3 };

struct Stream {
    int kind;
    /* media source */
    HttpSrc *hs;
    FileReaderCtx fmedia;
    Reader media;
    /* tables source (moov): a local file, or HTTP ranges through a block cache */
    FileReaderCtx ftables;
    Reader tables;
    bool tables_open;
    BlockCache *bc;
    struct TablesNet *net;
    AacStream aac;
    /* mp3 */
    mpg123_handle *mh;
    Mp3Info mi;
    uint64_t pos;
    uint8_t in[8192];
    int rate, ch;
    bool feed_eof;
    /* scratch for AAC frames larger than the caller wants */
    int16_t pcm[AACS_MAX_PCM];
    int pcm_frames, pcm_pos, pcm_ch, pcm_rate;
};

static int g_mpg_init;

bool stream_supported(const char *ext, const char *codec) {
    if (!strcasecmp(ext, ".mp3")) return true;
    if (!strcasecmp(ext, ".m4b") || !strcasecmp(ext, ".m4a") || !strcasecmp(ext, ".mp4") || !strcasecmp(ext, ".aac"))
        return !codec[0] || !strcasecmp(codec, "aac") || !strcasecmp(codec, "mp3") || !strcasecmp(codec, "he-aac");
    return false;
}

static bool is_mp4(const char *ext) {
    return !strcasecmp(ext, ".m4b") || !strcasecmp(ext, ".m4a") || !strcasecmp(ext, ".mp4");
}

/* ---- remote sample tables: Range requests through a block cache ------------------
 * The demuxer reads only the moov atoms it needs, a few KB at a time, so a 20 h book
 * starts after a handful of small requests instead of a multi-MB moov download. */

#define TABLE_BLOCK (32u * 1024)
#define TABLE_BLOCKS 16

typedef struct TablesNet {
    char url[512];
    void (*auth)(char *, unsigned);
    volatile int interrupted;
} TablesNet;

static int tables_fetch(void *ud, uint64_t off, uint32_t len, uint8_t *dst) {
    TablesNet *t = ud;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (t->interrupted) return -3;
        char auth[1100] = "";
        if (t->auth) t->auth(auth, sizeof auth);
        const char *hdrs[] = {auth[0] ? auth : NULL, NULL};
        HttpStream *st;
        int status;
        uint64_t clen;
        if (http_stream_open(t->url, hdrs, off, off + len - 1, &st, &status, &clen) != 0) continue;
        if (status != 206) {
            http_stream_close(st);
            dbg("tables: range %llu+%lu -> %d", (unsigned long long)off, (unsigned long)len, status);
            if (status == 404 || status == 416) return -1;
            continue;
        }
        uint32_t got = 0;
        while (got < len) {
            int k = http_stream_read(st, dst + got, len - got, 8000);
            if (k <= 0) break;
            got += (uint32_t)k;
        }
        http_stream_close(st);
        if (got == len) return (int)got;
        dbg("tables: short read %lu of %lu at %llu", (unsigned long)got, (unsigned long)len, (unsigned long long)off);
    }
    return -1;
}

static int open_remote_tables(Stream *s, const TrackSource *src, void (*auth)(char *, unsigned)) {
    s->net = calloc(1, sizeof *s->net);
    if (!s->net) return STREAM_ERR;
    snprintf(s->net->url, sizeof s->net->url, "%s", src->url);
    s->net->auth = auth;
    s->bc = bc_new(src->size, TABLE_BLOCK, TABLE_BLOCKS, tables_fetch, s->net);
    if (!s->bc) return STREAM_ERR;
    s->tables = bc_reader(s->bc);
    uint64_t moov_off, moov_size;
    if (mp4_find_moov(&s->tables, &moov_off, &moov_size) != 0) {
        dbg("moov: not found in %s (size %llu)", src->url, (unsigned long long)src->size);
        return STREAM_IO;
    }
    int fetches;
    uint64_t bytes;
    if (aacs_open_at(&s->aac, &s->media, &s->tables, moov_off, moov_size) == 0) {
        bc_stats(s->bc, &fetches, &bytes);
        dbg("moov: at %llu, %llu bytes; tables ready after %d requests, %llu KB", (unsigned long long)moov_off,
            (unsigned long long)moov_size, fetches, (unsigned long long)(bytes >> 10));
        return 0;
    }
    /* MP3-in-MP4 */
    static Mp4 probe;
    if (mp4_open_at(&probe, &s->tables, moov_off, moov_size) == 0 && probe.codec == MP4_MP3) {
        s->aac.mp4 = probe;
        s->kind = K_MP4MP3;
        return 0;
    }
    dbg("moov: no AAC or MP3 audio track");
    return STREAM_UNSUPPORTED;
}

static int open_mpg(Stream *s) {
    if (!g_mpg_init) { mpg123_init(); g_mpg_init = 1; }
    int err;
    s->mh = mpg123_new(NULL, &err);
    if (!s->mh) return STREAM_ERR;
    mpg123_param(s->mh, MPG123_FLAGS, MPG123_QUIET, 0);
    mpg123_format_none(s->mh);
    const long *rates;
    size_t nr;
    mpg123_rates(&rates, &nr);
    for (size_t i = 0; i < nr; i++) mpg123_format(s->mh, rates[i], MPG123_MONO | MPG123_STEREO, MPG123_ENC_SIGNED_16);
    if (mpg123_open_feed(s->mh) != MPG123_OK) return STREAM_ERR;
    return 0;
}

Stream *stream_open(const TrackSource *src, void (*auth)(char *, unsigned), int *err) {
    *err = 0;
    if (!stream_supported(src->ext, src->codec)) { *err = STREAM_UNSUPPORTED; return NULL; }
    Stream *s = calloc(1, sizeof *s);
    if (!s) { *err = STREAM_ERR; return NULL; }
    int rc = 0;
    if (src->local) {
        if (reader_file_open(&s->media, &s->fmedia, src->path, 0, 0) != 0) { rc = STREAM_IO; goto fail; }
    } else {
        s->hs = httpsrc_open(src->url, src->size, auth);
        if (!s->hs) { rc = STREAM_ERR; goto fail; }
        s->media = httpsrc_reader(s->hs);
    }
    if (is_mp4(src->ext)) {
        s->kind = K_AAC;
        if (src->local) {
            if (reader_file_open(&s->tables, &s->ftables, src->path, 0, 0) != 0) { rc = STREAM_IO; goto fail; }
            s->tables_open = true;
            if (aacs_open(&s->aac, &s->media, &s->tables) != 0) {
                static Mp4 probe;
                if (mp4_open(&probe, &s->tables) == 0 && probe.codec == MP4_MP3) { s->aac.mp4 = probe; s->kind = K_MP4MP3; }
                else { rc = STREAM_UNSUPPORTED; goto fail; }
            }
        } else if ((rc = open_remote_tables(s, src, auth)) != 0) {
            goto fail;
        }
        if (s->kind == K_MP4MP3 && (rc = open_mpg(s)) != 0) goto fail;
    } else {
        s->kind = K_MP3;
        if (mp3_probe(&s->media, &s->mi) != 0) { rc = STREAM_UNSUPPORTED; goto fail; }
        if ((rc = open_mpg(s)) != 0) goto fail;
        s->pos = 0; /* let mpg123 skip ID3 itself */
    }
    return s;
fail:
    *err = rc ? rc : STREAM_ERR;
    stream_close(s);
    return NULL;
}

int stream_seek(Stream *s, double t, double *actual) {
    s->pcm_frames = s->pcm_pos = 0;
    s->feed_eof = false;
    if (s->kind == K_AAC || s->kind == K_MP4MP3) {
        if (s->kind == K_AAC) return aacs_seek(&s->aac, t, actual) == 0 ? 0 : STREAM_ERR;
        if (mp4_seek(&s->aac.mp4, t, actual) != 0) return STREAM_ERR;
        mpg123_open_feed(s->mh);
        return 0;
    }
    s->pos = t <= 0 ? 0 : mp3_offset_for_time(&s->mi, t);
    mpg123_open_feed(s->mh); /* reset the parser; it resyncs on the next frame */
    if (actual) *actual = t < 0 ? 0 : t;
    return 0;
}

static int read_mpg(Stream *s, int16_t *out, int max_frames, int *channels, int *rate) {
    for (;;) {
        size_t done = 0;
        int ch = s->ch ? s->ch : 2;
        int r = mpg123_read(s->mh, (unsigned char *)out, (size_t)max_frames * ch * 2, &done);
        if (r == MPG123_NEW_FORMAT) {
            long rt;
            int c, enc;
            mpg123_getformat(s->mh, &rt, &c, &enc);
            s->rate = (int)rt;
            s->ch = c;
            if (done == 0) continue;
        }
        if (done > 0) {
            *channels = s->ch;
            *rate = s->rate;
            return (int)(done / (2 * s->ch));
        }
        if (r == MPG123_NEED_MORE || r == MPG123_NEW_FORMAT) {
            if (s->feed_eof) return STREAM_EOF;
            if (s->kind == K_MP3) {
                int k = s->media.read_at(s->media.ctx, s->pos, s->in, sizeof s->in);
                if (k == HSRC_INTERRUPTED) return STREAM_INTERRUPTED;
                if (k < 0) return STREAM_IO;
                if (k == 0) { s->feed_eof = true; continue; }
                s->pos += (uint64_t)k;
                mpg123_feed(s->mh, s->in, (size_t)k);
            } else {
                uint64_t off;
                uint32_t size;
                Mp4Cursor mark = mp4_tell(&s->aac.mp4);
                int rc = mp4_next(&s->aac.mp4, &off, &size);
                if (rc == 1) { s->feed_eof = true; continue; }
                if (rc < 0) { mp4_restore(&s->aac.mp4, &mark); return STREAM_IO; } /* tables unreadable: retry */
                if (size > sizeof s->in) return STREAM_ERR;
                int k = reader_read_full(&s->media, off, s->in, size);
                if (k != 0) {
                    /* not consumed: step the demuxer back so the sample is retried */
                    mp4_restore(&s->aac.mp4, &mark);
                    return k == -3 ? STREAM_INTERRUPTED : STREAM_IO;
                }
                mpg123_feed(s->mh, s->in, size);
            }
            continue;
        }
        if (r == MPG123_DONE) return STREAM_EOF;
        if (r == MPG123_ERR) {
            /* corrupt data: skip ahead a little and resync */
            mpg123_open_feed(s->mh);
            if (s->kind == K_MP3) s->pos += 417;
            continue;
        }
        return STREAM_ERR;
    }
}

int stream_read(Stream *s, int16_t *out, int max_frames, int *channels, int *rate) {
    if (s->kind == K_MP3 || s->kind == K_MP4MP3) return read_mpg(s, out, max_frames, channels, rate);
    if (s->pcm_pos >= s->pcm_frames) {
        int frames, ch, rt;
        int rc = aacs_decode(&s->aac, s->pcm, &frames, &ch, &rt);
        if (rc == 1) return STREAM_EOF;
        if (rc == -3) return STREAM_INTERRUPTED;
        if (rc == -2) return STREAM_IO;
        if (rc < 0) return STREAM_ERR;
        s->pcm_frames = frames;
        s->pcm_pos = 0;
        s->pcm_ch = ch;
        s->pcm_rate = rt;
    }
    int n = s->pcm_frames - s->pcm_pos;
    if (n > max_frames) n = max_frames;
    memcpy(out, s->pcm + s->pcm_pos * s->pcm_ch, (size_t)n * s->pcm_ch * sizeof(int16_t));
    s->pcm_pos += n;
    *channels = s->pcm_ch;
    *rate = s->pcm_rate;
    return n;
}

void stream_interrupt(Stream *s, int on) {
    if (!s) return;
    if (s->hs) httpsrc_interrupt(s->hs, on);
    if (s->net) s->net->interrupted = on;
}

int stream_net_state(Stream *s) {
    return s && s->hs ? httpsrc_state(s->hs) : 0;
}

void stream_close(Stream *s) {
    if (!s) return;
    if (s->kind == K_AAC) aacs_close(&s->aac);
    if (s->mh) { mpg123_close(s->mh); mpg123_delete(s->mh); }
    if (s->hs) httpsrc_close(s->hs);
    else reader_file_close(&s->fmedia);
    if (s->tables_open) reader_file_close(&s->ftables);
    bc_free(s->bc);
    free(s->net);
    free(s);
}
