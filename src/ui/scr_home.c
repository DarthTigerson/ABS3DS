/* Home: tabs Continue / Library / Downloads / Settings. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abs_client.h"
#include "anim.h"
#include "app.h"
#include "covers.h"
#include "downloads.h"
#include "engine.h"
#include "gfx.h"
#include "player.h"
#include "strutil.h"
#include "sys.h"

enum { TAB_CONTINUE, TAB_LIBRARY, TAB_DOWNLOADS, TAB_SETTINGS, NTABS };
static const char *kTabs[NTABS] = {"Continue", "Library", "Downloads", "Settings"};

#define TAB_H 32.0f
#define GRID_COLS 3
#define CELL_W (320.0f / GRID_COLS)
#define CELL_H 124.0f
#define COVER 88.0f
#define HEAD_H 30.0f
#define ROW_H 62.0f

static int g_tab = TAB_LIBRARY;
static float g_tab_x;               /* animated indicator */
static int g_focus[NTABS];
static float g_scroll[NTABS], g_scroll_v[NTABS];
static float g_ring_x, g_ring_y;    /* animated focus ring (library) */
static int g_confirm_logout;
static float g_content_alpha = 1;
static DlInfo g_dl[64];
static int g_ndl;

static float content_bottom(void) { return 240 - app_mini_player_height(); }

static void enter(void) {
    if (!g_lib.loaded && g_lib.ncont == 0 && g_tab == TAB_CONTINUE) g_tab = TAB_LIBRARY;
    g_tab_x = -1;
}

/* ---- item helpers ------------------------------------------------------------------ */

static int item_count(void) {
    switch (g_tab) {
    case TAB_CONTINUE: return g_lib.ncont;
    case TAB_LIBRARY: return g_lib.norder;
    case TAB_DOWNLOADS: return g_ndl;
    default: return 7;
    }
}

static BookSummary *cont_book(int i) { return i < g_lib.ncont ? lib_get(g_lib.cont[i]) : NULL; }
static BookSummary *lib_book(int i) { return i < g_lib.norder ? &g_lib.books[g_lib.order[i]] : NULL; }

static BookSummary *focused_book(void) {
    int f = g_focus[g_tab];
    if (g_tab == TAB_CONTINUE) return cont_book(f);
    if (g_tab == TAB_LIBRARY) return lib_book(f);
    if (g_tab == TAB_DOWNLOADS && f < g_ndl) {
        BookSummary *b = lib_get(g_dl[f].id);
        if (b) return b;
        static BookSummary tmp;
        memset(&tmp, 0, sizeof tmp);
        str_copy(tmp.id, sizeof tmp.id, g_dl[f].id);
        str_copy(tmp.title, sizeof tmp.title, g_dl[f].title);
        str_copy(tmp.author, sizeof tmp.author, g_dl[f].author);
        return &tmp;
    }
    return NULL;
}

/* content geometry: y of item i relative to the content top (before scroll) */
#define SET_H 44.0f
static float row_h(void) { return g_tab == TAB_SETTINGS ? SET_H : ROW_H; }
static float item_y(int i) {
    if (g_tab == TAB_LIBRARY) return HEAD_H + (i / GRID_COLS) * CELL_H;
    return 6 + i * row_h();
}
static float item_h(void) { return g_tab == TAB_LIBRARY ? CELL_H : row_h(); }
static float content_h(void) {
    int n = item_count();
    if (g_tab == TAB_LIBRARY) return HEAD_H + ((n + GRID_COLS - 1) / GRID_COLS) * CELL_H + 8;
    return 12 + n * row_h();
}

static void keep_visible(int i) {
    float view = content_bottom() - TAB_H;
    float y = item_y(i), h = item_h();
    float *s = &g_scroll[g_tab];
    if (y - (g_tab == TAB_LIBRARY ? HEAD_H : 0) < *s) *s = y - (g_tab == TAB_LIBRARY ? HEAD_H : 6);
    if (y + h > *s + view) *s = y + h - view + 6;
}

/* ---- settings ----------------------------------------------------------------------- */

