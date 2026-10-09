/* Setup / login: server URL, username, password (swkbd), connect. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "abs_client.h"
#include "anim.h"
#include "app.h"
#include "gfx.h"
#include "strutil.h"
#include "url.h"
#include "worker.h"

static int g_focus;            /* 0 server, 1 user, 2 password, 3 connect */
static char g_pass[128];
static volatile int g_busy;    /* connect job running */
static volatile int g_result;  /* 1 ok, <0 error code, 0 none */
static char g_msg[96];
static float g_ring_y;

typedef struct { char server[160], user[64], pass[128]; } LoginJob;

static void login_job(void *ud) {
    LoginJob *j = ud;
    int rc = absc_status(j->server);
    if (rc == ABS_OK) rc = absc_login(j->user, j->pass);
    memset(j->pass, 0, sizeof j->pass);
    free(j);
    g_result = rc == ABS_OK ? 1 : rc;
    g_busy = 0;
}

static void enter(void) {
    g_focus = g_cfg.username[0] ? 2 : 1;
    g_msg[0] = 0;
    g_result = 0;
    g_ring_y = 0;
}

static void do_connect(void) {
    if (g_busy) return;
    url_normalize(g_cfg.server, sizeof g_cfg.server);
    if (!url_valid(g_cfg.server)) { str_copy(g_msg, sizeof g_msg, "Server must look like http://host:port/path"); return; }
    if (!g_cfg.username[0] || !g_pass[0]) { str_copy(g_msg, sizeof g_msg, "Enter your username and password"); return; }
    LoginJob *j = calloc(1, sizeof *j);
    if (!j) return;
    str_copy(j->server, sizeof j->server, g_cfg.server);
    str_copy(j->user, sizeof j->user, g_cfg.username);
    str_copy(j->pass, sizeof j->pass, g_pass);
    g_cfg.library_id[0] = 0; /* re-discover for a new server/user */
    g_busy = 1;
    g_msg[0] = 0;
    worker_post(g_api, login_job, j);
}

static void edit(int field) {
    if (field == 0) {
        char buf[160];
        if (app_keyboard("Server address", g_cfg.server, false, buf, sizeof buf)) {
            str_copy(g_cfg.server, sizeof g_cfg.server, buf);
            url_normalize(g_cfg.server, sizeof g_cfg.server);
            app_save_cfg();
        }
    } else if (field == 1) {
        char buf[64];
        if (app_keyboard("Username", g_cfg.username, false, buf, sizeof buf)) str_copy(g_cfg.username, sizeof g_cfg.username, buf);
    } else if (field == 2) {
        char buf[128];
        if (app_keyboard("Password", NULL, true, buf, sizeof buf)) str_copy(g_pass, sizeof g_pass, buf);
    } else {
        do_connect();
    }
}

#define ROW_Y(i) (44.0f + (i) * 46.0f)

static void update(const Input *in, float dt) {
    if (g_result == 1) {
        g_result = 0;
        memset(g_pass, 0, sizeof g_pass);
        app_save_cfg();
        app_replace(SCR_HOME);
        lib_reload();
        return;
    }
    if (g_result < 0) {
        str_copy(g_msg, sizeof g_msg, g_result == ABS_ERR_AUTH ? "Wrong username or password" :
                 g_result == ABS_ERR_NET ? "Can't reach the server. Check the address and Wi-Fi." :
                 "That doesn't look like an Audiobookshelf server");
        g_result = 0;
    }
    if (in->repeat & (KEY_DOWN | KEY_CPAD_DOWN)) g_focus = g_focus < 3 ? g_focus + 1 : 3;
    if (in->repeat & (KEY_UP | KEY_CPAD_UP)) g_focus = g_focus > 0 ? g_focus - 1 : 0;
    if (in->down & KEY_A) edit(g_focus);
    if (in->down & KEY_START) do_connect();
    if (in->tap) {
        for (int i = 0; i < 3; i++)
            if (hit(in, 12, ROW_Y(i), 296, 40)) { g_focus = i; edit(i); }
        if (hit(in, 12, 186, 296, 40)) { g_focus = 3; do_connect(); }
    }
    float target = g_focus < 3 ? ROW_Y(g_focus) : 186;
    g_ring_y = g_ring_y == 0 ? target : approach(g_ring_y, target, dt, 18);
}

