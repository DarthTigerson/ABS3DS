/* Cover art: fetched from ABS (cached as JPEG on SD), decoded on a
 * background thread, uploaded to GPU textures on the main thread.
 * Thumbnails live in an LRU; one "hero" cover (+ blurred backdrop) is kept
 * for the focused/playing book. */
#ifndef SHELF3DS_COVERS_H
#define SHELF3DS_COVERS_H
#include <citro2d.h>
#include <stdbool.h>

typedef enum { COVER_NONE, COVER_LOADING, COVER_READY, COVER_MISSING } CoverState;

void covers_init(void);
void covers_exit(void);
/* Main thread, once per frame: uploads finished decodes. */
void covers_update(void);
/* 128x128 thumbnail (NULL until ready; requests it). */
const C2D_Image *covers_thumb(const char *id, CoverState *st);
/* 256x256 cover and a 32x32 linearly-filtered copy for blurred backdrops. */
const C2D_Image *covers_hero(const char *id, const C2D_Image **blur, CoverState *st);
/* Accent colour (C2D RGBA). false if unknown yet. */
bool covers_accent(const char *id, u32 *color);
/* Forget failures (e.g. after coming back online). */
void covers_retry_missing(void);

#endif
