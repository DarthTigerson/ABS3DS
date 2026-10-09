/* Audio engine: a high-priority thread that decodes the current book,
 * time-stretches it (sonic) and feeds NDSP. The UI talks to it through
 * commands and reads back state/position. */
#ifndef SHELF3DS_ENGINE_H
#define SHELF3DS_ENGINE_H
#include <stdbool.h>

#include "decoder.h"
#include "models.h"

typedef enum { ENG_IDLE, ENG_LOADING, ENG_PLAYING, ENG_PAUSED, ENG_BUFFERING, ENG_ENDED, ENG_ERROR } EngineState;

typedef struct {
    int ntracks;
    Track *tracks;          /* copied by engine_load */
    TrackSource *sources;   /* one per track, copied */
    double duration;
} PlayableBook;

int engine_init(void);
bool engine_available(void);    /* false when NDSP failed (no DSP firmware dump) */
void engine_exit(void);
void engine_set_auth(void (*auth)(char *, unsigned));

void engine_load(const PlayableBook *b, double start_t, bool autoplay);
void engine_stop(void);
void engine_play(void);
void engine_pause(void);
void engine_seek(double t);
void engine_set_speed(float speed);
void engine_set_volume(float v);

EngineState engine_state(void);
bool engine_is_playing(void);   /* playing or buffering with the intent to play */
double engine_position(void);   /* book seconds actually heard */
double engine_duration(void);
float engine_cpu_load(void);    /* share of wall time spent decoding, 0..1 */
int engine_error(void);         /* STREAM_* code for ENG_ERROR */
int engine_net_state(void);     /* HSRC_* for the current track */
float engine_speed(void);

#endif
