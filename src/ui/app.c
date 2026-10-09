#include "app.h"

#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "abs_client.h"
#include "anim.h"
#include "covers.h"
#include "downloads.h"
#include "engine.h"
#include "gfx.h"
#include "http.h"
#include "player.h"
#include "strutil.h"
#include "sys.h"
#include "timeline.h"
#include "worker.h"

#define DATA_DIR "sdmc:/3ds/abs3ds"
#define CFG_PATH DATA_DIR "/config.json"
#define PQ_PATH DATA_DIR "/progress_queue.json"

Config g_cfg;
ProgressQueue g_pq;
Library g_lib;
BookSummary g_sel;

static const Screen *g_screens[SCR_COUNT] = {&scr_login, &scr_home, &scr_book, &scr_player};
static ScreenId g_stack[8];
static int g_sp;
static float g_trans = 1;      /* 0 -> 1 transition progress */
static int g_trans_dir = 1;    /* +1 push, -1 pop */
static char g_toast[96];
static float g_toast_t;
static float g_time;
static u32 g_accent = COL_AMBER, g_accent_target = COL_AMBER;
static char g_focus_id[40];

float app_time(void) { return g_time; }

/* ---- persistence -------------------------------------------------------- */

void app_save_cfg(void) {
    bool known;
    long long off = http_clock_offset(&known);
    if (known) { g_cfg.clock_offset_ms = off; g_cfg.clock_known = true; }
    cfg_save(&g_cfg, CFG_PATH);
}

static LightLock g_pq_lock;
void app_save_queue(void) {
    LightLock_Lock(&g_pq_lock);
    pq_save(&g_pq, PQ_PATH);
    LightLock_Unlock(&g_pq_lock);
}

/* ---- navigation ------------------------------------------------------------ */

static void enter_top(void) {
    g_trans = 0;
    const Screen *s = g_screens[g_stack[g_sp - 1]];
    if (s->enter) s->enter();
}

void app_push(ScreenId s) {
    if (g_sp < 8) g_stack[g_sp++] = s;
    g_trans_dir = 1;
    enter_top();
}

void app_pop(void) {
    if (g_sp > 1) g_sp--;
    g_trans_dir = -1;
    g_trans = 0;
}

void app_replace(ScreenId s) {
    g_sp = 0;
    app_push(s);
}

void app_toast(const char *msg) {
    str_copy(g_toast, sizeof g_toast, msg);
    g_toast_t = 2.6f;
}

/* ---- library model ------------------------------------------------------------- */

typedef struct {
    BookSummary *books;
    int n;
    char cont[24][40];
    int ncont;
    int rc;
} LibResult;

static LibResult *g_lib_result;
static LightLock g_lib_lock;

static void lib_progress(int done, int total) {
    g_lib.load_done = done;
    g_lib.load_total = total;
}

/* offline: the library is whatever is downloaded */
static void offline_books(LibResult *r) {
    DlInfo *dl = malloc(sizeof(DlInfo) * 128);
    if (!dl) return;
    int n = dl_list(dl, 128);
    r->books = calloc(n ? n : 1, sizeof(BookSummary));
    for (int i = 0; i < n && r->books; i++) {
        if (dl[i].state != DL_DONE) continue;
        BookSummary *b = &r->books[r->n++];
        str_copy(b->id, sizeof b->id, dl[i].id);
        str_copy(b->title, sizeof b->title, dl[i].title);
        str_copy(b->author, sizeof b->author, dl[i].author);
        BookDetail d;
        if (dl_load_meta(dl[i].id, &d) == 0) { b->duration = d.duration; book_detail_free(&d); }
    }
    free(dl);
}

static void lib_job(void *ud) {
    (void)ud;
    LibResult *r = calloc(1, sizeof *r);
    if (!r) return;
    r->rc = absc_fetch_library(&r->books, &r->n, lib_progress);
    if (r->rc == ABS_OK) {
        int k = absc_fetch_in_progress(r->cont, 24);
        r->ncont = k > 0 ? k : 0;
    } else if (r->rc == ABS_ERR_NET) {
        offline_books(r);
    }
    LightLock_Lock(&g_lib_lock);
    if (g_lib_result) { free(g_lib_result->books); free(g_lib_result); }
    g_lib_result = r;
    LightLock_Unlock(&g_lib_lock);
}

void lib_reload(void) {
    if (g_lib.loading) return;
    g_lib.loading = true;
    g_lib.error[0] = 0;
    g_lib.load_done = g_lib.load_total = 0;
    worker_post(g_api, lib_job, NULL);
}

