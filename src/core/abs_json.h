/* Audiobookshelf (v2.31) JSON <-> plain C structs. All parsers tolerate
 * missing or null fields and return 0 on success, -1 on malformed JSON. */
#ifndef ABS3DS_ABS_JSON_H
#define ABS3DS_ABS_JSON_H
#include <stddef.h>

#include "models.h"

int abs_parse_login(const char *json, char *access, size_t an, char *refresh, size_t rn, char *user_id, size_t un);
/* id of the first library with mediaType "book" */
int abs_parse_libraries(const char *json, char *book_lib_id, size_t n);
/* Appends up to max-*count summaries to out; *total gets the server total. */
int abs_parse_items_page(const char *json, BookSummary *out, int max, int *count, int *total);
/* Merges mediaProgress[] (from /api/me or the login user) into books by id. */
int abs_parse_me_progress(const char *json, BookSummary *books, int n);
/* Ids from /api/me/items-in-progress, most recent first. Returns count. */
int abs_parse_in_progress(const char *json, char ids[][40], int max);
int abs_parse_item(const char *json, BookDetail *out);
int abs_parse_play(const char *json, PlaySession *out);
/* When a book has no chapters, uses one chapter per track (titled from the file name). */
void abs_chapters_from_tracks(BookDetail *b);

/* Request bodies. Return the length written (or -1 if it didn't fit). */
int abs_build_login_body(const char *user, const char *pass, char *out, size_t n);
int abs_build_play_body(const char *device_id, const char *model, const char *version, char *out, size_t n);
int abs_build_sync_body(double current_time, double listened, double duration, char *out, size_t n);
int abs_build_progress_body(double current_time, double duration, int finished, char *out, size_t n);

#endif
