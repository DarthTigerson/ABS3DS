/* Now Playing. Top: cover, chapter, progress. Bottom: transport controls,
 * chapter scrubber, speed / sleep / chapters / screen-off chips, and a
 * slide-up chapter list. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "anim.h"
#include "app.h"
#include "covers.h"
#include "engine.h"
#include "gfx.h"
#include "player.h"
#include "strutil.h"
#include "sys.h"
#include "timeline.h"

/* layout */
#define CY 82.0f        /* transport row centre */
#define SCRUB_Y 142.0f
#define CHIP_Y 182.0f

static float g_press[5];   /* button press animations */
static bool g_sheet;       /* chapter list open */
static float g_sheet_t;    /* 0..1 */
static int g_sheet_focus;
static float g_sheet_scroll;
static bool g_scrubbing;
static double g_scrub_t;
static int g_chip_focus = -1; /* -1 transport, 0..3 chips */
static float g_play_morph;    /* 0 play .. 1 pause */
static float g_disp_frac;     /* smoothed progress */

static void enter(void) {
    g_sheet = false;
    g_sheet_t = 0;
    g_scrubbing = false;
    g_chip_focus = -1;
    if (player_book()) app_focus_book(player_book()->id);
    else app_focus_book(g_sel.id);
}

static const BookDetail *book(void) { return player_has_book() ? player_book() : NULL; }

static void chapter_span(double t, double *s, double *e, int *idx) {
    const BookDetail *b = book();
    *s = 0;
    *e = b ? b->duration : 0;
    *idx = -1;
    if (!b || !b->nchapters) return;
    int i = tl_chapter_at(b->chapters, b->nchapters, t);
    *idx = i;
    *s = b->chapters[i].start;
    *e = b->chapters[i].end > *s ? b->chapters[i].end : b->duration;
}

static void press(int i) { g_press[i] = 1; }

static void chip_action(int i) {
    switch (i) {
    case 0:
        if (player_speed_locked()) app_toast("Old 3DS plays at 1x only");
        else player_cycle_speed();
        break;
    case 1: player_cycle_sleep(); break;
    case 2: {
        const BookDetail *b = book();
        if (b && b->nchapters) {
            g_sheet = true;
            g_sheet_focus = player_chapter_index() < 0 ? 0 : player_chapter_index();
            g_sheet_scroll = g_sheet_focus * 30 - 60;
        }
        break;
    }
    case 3: sys_backlight(false); break;
    }
}

static void update_sheet(const Input *in, float dt) {
    const BookDetail *b = book();
    int n = b ? b->nchapters : 0;
    if (in->down & (KEY_B | KEY_X)) { g_sheet = false; return; }
    if (in->repeat & (KEY_DOWN | KEY_CPAD_DOWN)) g_sheet_focus = g_sheet_focus + 1 < n ? g_sheet_focus + 1 : g_sheet_focus;
    if (in->repeat & (KEY_UP | KEY_CPAD_UP)) g_sheet_focus = g_sheet_focus > 0 ? g_sheet_focus - 1 : 0;
    if (in->repeat & KEY_RIGHT) g_sheet_focus = g_sheet_focus + 6 < n ? g_sheet_focus + 6 : n - 1;
    if (in->repeat & KEY_LEFT) g_sheet_focus = g_sheet_focus > 6 ? g_sheet_focus - 6 : 0;
    if (in->down & KEY_A && n) { player_jump(b->chapters[g_sheet_focus].start); g_sheet = false; return; }
    if (in->touching && in->dragging) g_sheet_scroll -= in->dy;
    if (in->tap) {
        if (in->ty < 34) { g_sheet = false; return; }
        int i = (int)((in->ty - 34 + g_sheet_scroll) / 30);
        if (i >= 0 && i < n) { player_jump(b->chapters[i].start); g_sheet = false; return; }
    }
    float target = g_sheet_focus * 30 - 70;
    if (!in->touching && (in->repeat & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_CPAD_UP | KEY_CPAD_DOWN)))
        g_sheet_scroll = approach(g_sheet_scroll, target, dt, 30);
    float maxs = n * 30 - (240 - 34) + 8;
    if (maxs < 0) maxs = 0;
    if (g_sheet_scroll < 0) g_sheet_scroll = 0;
    if (g_sheet_scroll > maxs) g_sheet_scroll = maxs;
}