void lib_apply(void) {
    free(g_lib.order);
    g_lib.order = malloc(sizeof(int) * (g_lib.n ? g_lib.n : 1));
    int *all = malloc(sizeof(int) * (g_lib.n ? g_lib.n : 1));
    if (!g_lib.order || !all) { free(all); g_lib.norder = 0; return; }
    for (int i = 0; i < g_lib.n; i++) all[i] = i;
    lib_sort(g_lib.books, all, g_lib.n, g_lib.sort);
    g_lib.norder = lib_filter(g_lib.books, all, g_lib.n, g_lib.query, g_lib.order);
    free(all);
}

BookSummary *lib_get(const char *id) {
    int i = lib_find(g_lib.books, g_lib.n, id);
    return i >= 0 ? &g_lib.books[i] : NULL;
}

static void take_lib_result(void) {
    LightLock_Lock(&g_lib_lock);
    LibResult *r = g_lib_result;
    g_lib_result = NULL;
    LightLock_Unlock(&g_lib_lock);
    if (!r) return;
    g_lib.loading = false;
    if (r->rc == ABS_ERR_AUTH) {
        free(r->books);
        free(r);
        app_toast("Session expired, please log in");
        app_replace(SCR_LOGIN);
        return;
    }
    if (r->rc != ABS_OK && !(r->rc == ABS_ERR_NET && r->n > 0)) {
        str_copy(g_lib.error, sizeof g_lib.error, r->rc == ABS_ERR_NET ? "Can't reach the server" : "Server error");
    }
    if (r->books || r->rc == ABS_OK) {
        free(g_lib.books);
        g_lib.books = r->books;
        g_lib.n = r->n;
        memcpy(g_lib.cont, r->cont, sizeof r->cont);
        g_lib.ncont = r->ncont;
        g_lib.offline = r->rc != ABS_OK;
        g_lib.loaded = true;
        /* overlay locally-queued progress that is newer than the server's */
        for (int i = 0; i < g_pq.n; i++) {
            BookSummary *b = lib_get(g_pq.e[i].item_id);
            if (b && g_pq.e[i].updated_ms > b->progress_updated) {
                b->current_time = g_pq.e[i].current_time;
                b->progress = b->duration > 0 ? (float)(b->current_time / b->duration) : 0;
                b->progress_updated = g_pq.e[i].updated_ms;
            }
        }
        if (g_lib.offline) {
            /* continue = locally-known progress, most recent first */
            g_lib.ncont = 0;
            for (int k = 0; k < g_pq.n && g_lib.ncont < 24; k++)
                if (lib_get(g_pq.e[k].item_id)) str_copy(g_lib.cont[g_lib.ncont++], 40, g_pq.e[k].item_id);
        }
        lib_apply();
        if (r->rc == ABS_OK) player_flush_queue();
    } else {
        free(r->books);
    }
    free(r);
}

void app_open_book(const BookSummary *b) {
    g_sel = *b;
    app_push(SCR_BOOK);
}

void app_play_book(const BookSummary *b) {
    player_open(b);
    g_sel = *b;
    app_push(SCR_PLAYER);
}

/* ---- accent / backdrop / status -------------------------------------------------- */

void app_focus_book(const char *id) {
    if (!id) return;
    str_copy(g_focus_id, sizeof g_focus_id, id);
}

u32 app_accent(void) { return g_accent; }

static void update_accent(float dt) {
    u32 c;
    const char *id = g_focus_id[0] ? g_focus_id : player_book_id();
    if (id[0] && covers_accent(id, &c)) g_accent_target = c;
    /* blend per channel, frame-rate independent */
    float k = 1 - expf(-6 * dt);
    g_accent = col_mix(g_accent, g_accent_target, k);
}

void app_draw_backdrop(const char *id, float dim) {
    const C2D_Image *blur = NULL;
    CoverState st;
    covers_hero(id, &blur, &st);
    gfx_rect(0, 0, 400, 240, COL_BG);
    if (blur) {
        /* a 32x32 cover stretched with linear filtering is a cheap, smooth blur */
        gfx_image(blur, -20, -100, 440, 440, RGBA(0, 0, 0, 255), 0);
        gfx_rect(0, 0, 400, 240, RGBA(14, 17, 22, (u8)(255 * dim)));
    } else {
        gfx_gradient_v(0, 0, 400, 240, col_mix(COL_BG, g_accent, 0.18f), COL_BG);
    }
    gfx_gradient_v(0, 150, 400, 90, RGBA(14, 17, 22, 0), RGBA(14, 17, 22, 220));
}

