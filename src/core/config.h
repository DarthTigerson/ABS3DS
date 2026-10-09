/* Persistent settings and credentials (sdmc:/3ds/abs3ds/config.json). */
#ifndef ABS3DS_CONFIG_H
#define ABS3DS_CONFIG_H
#include <stdbool.h>

#define CFG_DEFAULT_SERVER "http://192.168.1.10:13378/audiobookshelf"

typedef struct {
    char server[160];
    char username[64];
    char refresh[1024];
    char user_id[48];
    char device_id[40];
    char library_id[40];
    int skip_back, skip_fwd;   /* seconds */
    float speed;
    int sync_interval;         /* seconds */
    bool screen_off;           /* backlight off while playing with the lid closed */
    long long clock_offset_ms; /* console clock -> UTC, learned online */
    bool clock_known;
} Config;

void cfg_defaults(Config *c);
/* Missing/corrupt file -> defaults, returns -1. Out-of-range values are clamped. */
int cfg_load(Config *c, const char *path);
int cfg_save(const Config *c, const char *path);
float cfg_next_speed(float s);   /* 0.75 -> 1 -> 1.25 -> 1.5 -> 1.75 -> 2 -> 0.75 */
int cfg_next_skip(int s);        /* 10 -> 15 -> 30 -> 60 -> 10 */

#endif
