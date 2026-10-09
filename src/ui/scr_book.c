/* Book details: play/resume, download, chapters count, description. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abs_client.h"
#include "anim.h"
#include "app.h"
#include "decoder.h"
#include "downloads.h"
#include "gfx.h"
#include "player.h"
#include "strutil.h"
#include "worker.h"

#define MAXL 80
static char g_lines[MAXL][160];
static int g_nlines;
static int g_focus;
static float g_scroll, g_vel, g_ring_y;
static BookDetail g_detail;
static bool g_have_detail;
static bool g_supported = true;
static int g_confirm_delete;

typedef struct { char id[40]; BookDetail d; int rc; } DetailJob;
static DetailJob *g_done;
static LightLock g_lock;
static bool g_lock_init;

static void detail_job(void *ud) {
    DetailJob *j = ud;
    j->rc = absc_fetch_item(j->id, &j->d);
    if (j->rc != ABS_OK) j->rc = dl_load_meta(j->id, &j->d) == 0 ? ABS_OK : j->rc;
    LightLock_Lock(&g_lock);
    if (g_done) { book_detail_free(&g_done->d); free(g_done); }
    g_done = j;
    LightLock_Unlock(&g_lock);
}

static void enter(void) {
    if (!g_lock_init) { LightLock_Init(&g_lock); g_lock_init = true; }
    if (g_have_detail) book_detail_free(&g_detail);
    g_have_detail = false;
    g_nlines = 0;
    g_scroll = g_vel = 0;
    g_focus = 0;
    g_ring_y = 0;
    g_supported = true;
    g_confirm_delete = 0;
    app_focus_book(g_sel.id);
    DetailJob *j = calloc(1, sizeof *j);
    if (!j) return;
    str_copy(j->id, sizeof j->id, g_sel.id);
    worker_post(g_api, detail_job, j);
}

static void take_detail(void) {
    LightLock_Lock(&g_lock);
    DetailJob *j = g_done;
    g_done = NULL;
    LightLock_Unlock(&g_lock);
    if (!j) return;
    if (j->rc == ABS_OK && !strcmp(j->id, g_sel.id)) {
        g_detail = j->d;
        g_have_detail = true;
        for (int i = 0; i < g_detail.ntracks; i++)
            if (!stream_supported(g_detail.tracks[i].ext, g_detail.tracks[i].codec)) g_supported = false;
        g_nlines = g_detail.description ? gfx_wrap(F_SYS, g_detail.description, 13, 288, g_lines, MAXL) : 0;
    } else {
        book_detail_free(&j->d);
    }
    free(j);
}

static uint64_t total_size(void) {
    uint64_t t = 0;
    for (int i = 0; i < g_detail.ntracks; i++) t += g_detail.tracks[i].size;
    return t;
}

static void do_download(void) {
    float pct;
    DlState st = dl_state(g_sel.id, &pct);
    if (st == DL_DONE) {
        if (!g_confirm_delete) { g_confirm_delete = 1; return; }
        if (player_has_book() && !strcmp(player_book_id(), g_sel.id)) { app_toast("Stop playback before deleting"); return; }
        dl_delete(g_sel.id);
        g_confirm_delete = 0;
        app_toast("Download removed");
        return;
    }
    if (st == DL_ACTIVE || st == DL_QUEUED) { dl_cancel(g_sel.id); app_toast("Download paused"); return; }
    if (!g_have_detail) { app_toast("Still loading details\xE2\x80\xA6"); return; }
    if (!g_supported) { app_toast("Format not supported"); return; }
    if (dl_start(&g_detail) == 0) app_toast("Download started");
    else app_toast("Couldn't start the download");
}

#define BTN_PLAY_Y 38.0f
#define BTN_DL_Y 86.0f
#define DESC_Y 160.0f

static void update(const Input *in, float dt) {
    take_detail();
    if (app_mini_player_input(in)) return;
    if (in->down & KEY_B) { app_pop(); return; }
    if (in->repeat & (KEY_UP | KEY_CPAD_UP)) { if (g_focus == 1) g_focus = 0; else g_scroll -= 40; }
    if (in->repeat & (KEY_DOWN | KEY_CPAD_DOWN)) { if (g_focus == 0) g_focus = 1; else g_scroll += 40; }
    if (in->down & KEY_A) {
        if (g_focus == 0) {
            if (!g_supported) app_toast("Format not supported");
            else app_play_book(&g_sel);
            return;
        }
        do_download();
    }
    if (in->tap) {
        if (hit(in, 0, 0, 90, 30)) { app_pop(); return; }
        if (hit(in, 12, BTN_PLAY_Y, 296, 42)) { g_focus = 0; if (g_supported) app_play_book(&g_sel); else app_toast("Format not supported"); return; }
        if (hit(in, 12, BTN_DL_Y, 296, 36)) { g_focus = 1; do_download(); }
    }
    if (g_focus != 1) g_confirm_delete = 0;
    if (in->touching && in->dragging && in->sy > DESC_Y - 20) { g_scroll -= in->dy; g_vel = 0; }
    if (in->touch_up && in->dragging) g_vel = -in->fling_vy;
    if (!in->touching) { g_scroll += g_vel * dt; g_vel *= expf(-4.5f * dt); }
    float view = 240 - DESC_Y - app_mini_player_height() - 8;
    float maxs = g_nlines * 17 - view;
    if (maxs < 0) maxs = 0;
    if (g_scroll < 0) g_scroll = approach(g_scroll, 0, dt, 14);
    if (g_scroll > maxs) g_scroll = approach(g_scroll, maxs, dt, 14);
    float ty = g_focus == 0 ? BTN_PLAY_Y : BTN_DL_Y;
    g_ring_y = g_ring_y == 0 ? ty : approach(g_ring_y, ty, dt, 20);
}

static void draw_top(void) {
    app_draw_backdrop(g_sel.id, 0.6f);
    app_draw_status_bar(NULL);
    BookSummary b = g_sel;
    BookSummary *live = lib_get(g_sel.id);
    if (live) b = *live;
    app_draw_top_book(&b);
}

static void draw_bottom(void) {
    gfx_rect(0, 0, 320, 240, COL_BG);
    u32 acc = app_accent();
    /* description first (it scrolls under the buttons) */
    float y = DESC_Y - g_scroll;
    if (!g_have_detail) {
        float t = app_time() * 5;
        for (int i = 0; i < 3; i++) gfx_circle(148 + i * 12, DESC_Y + 30, 3.5f, col_alpha(COL_DIM, 0.4f + 0.6f * (0.5f + 0.5f * sinf(t - i))));
    }
    for (int i = 0; i < g_nlines; i++, y += 17) {
        if (y < DESC_Y - 20) continue;
        if (y > 240) break;
        gfx_text(F_SYS, g_lines[i], 16, y, 13, COL_DIM, AL_LEFT);
    }
    gfx_rect(0, 0, 320, DESC_Y - 6, COL_BG);
    gfx_gradient_v(0, DESC_Y - 6, 320, 12, RGBA(14, 17, 22, 255), RGBA(14, 17, 22, 0));

    /* header */
    gfx_icon(IC_CHEVRON_LEFT, 14, 15, 14, COL_DIM);
    gfx_text(F_UI, "Back", 24, 7, 14, COL_DIM, AL_LEFT);
    gfx_text_fit(F_SYS, g_sel.title, 308, 8, 13, 200, COL_FAINT, AL_RIGHT);

    /* play button */
    bool resume = g_sel.current_time > 5 && !g_sel.finished;
    char lbl[64];
    if (!g_supported) snprintf(lbl, sizeof lbl, "Format not supported");
    else if (resume) {
        char rem[24];
        fmt_duration(g_sel.duration - g_sel.current_time, rem, sizeof rem);
        snprintf(lbl, sizeof lbl, "Resume \xC2\xB7 %s left", rem);
    } else snprintf(lbl, sizeof lbl, g_sel.finished ? "Listen again" : "Play");
    gfx_rrect(12, BTN_PLAY_Y, 296, 42, 21, g_supported ? acc : COL_SURFACE2);
    float lw = gfx_text_width(F_BOLD, lbl, 16);
    if (g_supported) gfx_icon(IC_PLAY, 160 - lw / 2 - 12, BTN_PLAY_Y + 21, 14, COL_BG);
    gfx_text(F_BOLD, lbl, 160 + (g_supported ? 6 : 0), BTN_PLAY_Y + 11, 16, g_supported ? COL_BG : COL_DIM, AL_CENTER);

    /* download button */
    float pct = 0;
    DlState st = dl_state(g_sel.id, &pct);
    gfx_rrect(12, BTN_DL_Y, 296, 36, 18, COL_SURFACE);
    if (st == DL_ACTIVE || st == DL_QUEUED || (st == DL_PAUSED && pct > 0))
        gfx_rrect(12, BTN_DL_Y, fmaxf(36, 296 * pct), 36, 18, col_alpha(acc, 0.35f));
    char dl[64];
    IconId ic = IC_DOWNLOAD;
    uint64_t size = total_size();
    if (st == DL_DONE) { snprintf(dl, sizeof dl, g_confirm_delete ? "Press again to delete" : "Downloaded \xC2\xB7 delete"); ic = g_confirm_delete ? IC_TRASH : IC_CHECK; }
    else if (st == DL_ACTIVE) snprintf(dl, sizeof dl, "Downloading %d%% \xC2\xB7 tap to pause", (int)(pct * 100));
    else if (st == DL_QUEUED) snprintf(dl, sizeof dl, "Waiting to download\xE2\x80\xA6");
    else if (st == DL_PAUSED) snprintf(dl, sizeof dl, "Resume download (%d%%)", (int)(pct * 100));
    else if (size) snprintf(dl, sizeof dl, "Download \xC2\xB7 %.0f MB", size / 1e6);
    else snprintf(dl, sizeof dl, "Download");
    float dw = gfx_text_width(F_UI, dl, 14);
    gfx_icon(ic, 160 - dw / 2 - 12, BTN_DL_Y + 18, 13, st == DL_DONE && !g_confirm_delete ? COL_OK : COL_TEXT);
    gfx_text(F_UI, dl, 160 + 6, BTN_DL_Y + 9, 14, g_confirm_delete ? COL_DANGER : COL_TEXT, AL_CENTER);

    /* focus ring */
    float rh = g_focus == 0 ? 42 : 36;
    gfx_rrect_outline(9, g_ring_y - 3, 302, rh + 6, rh / 2 + 3, 2, col_alpha(COL_TEXT, 0.7f), RGBA(0, 0, 0, 0));

    /* meta */
    if (g_have_detail) {
        char meta[160];
        char dur[24];
        fmt_duration(g_detail.duration, dur, sizeof dur);
        snprintf(meta, sizeof meta, "%d chapters \xC2\xB7 %s%s%s", g_detail.nchapters, dur, g_detail.narrator[0] ? " \xC2\xB7 Read by " : "", g_detail.narrator);
        gfx_text_fit(F_SYS, meta, 16, 132, 12, 290, COL_FAINT, AL_LEFT);
    }
}

const Screen scr_book = {enter, update, draw_top, draw_bottom};