static void draw_battery(float x, float y) {
    int lvl = sys_battery();
    u32 c = lvl <= 1 && !sys_charging() ? COL_DANGER : col_alpha(COL_TEXT, 0.85f);
    gfx_rrect_outline(x, y, 22, 11, 3, 1.5f, c, RGBA(0, 0, 0, 0));
    gfx_rect(x + 22, y + 3.5f, 2, 4, c);
    float w = 18.0f * (lvl > 5 ? 5 : lvl) / 5.0f;
    if (w > 0) gfx_rrect(x + 2, y + 2, w, 7, 1.5f, sys_charging() ? COL_OK : c);
}

void app_draw_status_bar(const char *title) {
    gfx_gradient_v(0, 0, 400, 26, RGBA(0, 0, 0, 110), RGBA(0, 0, 0, 0));
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    char clock[16];
    snprintf(clock, sizeof clock, "%02d:%02d", tm->tm_hour, tm->tm_min);
    gfx_text(F_BOLD, clock, 12, 5, 15, col_alpha(COL_TEXT, 0.92f), AL_LEFT);
    if (title) gfx_text_fit(F_UI, title, 200, 6, 13, 200, col_alpha(COL_TEXT, 0.6f), AL_CENTER);
    draw_battery(366, 7);
    int bars = sys_wifi();
    if (sys_wifi_connected()) {
        for (int i = 0; i < 3; i++) {
            float h = 4 + i * 3.5f;
            gfx_rrect(338 + i * 6, 18 - h, 4, h, 1.5f, i < bars ? col_alpha(COL_TEXT, 0.85f) : col_alpha(COL_TEXT, 0.25f));
        }
    } else {
        gfx_icon(IC_CLOUD_OFF, 346, 12, 16, col_alpha(COL_TEXT, 0.5f));
    }
    if (g_lib.offline) gfx_text(F_UI, "Offline", 330, 6, 12, COL_DIM, AL_RIGHT);
}

void app_draw_top_book(const BookSummary *b) {
    if (!b) return;
    const C2D_Image *img;
    CoverState st;
    img = covers_hero(b->id, NULL, &st);
    float cs = 168, cx = 24, cy = 44;
    gfx_shadow(cx, cy + 6, cs, cs, 18, 0.55f);
    if (img) gfx_image(img, cx, cy, cs, cs, 0, 0);
    else gfx_placeholder_cover(b->title, cx, cy, cs);
    float tx = 212, tw = 172;
    char lines[3][160];
    int n = gfx_wrap(F_BOLD, b->title, 21, tw, lines, 3);
    float y = 56;
    if (n == 1) y = 74;
    for (int i = 0; i < n; i++, y += 24) gfx_text(F_BOLD, lines[i], tx, y, 21, COL_TEXT, AL_LEFT);
    y += 4;
    if (b->author[0]) { gfx_text_fit(F_SYS, b->author, tx, y, 15, tw, COL_DIM, AL_LEFT); y += 20; }
    if (b->series[0]) { gfx_text_fit(F_SYS, b->series, tx, y, 13, tw, COL_FAINT, AL_LEFT); y += 18; }
    y += 6;
    char dur[32];
    fmt_duration(b->duration, dur, sizeof dur);
    float w = gfx_text_width(F_UI, dur, 13) + 16;
    gfx_rrect(tx, y, w, 20, 10, col_alpha(COL_TEXT, 0.12f));
    gfx_text(F_UI, dur, tx + w / 2, y + 3, 13, COL_TEXT, AL_CENTER);
    float bx = tx + w + 6;
    if (dl_is_done(b->id)) {
        gfx_rrect(bx, y, 24, 20, 10, col_alpha(g_accent, 0.25f));
        gfx_icon(IC_DOWNLOAD, bx + 12, y + 10, 13, g_accent);
    }
    if (b->progress > 0.001f && !b->finished) {
        float py = 196;
        gfx_progress(tx, py, tw, 5, b->progress, g_accent, col_alpha(COL_TEXT, 0.15f));
        char left[48], rem[24];
        fmt_duration(b->duration * (1 - b->progress), rem, sizeof rem);
        snprintf(left, sizeof left, "%d%%  \xC2\xB7  %s left", (int)(b->progress * 100 + 0.5f), rem);
        gfx_text(F_UI, left, tx, py + 10, 13, COL_DIM, AL_LEFT);
    } else if (b->finished) {
        gfx_icon(IC_CHECK, tx + 8, 204, 14, COL_OK);
        gfx_text(F_UI, "Finished", tx + 20, 196, 13, COL_OK, AL_LEFT);
    }
}

/* ---- mini player ------------------------------------------------------------------ */

static bool mini_visible(void) {
    ScreenId top = g_stack[g_sp - 1];
    return player_has_book() && top != SCR_PLAYER && top != SCR_LOGIN;
}