static void update(const Input *in, float dt) {
    for (int i = 0; i < 5; i++) g_press[i] = approach(g_press[i], 0, dt, 9);
    g_sheet_t = approach(g_sheet_t, g_sheet ? 1 : 0, dt, 14);
    g_play_morph = approach(g_play_morph, player_playing() ? 1 : 0, dt, 16);
    if (book()) app_focus_book(book()->id);
    PlayerPhase ph = player_phase();
    if (ph == PL_FAILED && (in->down & (KEY_B | KEY_A))) { app_pop(); return; }
    if (g_sheet) { update_sheet(in, dt); return; }

    if (in->down & KEY_B || in->down & KEY_START) { app_pop(); return; }
    if (in->tap && hit(in, 0, 0, 90, 28)) { app_pop(); return; }
    if (!book()) return;

    /* buttons */
    if (in->down & KEY_A) {
        if (g_chip_focus >= 0) chip_action(g_chip_focus);
        else { player_toggle(); press(2); }
    }
    if (in->repeat & KEY_LEFT && g_chip_focus < 0) { player_skip(-g_cfg.skip_back); press(1); }
    if (in->repeat & KEY_RIGHT && g_chip_focus < 0) { player_skip(g_cfg.skip_fwd); press(3); }
    if (g_chip_focus >= 0) {
        if (in->repeat & KEY_LEFT) g_chip_focus = g_chip_focus > 0 ? g_chip_focus - 1 : 0;
        if (in->repeat & KEY_RIGHT) g_chip_focus = g_chip_focus < 3 ? g_chip_focus + 1 : 3;
    }
    if (in->down & (KEY_DOWN | KEY_CPAD_DOWN)) g_chip_focus = g_chip_focus < 0 ? 0 : g_chip_focus;
    if (in->down & (KEY_UP | KEY_CPAD_UP)) g_chip_focus = -1;
    if (in->down & KEY_L) { player_chapter(-1); press(0); }
    if (in->down & KEY_R) { player_chapter(1); press(4); }
    if (in->down & KEY_X) chip_action(2);
    if (in->down & KEY_Y) chip_action(0);
    if (in->down & KEY_SELECT) chip_action(1);

    if (in->tap) {
        float x = in->tx, y = in->ty;
        if (fabsf(y - CY) < 34) {
            if (fabsf(x - 160) < 36) { player_toggle(); press(2); }
            else if (fabsf(x - 92) < 28) { player_skip(-g_cfg.skip_back); press(1); }
            else if (fabsf(x - 228) < 28) { player_skip(g_cfg.skip_fwd); press(3); }
            else if (x < 52) { player_chapter(-1); press(0); }
            else if (x > 268) { player_chapter(1); press(4); }
        }
        if (y > CHIP_Y - 4 && y < CHIP_Y + 34) {
            int i = (int)((x - 8) / 77);
            if (i >= 0 && i < 4) chip_action(i);
        }
    }

    /* chapter scrubber */
    double cs, ce;
    int ci;
    chapter_span(player_position(), &cs, &ce, &ci);
    if (in->touch_down && fabsf(in->ty - SCRUB_Y) < 16 && in->tx > 10 && in->tx < 310) g_scrubbing = true;
    if (g_scrubbing) {
        float f = clampf((in->tx - 20) / 280.0f, 0, 1);
        g_scrub_t = cs + f * (ce - cs);
        if (in->touch_up) {
            player_jump(g_scrub_t);
            g_scrubbing = false;
        }
    }
}

/* ---- drawing ------------------------------------------------------------------ */