static void setting_label(int i, char *name, char *val, int n) {
    switch (i) {
    case 0: strcpy(name, "Skip back"); snprintf(val, n, "%ds", g_cfg.skip_back); break;
    case 1: strcpy(name, "Skip forward"); snprintf(val, n, "%ds", g_cfg.skip_fwd); break;
    case 2: strcpy(name, "Playback speed"); snprintf(val, n, "%.2g\xC3\x97", g_cfg.speed); break;
    case 3: strcpy(name, "Sync progress every"); snprintf(val, n, "%ds", g_cfg.sync_interval); break;
    case 4: strcpy(name, "Screen off when lid closes"); snprintf(val, n, "%s", g_cfg.screen_off ? "On" : "Off"); break;
    case 5: strcpy(name, g_confirm_logout ? "Press A again to log out" : "Log out"); snprintf(val, n, "%s", g_cfg.username); break;
    default: {
        strcpy(name, "About");
        double gb = dl_total_bytes() / 1e9;
        snprintf(val, n, "v" APP_VERSION " \xC2\xB7 %.1f GB \xC2\xB7 CPU %d%%", gb, (int)(engine_cpu_load() * 100));
    }
    }
}

static void setting_activate(int i) {
    switch (i) {
    case 0: g_cfg.skip_back = cfg_next_skip(g_cfg.skip_back); break;
    case 1: g_cfg.skip_fwd = cfg_next_skip(g_cfg.skip_fwd); break;
    case 2: player_set_speed(cfg_next_speed(g_cfg.speed)); break;
    case 3: g_cfg.sync_interval = g_cfg.sync_interval >= 60 ? 10 : g_cfg.sync_interval >= 30 ? 60 : g_cfg.sync_interval >= 15 ? 30 : 15; break;
    case 4: g_cfg.screen_off = !g_cfg.screen_off; break;
    case 5:
        if (!g_confirm_logout) { g_confirm_logout = 1; return; }
        g_confirm_logout = 0;
        player_exit();
        absc_logout();
        app_replace(SCR_LOGIN);
        return;
    default: return;
    }
    app_save_cfg();
}

/* ---- update ----------------------------------------------------------------------- */

static void activate(int i) {
    if (g_tab == TAB_SETTINGS) { setting_activate(i); return; }
    BookSummary *b = g_tab == TAB_CONTINUE ? cont_book(i) : g_tab == TAB_LIBRARY ? lib_book(i) : NULL;
    if (g_tab == TAB_DOWNLOADS && i < g_ndl) {
        b = focused_book();
        if (g_dl[i].state == DL_DONE) { app_play_book(b); return; }
        app_open_book(b);
        return;
    }
    if (!b) return;
    if (g_tab == TAB_CONTINUE) app_play_book(b);
    else app_open_book(b);
}

static void switch_tab(int t) {
    if (t < 0) t = NTABS - 1;
    if (t >= NTABS) t = 0;
    if (t == g_tab) return;
    g_tab = t;
    g_confirm_logout = 0;
    g_content_alpha = 0;
}

static int item_at(float x, float y) {
    float cy = y - TAB_H + g_scroll[g_tab];
    int n = item_count();
    if (g_tab == TAB_LIBRARY) {
        if (cy < HEAD_H) return -1;
        int row = (int)((cy - HEAD_H) / CELL_H), col = (int)(x / CELL_W);
        int i = row * GRID_COLS + col;
        return i < n ? i : -1;
    }
    int i = (int)((cy - 6) / row_h());
    return cy >= 6 && i < n ? i : -1;
}