float app_mini_player_height(void) { return mini_visible() ? 46 : 0; }

void app_draw_mini_player(void) {
    if (!mini_visible()) return;
    const BookDetail *b = player_book();
    float y = 194;
    gfx_shadow(0, y + 4, 320, 46, 10, 0.5f);
    gfx_rect(0, y, 320, 46, COL_SURFACE2);
    double dur = player_duration(), pos = player_position();
    gfx_rect(0, y, 320, 2, col_alpha(COL_TEXT, 0.08f));
    if (dur > 0) gfx_rect(0, y, (float)(320 * pos / dur), 2, g_accent);
    CoverState st;
    const C2D_Image *img = covers_thumb(b->id, &st);
    if (img) gfx_image(img, 7, y + 7, 32, 32, 0, 0);
    else gfx_placeholder_cover(b->title, 7, y + 7, 32);
    gfx_text_fit(F_SYS, b->title, 48, y + 7, 14, 200, COL_TEXT, AL_LEFT);
    int ci = player_chapter_index();
    const char *sub = ci >= 0 ? b->chapters[ci].title : b->author;
    const char *status = player_status();
    gfx_text_fit(F_SYS, status ? status : sub, 48, y + 25, 12, 200, status ? g_accent : COL_DIM, AL_LEFT);
    gfx_circle(292, y + 23, 16, g_accent);
    gfx_icon(player_playing() ? IC_PAUSE : IC_PLAY, player_playing() ? 292 : 293.5f, y + 23, 14, COL_BG);
}

bool app_mini_player_input(const Input *in) {
    if (!mini_visible()) return false;
    if (in->tap && hit(in, 268, 194, 52, 46)) { player_toggle(); return true; }
    if (in->tap && hit(in, 0, 194, 268, 46)) { app_push(SCR_PLAYER); return true; }
    return false;
}

/* ---- input ---------------------------------------------------------------------------- */

bool hit(const Input *in, float x, float y, float w, float h) {
    return in->tx >= x && in->tx < x + w && in->ty >= y && in->ty < y + h;
}

static Input g_in;
static float g_vy_hist[4];
static u64 g_touch_start_ms;

static void read_input(float dt) {
    Input *in = &g_in;
    u32 down = hidKeysDown(), held = hidKeysHeld(), rep = hidKeysDownRepeat();
    bool touching = (held & KEY_TOUCH) != 0;
    touchPosition tp;
    hidTouchRead(&tp);
    in->down = down & ~KEY_TOUCH;
    in->held = held & ~KEY_TOUCH;
    in->repeat = rep & ~KEY_TOUCH;
    in->touch_down = touching && !in->touching;
    in->touch_up = !touching && in->touching;
    in->tap = false;
    in->dx = in->dy = 0;
    if (in->touch_down) {
        in->sx = in->tx = tp.px;
        in->sy = in->ty = tp.py;
        in->dragging = false;
        memset(g_vy_hist, 0, sizeof g_vy_hist);
        g_touch_start_ms = osGetTime();
    } else if (touching) {
        in->dx = tp.px - in->tx;
        in->dy = tp.py - in->ty;
        in->tx = tp.px;
        in->ty = tp.py;
        if (fabsf(in->tx - in->sx) > 7 || fabsf(in->ty - in->sy) > 7) in->dragging = true;
        memmove(g_vy_hist, g_vy_hist + 1, sizeof(float) * 3);
        g_vy_hist[3] = dt > 0 ? in->dy / dt : 0;
    }
    if (in->touch_up) {
        in->tap = !in->dragging && osGetTime() - g_touch_start_ms < 600;
        in->fling_vy = (g_vy_hist[0] + g_vy_hist[1] + g_vy_hist[2] + g_vy_hist[3]) / 4;
    }
    in->touching = touching;
}

/* ---- keyboard ------------------------------------------------------------------------ */

bool app_keyboard(const char *hint, const char *initial, bool password, char *out, int n) {
    SwkbdState kb;
    swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, n - 1);
    swkbdSetHintText(&kb, hint);
    if (initial) swkbdSetInitialText(&kb, initial);
    if (password) swkbdSetPasswordMode(&kb, SWKBD_PASSWORD_HIDE_DELAY);
    swkbdSetValidation(&kb, SWKBD_ANYTHING, 0, 0);
    char buf[256] = "";
    SwkbdButton b = swkbdInputText(&kb, buf, sizeof buf);
    if (b != SWKBD_BUTTON_CONFIRM) return false;
    str_copy(out, n, buf);
    return true;
}

/* ---- main loop -------------------------------------------------------------------------- */

