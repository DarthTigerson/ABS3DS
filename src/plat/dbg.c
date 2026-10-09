#include "dbg.h"

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>

#define LOG "sdmc:/3ds/abs3ds/debug.log"
static LightLock g_lock;
static int g_init;

void dbg(const char *fmt, ...) {
    if (!g_init) { LightLock_Init(&g_lock); g_init = 1; }
    LightLock_Lock(&g_lock);
    FILE *f = fopen(LOG, "a");
    if (f) {
        if (ftell(f) > 256 * 1024) { fclose(f); remove(LOG ".old"); rename(LOG, LOG ".old"); f = fopen(LOG, "a"); }
    }
    if (f) {
        fprintf(f, "[%llu] ", (unsigned long long)(osGetTime() % 100000000ULL));
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    LightLock_Unlock(&g_lock);
}
