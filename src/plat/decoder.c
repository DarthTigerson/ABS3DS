#include "decoder.h"

#include <3ds.h>
#include <mpg123.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "aacstream.h"
#include "httpsrc.h"
#include "mp3.h"
#include "mp4.h"
#include "reader.h"

#define MOOV_DIR "sdmc:/3ds/shelf3ds/moov"
#define MOOV_MAX (48u << 20)

enum { K_AAC, K_MP3, K_MP4MP3 };

struct Stream {
    int kind;
    /* media source */
    HttpSrc *hs;
    FileReaderCtx fmedia;
    Reader media;
    /* tables source (moov) */
    FileReaderCtx ftables;
    Reader tables;
    bool tables_open;
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

/* Finds (or downloads) the moov of a remote file into the SD cache. */
static int open_remote_tables(Stream *s, const TrackSource *src, void (*auth)(char *, unsigned)) {
    char path[200], offp[200];
    snprintf(path, sizeof path, MOOV_DIR "/%s.moov", src->cache_key);
    snprintf(offp, sizeof offp, MOOV_DIR "/%s.off", src->cache_key);
    unsigned long long moov_off = 0, moov_size = 0;
    FILE *f = fopen(offp, "r");
    if (f) {
        if (fscanf(f, "%llu %llu", &moov_off, &moov_size) != 2) moov_off = moov_size = 0;
        fclose(f);
    }
    struct stat st;
    if (!moov_size || stat(path, &st) != 0 || (unsigned long long)st.st_size != moov_size) {
        static RangeReaderCtx rc; /* only used from the engine thread */
        Reader rr;
        range_reader_open(&rr, &rc, src->url, src->size, auth);
        uint64_t o, z;
        if (mp4_find_moov(&rr, &o, &z) != 0 || z > MOOV_MAX) return STREAM_UNSUPPORTED;
        mkdir("sdmc:/3ds", 0777);
        mkdir("sdmc:/3ds/shelf3ds", 0777);
        mkdir(MOOV_DIR, 0777);
        if (http_fetch_range_to_file(src->url, auth, o, z, path) != 0) return STREAM_IO;
        moov_off = o;
        moov_size = z;
        f = fopen(offp, "w");
        if (f) { fprintf(f, "%llu %llu\n", moov_off, moov_size); fclose(f); }
    }
    if (reader_file_open(&s->tables, &s->ftables, path, moov_off, src->size) != 0) return STREAM_IO;
    s->tables_open = true;
    if (aacs_open_at(&s->aac, &s->media, &s->tables, moov_off, moov_size) == 0) return 0;
    /* MP3-in-MP4 */
    static Mp4 probe;
    if (mp4_open_at(&probe, &s->tables, moov_off, moov_size) == 0 && probe.codec == MP4_MP3) {
        s->aac.mp4 = probe;
        s->kind = K_MP4MP3;
        return 0;
    }
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
                if (rc < 0 || size > sizeof s->in) return STREAM_ERR;
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
    if (s && s->hs) httpsrc_interrupt(s->hs, on);
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
    free(s);
}