static void update(const Input *in, float dt) {
    if (g_tab == TAB_DOWNLOADS || app_time() < 1) g_ndl = dl_list(g_dl, 64);
    if (app_mini_player_input(in)) return;
    if (in->down & KEY_L) switch_tab(g_tab - 1);
    if (in->down & KEY_R) switch_tab(g_tab + 1);
    if (in->tap && in->ty < TAB_H) { switch_tab((int)(in->tx / 80)); return; }

    int n = item_count();
    int *f = &g_focus[g_tab];
    if (*f >= n) *f = n > 0 ? n - 1 : 0;
    int step = g_tab == TAB_LIBRARY ? GRID_COLS : 1;
    if (in->repeat & (KEY_DOWN | KEY_CPAD_DOWN)) { if (*f + step < n) *f += step; else if (n) *f = n - 1; keep_visible(*f); }
    if (in->repeat & (KEY_UP | KEY_CPAD_UP)) { if (*f - step >= 0) *f -= step; keep_visible(*f); }
    if (g_tab == TAB_LIBRARY) {
        if (in->repeat & (KEY_RIGHT | KEY_CPAD_RIGHT)) { if (*f + 1 < n) (*f)++; keep_visible(*f); }
        if (in->repeat & (KEY_LEFT | KEY_CPAD_LEFT)) { if (*f > 0) (*f)--; keep_visible(*f); }
        if (in->down & KEY_Y) {
            g_lib.sort = (g_lib.sort + 1) % SORT_COUNT;
            lib_apply();
            char m[48];
            snprintf(m, sizeof m, "Sorted by %s", lib_sort_name(g_lib.sort));
            app_toast(m);
            *f = 0;
            g_scroll[g_tab] = 0;
        }
        if (in->down & KEY_X) {
            char q[64];
            if (app_keyboard("Search title or author", g_lib.query, false, q, sizeof q)) {
                str_copy(g_lib.query, sizeof g_lib.query, q);
                lib_apply();
                *f = 0;
                g_scroll[g_tab] = 0;
            }
        }
        if ((in->down & KEY_B) && g_lib.query[0]) { g_lib.query[0] = 0; lib_apply(); }
    }
    if (g_tab == TAB_DOWNLOADS && *f < g_ndl) {
        if (in->down & KEY_X) {
            dl_delete(g_dl[*f].id);
            app_toast("Download removed");
        }
        if ((in->down & KEY_Y) && g_dl[*f].state == DL_PAUSED) {
            BookDetail d;
            if (dl_load_meta(g_dl[*f].id, &d) == 0) { dl_start(&d); book_detail_free(&d); app_toast("Resuming download"); }
        }
    }
    if (in->down & KEY_A) activate(*f);
    if ((in->down & KEY_SELECT) && !g_lib.loading) { lib_reload(); app_toast("Refreshing library"); }
    if ((in->down & KEY_START) && player_has_book()) app_push(SCR_PLAYER);
    if (!(in->held & KEY_A) && g_tab != TAB_SETTINGS) g_confirm_logout = 0;

    /* touch: drag to scroll with inertia, tap to open */
    float *s = &g_scroll[g_tab], *v = &g_scroll_v[g_tab];
    if (in->touching && in->dragging && in->sy > TAB_H) { *s -= in->dy; *v = 0; }
    if (in->touch_up && in->dragging) *v = -in->fling_vy;
    if (in->tap && in->ty > TAB_H && in->ty < content_bottom()) {
        if (g_tab == TAB_LIBRARY && in->ty - TAB_H + *s < HEAD_H) {
            if (in->tx < 110) { g_lib.sort = (g_lib.sort + 1) % SORT_COUNT; lib_apply(); }
            else if (in->tx > 210) {
                char q[64];
                if (app_keyboard("Search title or author", g_lib.query, false, q, sizeof q)) { str_copy(g_lib.query, sizeof g_lib.query, q); lib_apply(); }
            }
        } else {
            int i = item_at(in->tx, in->ty);
            if (i >= 0) { *f = i; activate(i); }
        }
    }
    if (!in->touching) {
        *s += *v * dt;
        *v *= expf(-4.5f * dt);
        if (fabsf(*v) < 5) *v = 0;
    }
    float maxs = content_h() - (content_bottom() - TAB_H);
    if (maxs < 0) maxs = 0;
    if (*s < 0) { *s = approach(*s, 0, dt, 14); if (!in->touching) *v = 0; }
    if (*s > maxs) { *s = approach(*s, maxs, dt, 14); if (!in->touching) *v = 0; }

    float tx = g_tab * 80.0f;
    g_tab_x = g_tab_x < 0 ? tx : approach(g_tab_x, tx, dt, 16);
    g_content_alpha = approach(g_content_alpha, 1, dt, 10);
    BookSummary *b = focused_book();
    if (b) app_focus_book(b->id);
    if (g_tab == TAB_LIBRARY && n) {
        float rx = (*f % GRID_COLS) * CELL_W + (CELL_W - COVER) / 2, ry = item_y(*f);
        g_ring_x = g_ring_x == 0 && g_ring_y == 0 ? rx : approach(g_ring_x, rx, dt, 20);
        g_ring_y = g_ring_y == 0 ? ry : approach(g_ring_y, ry, dt, 20);
    }
}