static void draw_top(void) {
    const BookDetail *b = book();
    const char *id = b ? b->id : g_sel.id;
    app_draw_backdrop(id, 0.55f);
    app_draw_status_bar("Now Playing");
    u32 acc = app_accent();
    CoverState st;
    const C2D_Image *img = covers_hero(id, NULL, &st);
    float cs = 164, cx = 22, cy = 34;
    float breathe = player_playing() ? 0 : 6;
    static float shrink;
    shrink = approach(shrink, breathe, 1 / 60.0f, 8);
    float s = cs - shrink * 2;
    gfx_shadow(cx + shrink, cy + shrink + 8, s, s, 20, 0.6f);
    if (img) gfx_image(img, cx + shrink, cy + shrink, s, s, 0, 0);
    else gfx_placeholder_cover(b ? b->title : g_sel.title, cx + shrink, cy + shrink, s);

    float tx = 206, tw = 178;
    if (!b) {
        PlayerPhase ph = player_phase();
        gfx_text_fit(F_BOLD, g_sel.title, tx, 70, 20, tw, COL_TEXT, AL_LEFT);
        gfx_text_fit(F_SYS, g_sel.author, tx, 98, 14, tw, COL_DIM, AL_LEFT);
        gfx_text_fit(F_UI, ph == PL_FAILED ? player_error() : "Opening\xE2\x80\xA6", tx, 130, 15, tw, ph == PL_FAILED ? COL_DANGER : acc, AL_LEFT);
        if (ph == PL_FAILED) gfx_text(F_UI, "Press B to go back", tx, 152, 13, COL_FAINT, AL_LEFT);
        return;
    }
    double pos = g_scrubbing ? g_scrub_t : player_position(), dur = b->duration;
    int ci = tl_chapter_at(b->chapters, b->nchapters, pos);
    char lbl[48];
    if (ci >= 0) snprintf(lbl, sizeof lbl, "CHAPTER %d OF %d", ci + 1, b->nchapters);
    else snprintf(lbl, sizeof lbl, "NOW PLAYING");
    gfx_text(F_BOLD, lbl, tx, 44, 12, acc, AL_LEFT);
    char lines[2][160];
    const char *ctitle = ci >= 0 && b->chapters[ci].title[0] ? b->chapters[ci].title : b->title;
    int n = gfx_wrap(F_SYS, ctitle, 19, tw, lines, 2);
    float y = 62;
    for (int i = 0; i < n; i++, y += 23) gfx_text(F_SYS, lines[i], tx, y, 19, COL_TEXT, AL_LEFT);
    y += 6;
    gfx_text_fit(F_SYS, b->title, tx, y, 14, tw, COL_DIM, AL_LEFT);
    gfx_text_fit(F_SYS, b->author, tx, y + 18, 13, tw, COL_FAINT, AL_LEFT);

    /* status chips (speed/sleep) */
    float chy = 162;
    float chx = tx;
    char sp[16];
    snprintf(sp, sizeof sp, "%.2g\xC3\x97", player_speed());
    if (fabsf(player_speed() - 1) > 0.01f) {
        float w = gfx_text_width(F_UI, sp, 12) + 14;
        gfx_rrect(chx, chy, w, 18, 9, col_alpha(acc, 0.25f));
        gfx_text(F_UI, sp, chx + w / 2, chy + 2, 12, acc, AL_CENTER);
        chx += w + 6;
    }
    if (player_sleep()->mode != SLEEP_OFF) {
        char sl[24];
        player_sleep_label(sl, sizeof sl);
        float w = gfx_text_width(F_UI, sl, 12) + 26;
        gfx_rrect(chx, chy, w, 18, 9, col_alpha(COL_TEXT, 0.12f));
        gfx_icon(IC_MOON, chx + 10, chy + 9, 10, COL_TEXT);
        gfx_text(F_UI, sl, chx + 18, chy + 2, 12, COL_TEXT, AL_LEFT);
        chx += w + 6;
    }
    const char *status = player_status();
    if (status) gfx_text_fit(F_UI, status, tx, chy + 22, 12, tw, acc, AL_LEFT);
    if (sys_headphones()) gfx_icon(IC_HEADPHONES, 380, 44, 12, COL_FAINT);

    /* book progress */
    float frac = dur > 0 ? (float)(pos / dur) : 0;
    g_disp_frac = fabsf(g_disp_frac - frac) > 0.05f ? frac : approach(g_disp_frac, frac, 1 / 60.0f, 12);
    float px = 22, pw = 356, py = 212;
    gfx_progress(px, py, pw, 4, g_disp_frac, acc, col_alpha(COL_TEXT, 0.15f));
    gfx_circle(px + pw * g_disp_frac, py + 2, 5, COL_TEXT);
    char a[24], r[24], rr[32];
    fmt_time(pos, a, sizeof a);
    fmt_time((dur - pos) / (player_speed() > 0 ? player_speed() : 1), r, sizeof r);
    snprintf(rr, sizeof rr, "-%s", r);
    gfx_text(F_UI, a, px, py + 9, 12, COL_DIM, AL_LEFT);
    gfx_text(F_UI, rr, px + pw, py + 9, 12, COL_DIM, AL_RIGHT);
    char pct[16];
    snprintf(pct, sizeof pct, "%d%%", (int)(frac * 100));
    gfx_text(F_UI, pct, 200, py + 9, 12, COL_FAINT, AL_CENTER);
}

