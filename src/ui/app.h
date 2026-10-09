/* App shell: screens, navigation, input, shared library model. */
#ifndef ABS3DS_APP_H
#define ABS3DS_APP_H
#include <3ds.h>
#include <stdbool.h>

#include "config.h"
#include "library.h"
#include "models.h"
#include "progress_queue.h"

typedef struct {
    u32 down, held, repeat;
    bool touch_down, touching, touch_up, tap;
    float tx, ty;       /* current (or last) touch point */
    float sx, sy;       /* where the touch started */
    float dx, dy;       /* movement this frame */
    bool dragging;      /* moved past the tap threshold */
    float fling_vy;     /* release velocity (px/s), on touch_up */
} Input;

typedef enum { SCR_LOGIN, SCR_HOME, SCR_BOOK, SCR_PLAYER, SCR_COUNT } ScreenId;

typedef struct {
    void (*enter)(void);
    void (*update)(const Input *in, float dt);
    void (*draw_top)(void);
    void (*draw_bottom)(void);
} Screen;

extern const Screen scr_login, scr_home, scr_book, scr_player;

void app_push(ScreenId s);
void app_pop(void);
void app_replace(ScreenId s);
void app_toast(const char *msg);
int app_run(void);

/* shared state */
extern Config g_cfg;
extern ProgressQueue g_pq;
void app_save_cfg(void);
void app_save_queue(void);

typedef struct {
    BookSummary *books;
    int n;
    int *order;          /* sorted + filtered indices */
    int norder;
    SortMode sort;
    char query[64];
    char cont[24][40];   /* continue listening ids, most recent first */
    int ncont;
    bool loading, loaded, offline;
    int load_done, load_total;
    char error[96];
} Library;
extern Library g_lib;
void lib_reload(void);
void lib_apply(void);    /* re-sort/filter after changing sort/query */
BookSummary *lib_get(const char *id);

/* the book opened in the details screen */
extern BookSummary g_sel;

/* top-screen helpers */
void app_focus_book(const char *id);      /* drives the accent colour */
u32 app_accent(void);
void app_draw_backdrop(const char *id, float dim);
void app_draw_status_bar(const char *title);
void app_draw_top_book(const BookSummary *b); /* hero layout used by home/book screens */
/* bottom-screen mini player; returns its height (0 when hidden) */
float app_mini_player_height(void);
void app_draw_mini_player(void);
bool app_mini_player_input(const Input *in);
/* swkbd wrapper */
bool app_keyboard(const char *hint, const char *initial, bool password, char *out, int n);
bool hit(const Input *in, float x, float y, float w, float h);
void app_open_book(const BookSummary *b);   /* -> details screen */
void app_play_book(const BookSummary *b);   /* -> player screen */
float app_time(void);                       /* seconds since start */

#endif