/* ---- drawing --------------------------------------------------------------------------- */

static void draw_top(void) {
    BookSummary *b = focused_book();
    if (b) {
        app_draw_backdrop(b->id, 0.62f);
        app_draw_status_bar(NULL);
        app_draw_top_book(b);
        return;
    }
    app_draw_backdrop("", 0);
    app_draw_status_bar(NULL);
    const char *msg = g_lib.loading ? "Loading your library\xE2\x80\xA6" : g_lib.error[0] ? g_lib.error :
                      g_tab == TAB_CONTINUE ? "Nothing in progress yet" : g_tab == TAB_SETTINGS ? "Settings" : "No books here";
    gfx_icon(g_tab == TAB_SETTINGS ? IC_GEAR : IC_BOOK, 200, 100, 48, col_alpha(app_accent(), 0.8f));
    gfx_text(F_BOLD, msg, 200, 140, 18, COL_TEXT, AL_CENTER);
    if (g_lib.loading && g_lib.load_total) {
        char p[32];
        snprintf(p, sizeof p, "%d / %d", g_lib.load_done, g_lib.load_total);
        gfx_progress(130, 172, 140, 4, (float)g_lib.load_done / g_lib.load_total, app_accent(), col_alpha(COL_TEXT, 0.12f));
        gfx_text(F_UI, p, 200, 182, 13, COL_DIM, AL_CENTER);
    }
}

static void draw_cover(const BookSummary *b, float x, float y, float s, float alpha) {
    CoverState st;
    const C2D_Image *img = covers_thumb(b->id, &st);
    gfx_shadow(x, y + 3, s, s, 8, 0.45f * alpha);
    if (img) gfx_image(img, x, y, s, s, col_alpha(RGBA(255, 255, 255, 255), alpha), 0);
    else gfx_placeholder_cover(b->title, x, y, s);
}

static void draw_library(float top, float a) {
    float s = g_scroll[TAB_LIBRARY];
    /* header: sort + search chips */
    float hy = top - s + 4;
    if (hy > top - HEAD_H) {
        char lbl[48];
        snprintf(lbl, sizeof lbl, "%s", lib_sort_name(g_lib.sort));
        float w = gfx_text_width(F_UI, lbl, 13) + 34;
        gfx_rrect(8, hy, w, 22, 11, COL_SURFACE);
        gfx_icon(IC_SORT, 20, hy + 11, 12, COL_DIM);
        gfx_text(F_UI, lbl, 30, hy + 4, 13, COL_TEXT, AL_LEFT);
        const char *q = g_lib.query[0] ? g_lib.query : "Search";
        float qw = gfx_text_width(F_SYS, q, 13) + 34;
        if (qw > 150) qw = 150;
        gfx_rrect(312 - qw, hy, qw, 22, 11, g_lib.query[0] ? col_alpha(app_accent(), 0.3f) : COL_SURFACE);
        gfx_icon(IC_SEARCH, 312 - qw + 13, hy + 11, 12, COL_DIM);
        gfx_text_fit(F_SYS, q, 312 - qw + 24, hy + 4, 13, qw - 30, g_lib.query[0] ? COL_TEXT : COL_DIM, AL_LEFT);
        char cnt[24];
        snprintf(cnt, sizeof cnt, "%d", g_lib.norder);
        gfx_text(F_UI, cnt, 160, hy + 5, 12, COL_FAINT, AL_CENTER);
    }
    int n = g_lib.norder;
    if (n) gfx_focus_ring(g_ring_x, top + g_ring_y - s + 4, COVER, COVER, 4, 3, app_accent());
    int first = (int)((s - HEAD_H) / CELL_H) * GRID_COLS;
    if (first < 0) first = 0;
    float bottom = content_bottom();
    for (int i = first; i < n; i++) {
        float y = top + item_y(i) - s;
        if (y > bottom) break;
        if (y + CELL_H < top) continue;
        BookSummary *b = lib_book(i);
        float x = (i % GRID_COLS) * CELL_W + (CELL_W - COVER) / 2;
        bool f = i == g_focus[TAB_LIBRARY];
        draw_cover(b, x, y + 4, COVER, a);
        if (b->progress > 0.01f && !b->finished) gfx_progress(x + 4, y + COVER - 3, COVER - 8, 3, b->progress, app_accent(), RGBA(0, 0, 0, 150));
        if (dl_is_done(b->id)) {
            gfx_circle(x + COVER - 9, y + 13, 8, app_accent());
            gfx_icon(IC_DOWNLOAD, x + COVER - 9, y + 13, 10, COL_BG);
        }
        gfx_text_fit(F_SYS, b->title, x + COVER / 2, y + COVER + 8, 12, COVER + 10, f ? COL_TEXT : COL_DIM, AL_CENTER);
        gfx_text_fit(F_SYS, b->author, x + COVER / 2, y + COVER + 22, 10.5f, COVER + 6, COL_FAINT, AL_CENTER);
    }
    if (!n && !g_lib.loading) {
        gfx_text(F_UI, g_lib.query[0] ? "No matches. Press B to clear." : "Your library is empty", 160, top + 70, 14, COL_DIM, AL_CENTER);
    }
}

