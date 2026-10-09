/* One playable track: byte source (SD file or HTTP) + demux + decoder. */
#ifndef SHELF3DS_DECODER_H
#define SHELF3DS_DECODER_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool local;
    char path[256];  /* local file */
    char url[512];   /* remote file */
    uint64_t size;   /* bytes, 0 if unknown */
    char cache_key[96]; /* e.g. "<itemId>_<ino>" for the moov cache */
    char ext[8];
    char codec[16];
} TrackSource;

enum { STREAM_EOF = 1, STREAM_ERR = -1, STREAM_IO = -2, STREAM_INTERRUPTED = -3, STREAM_UNSUPPORTED = -4 };

typedef struct Stream Stream;

/* Blocking (network). On failure returns NULL and *err gets a STREAM_* code. */
Stream *stream_open(const TrackSource *src, void (*auth)(char *, unsigned), int *err);
/* Seeks to local time t; *actual gets the real position. 0 ok. */
int stream_seek(Stream *s, double t, double *actual);
/* Decodes some PCM (interleaved s16). Returns frames (>0), or STREAM_* (<= 0 handled: EOF=1 means end). */
int stream_read(Stream *s, int16_t *out, int max_frames, int *channels, int *rate);
void stream_interrupt(Stream *s, int on);
int stream_net_state(Stream *s); /* HSRC_* or 0 for local */
void stream_close(Stream *s);
/* True when the codec/container is something this player can decode. */
bool stream_supported(const char *ext, const char *codec);

#define STREAM_MAX_FRAMES 4096

#endif
