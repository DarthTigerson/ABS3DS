/* Tiny append-only log at sdmc:/3ds/abs3ds/debug.log (rotated at 256 KB). */
#ifndef ABS3DS_DBG_H
#define ABS3DS_DBG_H
void dbg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#endif