static void make_device_id(void) {
    if (g_cfg.device_id[0]) return;
    u64 seed = svcGetSystemTick() ^ osGetTime();
    snprintf(g_cfg.device_id, sizeof g_cfg.device_id, "abs3ds-%08lx%08lx", (unsigned long)(seed >> 32) ^ 0x5bd1e995,
             (unsigned long)seed);
    app_save_cfg();
}

static void draw_toast(void) {
    if (g_toast_t <= 0) return;
    float a = clampf(g_toast_t / 0.3f, 0, 1);
    float w = gfx_text_width(F_UI, g_toast, 14) + 28;
    if (w > 300) w = 300;
    float x = 160 - w / 2, y = 196 - app_mini_player_height() + (1 - a) * 10;
    gfx_shadow(x, y + 3, w, 30, 10, 0.5f * a);
    gfx_rrect(x, y, w, 30, 15, col_alpha(COL_SURFACE2, a));
    gfx_text_fit(F_UI, g_toast, 160, y + 7, 14, w - 20, col_alpha(COL_TEXT, a), AL_CENTER);
}

static void on_suspend(void) {
    player_on_suspend();
    app_save_cfg();
}

int app_run(void) {
    romfsInit();
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS * 4);
    C2D_Prepare();
    C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget *bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    hidSetRepeatParameters(22, 6);
    APT_SetAppCpuTimeLimit(30);

    mkdir("sdmc:/3ds", 0777);
    mkdir(DATA_DIR, 0777);
    cfg_load(&g_cfg, CFG_PATH);
    make_device_id();
    LightLock_Init(&g_pq_lock);
    LightLock_Init(&g_lib_lock);
    pq_load(&g_pq, PQ_PATH);

    gfx_init();
    sys_init(on_suspend);
    http_init();
    if (g_cfg.clock_known) http_set_clock_offset(g_cfg.clock_offset_ms);
    absc_init(&g_cfg, app_save_cfg);
    g_api = worker_create(0x30, 1, 64 * 1024);
    engine_init();
    dl_init();
    covers_init();
    player_init(&g_cfg, &g_pq, app_save_queue);

    if (absc_have_session()) {
        app_replace(SCR_HOME);
        lib_reload();
    } else {
        app_replace(SCR_LOGIN);
    }

    u64 last = osGetTime();
    while (aptMainLoop()) {
        u64 now = osGetTime();
        float dt = (now - last) / 1000.0f;
        if (dt > 0.1f) dt = 0.1f;
        last = now;
        g_time += dt;

        hidScanInput();
        read_input(dt);
        sys_poll();
        covers_update();
        take_lib_result();
        player_tick(dt);
        update_accent(dt);

        /* any input wakes a manually darkened screen */
        if (!sys_backlight_on() && !sys_lid_closed() && (g_in.down || g_in.touch_down)) {
            sys_backlight(true);
            memset(&g_in, 0, sizeof g_in);
        }
        if ((g_in.held & (KEY_START | KEY_SELECT)) == (KEY_START | KEY_SELECT)) break;

        if (g_trans < 1) g_trans = clampf(g_trans + dt / 0.24f, 0, 1);
        if (g_toast_t > 0) g_toast_t -= dt;
        const Screen *s = g_screens[g_stack[g_sp - 1]];
        if (g_trans > 0.35f) s->update(&g_in, dt);
        s = g_screens[g_stack[g_sp - 1]]; /* update may navigate */

        if (!sys_backlight_on()) {
            /* nothing to show: skip rendering to save power */
            svcSleepThread(30000000LL);
            continue;
        }
        float e = ease_out_cubic(g_trans);
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        gfx_frame_begin();
        C2D_TargetClear(top, COL_BG);
        C2D_SceneBegin(top);
        s->draw_top();
        if (e < 1) gfx_rect(0, 0, 400, 240, RGBA(14, 17, 22, (u8)(255 * (1 - e))));

        C2D_TargetClear(bot, COL_BG);
        C2D_SceneBegin(bot);
        C2D_ViewTranslate(g_trans_dir * (1 - e) * 26, 0);
        s->draw_bottom();
        C2D_ViewReset();
        if (e < 1) gfx_rect(0, 0, 320, 240, RGBA(14, 17, 22, (u8)(255 * (1 - e))));
        app_draw_mini_player();
        draw_toast();
        C3D_FrameEnd(0);
    }

    player_exit();
    covers_exit();
    dl_exit();
    engine_exit();
    worker_destroy(g_api);
    http_exit();
    app_save_cfg();
    app_save_queue();
    sys_exit();
    gfx_exit();
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    romfsExit();
    return 0;
}