static void draw_top(void) {
    u32 acc = COL_AMBER;
    gfx_rect(0, 0, 400, 240, COL_BG);
    float t = app_time();
    float pulse = 0.5f + 0.5f * sinf(t * 1.3f);
    /* warm glow behind the mark */
    for (int i = 6; i >= 1; i--) gfx_circle(200, 96, 40 + i * 14, col_alpha(acc, 0.025f + 0.012f * pulse));
    gfx_circle(200, 96, 46, acc);
    gfx_icon(IC_BOOK, 200, 104, 46, RGBA(250, 242, 228, 255));
    gfx_text(F_BOLD, "ABS3DS", 200, 156, 34, COL_TEXT, AL_CENTER);
    gfx_text(F_UI, "Audiobookshelf for Nintendo 3DS", 200, 194, 15, COL_DIM, AL_CENTER);
}

static void field(int i, IconId ic, const char *label, const char *value, bool secret) {
    float y = ROW_Y(i);
    bool f = g_focus == i;
    gfx_rrect(12, y, 296, 40, 10, f ? COL_SURFACE2 : COL_SURFACE);
    gfx_icon(ic, 32, y + 20, 18, f ? COL_AMBER : COL_DIM);
    gfx_text(F_UI, label, 52, y + 4, 12, COL_FAINT, AL_LEFT);
    char shown[160];
    if (secret) {
        int n = (int)strlen(value);
        if (n > 24) n = 24;
        memset(shown, '*', n);
        shown[n] = 0;
        value = shown;
    }
    if (!value[0]) gfx_text(F_UI, "Tap to enter", 52, y + 19, 14, col_alpha(COL_DIM, 0.6f), AL_LEFT);
    else gfx_text_fit(F_SYS, value, 52, y + 19, 15, 240, COL_TEXT, AL_LEFT);
}

static void draw_bottom(void) {
    gfx_rect(0, 0, 320, 240, COL_BG);
    gfx_text(F_BOLD, "Sign in", 16, 10, 20, COL_TEXT, AL_LEFT);
    gfx_text(F_UI, "to your Audiobookshelf server", 104, 14, 13, COL_DIM, AL_LEFT);
    field(0, IC_SERVER, "SERVER", g_cfg.server, false);
    field(1, IC_USER, "USERNAME", g_cfg.username, false);
    field(2, IC_LOCK, "PASSWORD", g_pass, true);
    /* focus ring slides between rows */
    float ry = g_ring_y, rh = 40;
    gfx_rrect_outline(10, ry - 2, 300, rh + 4, 12, 2, col_alpha(COL_AMBER, 0.9f), RGBA(0, 0, 0, 0));
    /* connect */
    bool busy = g_busy;
    gfx_rrect(12, 186, 296, 40, 20, busy ? col_mix(COL_AMBER, COL_BG, 0.4f) : COL_AMBER);
    if (busy) {
        float t = app_time() * 6;
        for (int i = 0; i < 3; i++) {
            float a = 0.4f + 0.6f * (0.5f + 0.5f * sinf(t - i * 0.9f));
            gfx_circle(148 + i * 12, 206, 3.5f, col_alpha(COL_BG, a));
        }
    } else {
        gfx_text(F_BOLD, "Connect", 160, 196, 17, COL_BG, AL_CENTER);
    }
    if (g_msg[0]) gfx_text_fit(F_UI, g_msg, 160, 230 - 2, 12, 300, COL_DANGER, AL_CENTER);
}

const Screen scr_login = {enter, update, draw_top, draw_bottom};
