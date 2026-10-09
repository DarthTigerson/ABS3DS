/* Offline downloads to sdmc:/3ds/abs3ds/books/<itemId>/ :
 *   meta.json   book detail (abs_parse_item format)
 *   NN<ext>     track files (".part" while incomplete)
 *   .done       marker written when every track is complete */
#ifndef ABS3DS_DOWNLOADS_H
#define ABS3DS_DOWNLOADS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "models.h"

typedef enum { DL_NONE, DL_QUEUED, DL_ACTIVE, DL_DONE, DL_PAUSED } DlState;

typedef struct {
    char id[40];
    char title[160];
    char author[96];
    DlState state;
    uint64_t done_bytes, total_bytes;
    char error[64];
} DlInfo;

void dl_init(void);   /* scans the SD card */
void dl_exit(void);
int dl_start(const BookDetail *b);         /* queue (or resume) a download */
void dl_cancel(const char *id);            /* stops; keeps partial files */
int dl_delete(const char *id);
DlState dl_state(const char *id, float *pct);
bool dl_is_done(const char *id);
int dl_list(DlInfo *out, int max);
int dl_load_meta(const char *id, BookDetail *out);
void dl_track_path(const char *id, int i, const char *ext, char *out, size_t n);
uint64_t dl_total_bytes(void);

#endif