static void row_frame(int i, float y, bool f) {
    gfx_rrect(8, y, 304, ROW_H - 6, 12, f ? COL_SURFACE2 : COL_SURFACE);
    if (f) gfx_rect(8, y + 12, 3, ROW_H - 30, app_accent());
    (void)i;
}

static void draw_continue(float top, float a) {
    float s = g_scroll[TAB_CONTINUE];
    for (int i = 0; i < g_lib.ncont; i++) {
        float y = top + item_y(i) - s;
        if (y > content_bottom()) break;
        if (y + ROW_H < top) continue;
        BookSummary *b = cont_book(i);
        if (!b) continue;
        bool f = i == g_focus[TAB_CONTINUE];
        row_frame(i, y, f);
        draw_cover(b, 16, y + 5, 46, a);
        gfx_text_fit(F_SYS, b->title, 72, y + 7, 14, 196, COL_TEXT, AL_LEFT);
        gfx_text_fit(F_SYS, b->author, 72, y + 24, 12, 196, COL_DIM, AL_LEFT);
        gfx_progress(72, y + 42, 150, 4, b->progress, app_accent(), col_alpha(COL_TEXT, 0.12f));
        char rem[24];
        fmt_duration(b->duration * (1 - b->progress), rem, sizeof rem);
        gfx_text(F_UI, rem, 302, y + 37, 12, COL_FAINT, AL_RIGHT);
        gfx_circle(286, y + 18, 12, f ? app_accent() : col_alpha(COL_TEXT, 0.1f));
        gfx_icon(IC_PLAY, 287, y + 18, 10, f ? COL_BG : COL_DIM);
    }
    if (!g_lib.ncont) {
        gfx_icon(IC_HEADPHONES, 160, top + 60, 36, COL_FAINT);
        gfx_text(F_UI, g_lib.loading ? "Loading\xE2\x80\xA6" : "Start a book from your Library", 160, top + 88, 14, COL_DIM, AL_CENTER);
    }
}