static void round_button(float x, float y, float r, IconId ic, float isz, float pressed, u32 bg, u32 fg, const char *num) {
    float s = 1 - pressed * 0.12f;
    gfx_circle(x, y, r * s, bg);
    gfx_icon(ic, x, y, isz * s, fg);
    if (num) gfx_text(F_BOLD, num, x, y - 6 * s, 11 * s, fg, AL_CENTER);
}

static void draw_sheet(void) {
    const BookDetail *b = book();
    if (!b || g_sheet_t <= 0.01f) return;
    float off = (1 - ease_out_cubic(g_sheet_t)) * 240;
    gfx_rect(0, 0, 320, 240, RGBA(0, 0, 0, (u8)(150 * g_sheet_t)));
    float top = off;
    gfx_rrect(0, top, 320, 260, 14, COL_SURFACE);
    gfx_rrect(146, top + 6, 28, 4, 2, COL_LINE);
    gfx_text(F_BOLD, "Chapters", 16, top + 12, 15, COL_TEXT, AL_LEFT);
    gfx_text(F_UI, "B close", 304, top + 14, 12, COL_FAINT, AL_RIGHT);
    int cur = player_chapter_index();
    for (int i = 0; i < b->nchapters; i++) {
        float y = top + 34 + i * 30 - g_sheet_scroll;
        if (y < top + 28) continue;
        if (y > 240) break;
        bool f = i == g_sheet_focus;
        if (f) gfx_rrect(6, y, 308, 28, 8, COL_SURFACE2);
        u32 c = i == cur ? app_accent() : f ? COL_TEXT : COL_DIM;
        char num[8];
        snprintf(num, sizeof num, "%d", i + 1);
        gfx_text(F_UI, num, 30, y + 6, 12, i == cur ? app_accent() : COL_FAINT, AL_RIGHT);
        gfx_text_fit(F_SYS, b->chapters[i].title[0] ? b->chapters[i].title : num, 40, y + 6, 14, 200, c, AL_LEFT);
        char t[16];
        fmt_time(b->chapters[i].start, t, sizeof t);
        gfx_text(F_UI, t, 306, y + 7, 12, COL_FAINT, AL_RIGHT);
        if (i == cur) {
            /* little equaliser bars on the current chapter */
            for (int k = 0; k < 3; k++) {
                float h = player_playing() ? 4 + 6 * (0.5f + 0.5f * sinf(app_time() * 7 + k * 1.7f)) : 4;
                gfx_rect(250 + k * 4, y + 20 - h, 2.5f, h, c);
            }
        }
    }
}

