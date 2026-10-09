/* Tiny append-only log at sdmc:/3ds/shelf3ds/debug.log (rotated at 256 KB). */
#ifndef SHELF3DS_DBG_H
#define SHELF3DS_DBG_H
void dbg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#endif