static void draw_downloads(float top, float a) {
    float s = g_scroll[TAB_DOWNLOADS];
    for (int i = 0; i < g_ndl; i++) {
        float y = top + item_y(i) - s;
        if (y > content_bottom()) break;
        if (y + ROW_H < top) continue;
        DlInfo *d = &g_dl[i];
        bool f = i == g_focus[TAB_DOWNLOADS];
        row_frame(i, y, f);
        BookSummary tmp = {0};
        str_copy(tmp.id, sizeof tmp.id, d->id);
        str_copy(tmp.title, sizeof tmp.title, d->title);
        draw_cover(&tmp, 16, y + 5, 46, a);
        gfx_text_fit(F_SYS, d->title, 72, y + 7, 14, 230, COL_TEXT, AL_LEFT);
        char st[96], mb[32];
        snprintf(mb, sizeof mb, "%.0f MB", d->total_bytes / 1e6);
        float pct = d->total_bytes ? (float)((double)d->done_bytes / d->total_bytes) : 0;
        u32 sc = COL_DIM;
        switch (d->state) {
        case DL_DONE: snprintf(st, sizeof st, "Downloaded \xC2\xB7 %s", mb); sc = COL_OK; break;
        case DL_ACTIVE: snprintf(st, sizeof st, "Downloading %d%% \xC2\xB7 %s", (int)(pct * 100), mb); sc = app_accent(); break;
        case DL_QUEUED: snprintf(st, sizeof st, "Waiting \xC2\xB7 %s", mb); break;
        default: snprintf(st, sizeof st, "%s \xC2\xB7 Y to resume", d->error[0] ? d->error : "Paused"); sc = COL_DANGER; break;
        }
        gfx_text_fit(F_UI, st, 72, y + 25, 12, 230, sc, AL_LEFT);
        if (d->state != DL_DONE) gfx_progress(72, y + 43, 220, 4, pct, app_accent(), col_alpha(COL_TEXT, 0.12f));
        if (f) gfx_text(F_UI, "X delete", 302, y + 7, 11, COL_FAINT, AL_RIGHT);
    }
    if (!g_ndl) {
        gfx_icon(IC_DOWNLOAD, 160, top + 60, 34, COL_FAINT);
        gfx_text(F_UI, "Download books to listen offline", 160, top + 88, 14, COL_DIM, AL_CENTER);
    }
}

static void draw_settings(float top) {
    float s = g_scroll[TAB_SETTINGS];
    for (int i = 0; i < 7; i++) {
        float y = top + item_y(i) - s;
        if (y > content_bottom()) break;
        bool f = i == g_focus[TAB_SETTINGS];
        char name[64], val[96];
        setting_label(i, name, val, sizeof val);
        gfx_rrect(8, y, 304, 38, 10, f ? COL_SURFACE2 : COL_SURFACE);
        u32 nc = i == 5 && g_confirm_logout ? COL_DANGER : COL_TEXT;
        gfx_text(F_UI, name, 20, y + 10, 14, nc, AL_LEFT);
        gfx_text_fit(i == 5 ? F_SYS : F_UI, val, 300, y + 10, 14, 150, i < 5 ? app_accent() : COL_DIM, AL_RIGHT);
    }
}

static void draw_bottom(void) {
    gfx_rect(0, 0, 320, 240, COL_BG);
    float a = g_content_alpha;
    float top = TAB_H;
    switch (g_tab) {
    case TAB_CONTINUE: draw_continue(top, a); break;
    case TAB_LIBRARY: draw_library(top, a); break;
    case TAB_DOWNLOADS: draw_downloads(top, a); break;
    default: draw_settings(top); break;
    }
    if (a < 1) gfx_rect(0, top, 320, 240 - top, RGBA(14, 17, 22, (u8)(255 * (1 - a))));
    /* tab bar drawn last so content scrolls underneath */
    gfx_rect(0, 0, 320, TAB_H, COL_BG);
    gfx_gradient_v(0, TAB_H, 320, 8, RGBA(14, 17, 22, 255), RGBA(14, 17, 22, 0));
    for (int i = 0; i < NTABS; i++) {
        bool on = i == g_tab;
        gfx_text(on ? F_BOLD : F_UI, kTabs[i], i * 80 + 40, 7, 14, on ? COL_TEXT : COL_DIM, AL_CENTER);
    }
    float iw = 28;
    gfx_rrect(g_tab_x + 40 - iw / 2, TAB_H - 5, iw, 3, 1.5f, app_accent());
    if (g_lib.loading) {
        float t = fmodf(app_time() * 1.2f, 1.0f);
        gfx_rect(0, TAB_H - 1, 320, 1, col_alpha(COL_TEXT, 0.06f));
        gfx_rect(t * 400 - 80, TAB_H - 1, 80, 1, app_accent());
    }
}

const Screen scr_home = {enter, update, draw_top, draw_bottom};