static void draw_bottom(void) {
    gfx_rect(0, 0, 320, 240, COL_BG);
    u32 acc = app_accent();
    gfx_gradient_v(0, 0, 320, 240, col_mix(COL_BG, acc, 0.06f), COL_BG);
    gfx_icon(IC_CHEVRON_LEFT, 14, 14, 14, COL_DIM);
    gfx_text(F_UI, "Library", 24, 6, 14, COL_DIM, AL_LEFT);
    if (sys_lid_closed()) gfx_text(F_UI, "Lid closed", 306, 6, 12, COL_FAINT, AL_RIGHT);
    const BookDetail *b = book();
    if (!b) {
        PlayerPhase ph = player_phase();
        if (ph == PL_FAILED) {
            gfx_icon(IC_CLOSE, 160, 90, 32, COL_DANGER);
            gfx_text_fit(F_UI, player_error(), 160, 120, 15, 290, COL_TEXT, AL_CENTER);
        } else {
            float t = app_time() * 5;
            for (int i = 0; i < 3; i++) gfx_circle(148 + i * 12, 100, 4, col_alpha(acc, 0.4f + 0.6f * (0.5f + 0.5f * sinf(t - i))));
        }
        return;
    }
    char back[8], fwd[8];
    snprintf(back, sizeof back, "%d", g_cfg.skip_back);
    snprintf(fwd, sizeof fwd, "%d", g_cfg.skip_fwd);
    bool tf = g_chip_focus < 0;
    round_button(28, CY, 20, IC_PREV, 16, g_press[0], COL_SURFACE, COL_DIM, NULL);
    round_button(92, CY, 26, IC_BACK, 34, g_press[1], COL_SURFACE2, COL_TEXT, NULL);
    gfx_text(F_BOLD, back, 92, CY - 3, 10, COL_TEXT, AL_CENTER);
    round_button(228, CY, 26, IC_FWD, 34, g_press[3], COL_SURFACE2, COL_TEXT, NULL);
    gfx_text(F_BOLD, fwd, 228, CY - 3, 10, COL_TEXT, AL_CENTER);
    round_button(292, CY, 20, IC_NEXT, 16, g_press[4], COL_SURFACE, COL_DIM, NULL);
    /* play/pause with a glow while playing */
    float s = 1 - g_press[2] * 0.1f;
    if (g_play_morph > 0.01f) for (int i = 3; i >= 1; i--) gfx_circle(160, CY, (38 + i * 5) * s, col_alpha(acc, 0.07f * g_play_morph));
    if (tf) gfx_circle(160, CY, 41 * s, col_alpha(COL_TEXT, 0.55f));
    gfx_circle(160, CY, 38 * s, acc);
    EngineState es = engine_state();
    if (es == ENG_LOADING || (es == ENG_BUFFERING && player_playing())) {
        float t = app_time() * 6;
        for (int i = 0; i < 8; i++) {
            float a = t + i * 0.785f;
            gfx_circle(160 + cosf(i * 0.785f) * 18, CY + sinf(i * 0.785f) * 18, 3, col_alpha(COL_BG, 0.25f + 0.75f * (0.5f + 0.5f * sinf(a))));
        }
    } else {
        IconId ic = g_play_morph > 0.5f ? IC_PAUSE : IC_PLAY;
        gfx_icon(ic, ic == IC_PLAY ? 163 : 160, CY, 30 * s, COL_BG);
    }

    /* chapter scrubber */
    double pos = g_scrubbing ? g_scrub_t : player_position(), cs, ce;
    int ci;
    chapter_span(pos, &cs, &ce, &ci);
    float f = ce > cs ? (float)((pos - cs) / (ce - cs)) : 0;
    float sx = 20, sw = 280;
    float th = g_scrubbing ? 6 : 4;
    gfx_progress(sx, SCRUB_Y - th / 2, sw, th, f, acc, col_alpha(COL_TEXT, 0.12f));
    gfx_circle(sx + sw * clampf(f, 0, 1), SCRUB_Y, g_scrubbing ? 9 : 6, COL_TEXT);
    char a[16], r[16], rr[20];
    fmt_time(pos - cs, a, sizeof a);
    fmt_time(ce - pos, r, sizeof r);
    snprintf(rr, sizeof rr, "-%s", r);
    gfx_text(F_UI, a, sx, SCRUB_Y + 9, 11, COL_FAINT, AL_LEFT);
    gfx_text(F_UI, rr, sx + sw, SCRUB_Y + 9, 11, COL_FAINT, AL_RIGHT);
    if (ci >= 0) gfx_text_fit(F_SYS, b->chapters[ci].title, 160, SCRUB_Y + 8, 11, 170, COL_DIM, AL_CENTER);

    /* chips */
    char sp[16], sl[24];
    snprintf(sp, sizeof sp, "%.2g\xC3\x97", player_speed());
    player_sleep_label(sl, sizeof sl);
    const char *labels[4] = {sp, sl, "Chapters", "Screen"};
    IconId icons[4] = {IC_SPEED, IC_MOON, IC_LIST, IC_SCREEN};
    for (int i = 0; i < 4; i++) {
        float x = 8 + i * 77, w = 73;
        bool active = (i == 0 && fabsf(player_speed() - 1) > 0.01f) || (i == 1 && player_sleep()->mode != SLEEP_OFF);
        bool focus = g_chip_focus == i;
        if (focus) gfx_focus_ring(x, CHIP_Y, w, 32, 10, 2, col_alpha(COL_TEXT, 0.7f));
        gfx_rrect(x, CHIP_Y, w, 32, 10, active ? col_mix(COL_SURFACE, acc, 0.3f) : COL_SURFACE);
        gfx_icon(icons[i], x + 12, CHIP_Y + 16, 13, active ? acc : COL_DIM);
        gfx_text_fit(F_UI, labels[i], x + 22, CHIP_Y + 9, 11, w - 25, active ? COL_TEXT : COL_DIM, AL_LEFT);
    }
    gfx_text(F_UI, "L/R chapter \xC2\xB7 left/right skip \xC2\xB7 X chapters", 160, 222, 10.5f, col_alpha(COL_FAINT, 0.8f), AL_CENTER);
    draw_sheet();
}

const Screen scr_player = {enter, update, draw_top, draw_bottom};
