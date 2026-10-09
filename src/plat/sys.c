#include "sys.h"

#include <3ds.h>

#include "dbg.h"
#include "http.h"

static bool g_lid_closed, g_backlight = true, g_new3ds, g_sleep_allowed = true;
static int g_battery = 5, g_wifi;
static bool g_charging, g_wifi_ok;
static u64 g_last_poll;
static aptHookCookie g_cookie;
static void (*g_on_suspend)(void);
static const char *g_model = "3DS";

static volatile bool g_suspended;
bool sys_suspended(void) { return g_suspended; }

static void hook(APT_HookType t, void *p) {
    (void)p;
    static const char *names[] = {"suspend", "restore", "sleep", "wakeup", "exit"};
    dbg("apt hook: %s (begin)", t < 5 ? names[t] : "?");
    if (t == APTHOOK_ONSUSPEND || t == APTHOOK_ONSLEEP) g_suspended = true;
    if ((t == APTHOOK_ONSUSPEND || t == APTHOOK_ONSLEEP || t == APTHOOK_ONEXIT) && g_on_suspend) g_on_suspend();
    if (t == APTHOOK_ONWAKEUP || t == APTHOOK_ONRESTORE) {
        /* the system turns the backlights back on after sleep */
        g_backlight = true;
        g_suspended = false;
    }
    dbg("apt hook: %s (end)", t < 5 ? names[t] : "?");
}

void sys_init(void (*on_suspend)(void)) {
    g_on_suspend = on_suspend;
    ptmuInit();
    acInit();
    cfguInit();
    APT_CheckNew3DS(&g_new3ds);
    if (g_new3ds) osSetSpeedupEnable(true);
    u8 model = 0;
    if (R_SUCCEEDED(CFGU_GetSystemModel(&model))) {
        static const char *names[] = {"3DS", "3DS XL", "New 3DS", "2DS", "New 3DS XL", "New 2DS XL"};
        if (model < 6) g_model = names[model];
    }
    aptHook(&g_cookie, hook, NULL);
    dbg("start: Shelf3DS %s on %s", APP_VERSION, g_model);
    sys_poll();
}

void sys_exit(void) {
    aptUnhook(&g_cookie);
    if (!g_backlight) sys_backlight(true);
    aptSetSleepAllowed(true);
    cfguExit();
    acExit();
    ptmuExit();
}

void sys_poll(void) {
    u64 now = osGetTime();
    if (now - g_last_poll < 250) return;
    g_last_poll = now;
    u8 v = 0;
    if (R_SUCCEEDED(PTMU_GetShellState(&v))) g_lid_closed = v == 0;
    if (R_SUCCEEDED(PTMU_GetBatteryLevel(&v))) g_battery = v;
    if (R_SUCCEEDED(PTMU_GetBatteryChargeState(&v))) g_charging = v != 0;
    u32 ws = 0;
    g_wifi_ok = R_SUCCEEDED(ACU_GetWifiStatus(&ws)) && ws != 0;
    g_wifi = g_wifi_ok ? osGetWifiStrength() : 0;
}

bool sys_lid_closed(void) { return g_lid_closed; }
bool sys_headphones(void) { return osIsHeadsetConnected(); }
int sys_battery(void) { return g_battery; }
bool sys_charging(void) { return g_charging; }
int sys_wifi(void) { return g_wifi; }
bool sys_wifi_connected(void) { return g_wifi_ok; }
bool sys_is_new3ds(void) { return g_new3ds; }
const char *sys_model(void) { return g_model; }

void sys_backlight(bool on) {
    if (on == g_backlight) return;
    /* hold the gsp::Lcd session only for the call itself, so we never
     * compete with the HOME Menu or applets for it */
    if (R_FAILED(gspLcdInit())) return;
    if (on) GSPLCD_PowerOnAllBacklights();
    else GSPLCD_PowerOffAllBacklights();
    gspLcdExit();
    g_backlight = on;
}

bool sys_backlight_on(void) { return g_backlight; }

void sys_allow_sleep(bool allow) {
    if (allow == g_sleep_allowed) return;
    aptSetSleepAllowed(allow);
    g_sleep_allowed = allow;
}

unsigned long long sys_now_ms(void) {
    /* osGetTime counts ms since 1900-01-01 in *local* time; the HTTP layer
     * learns the offset to the server's UTC clock */
    long long ms = (long long)osGetTime() - 2208988800000LL + http_clock_offset(NULL);
    return ms > 0 ? (unsigned long long)ms : 0;
}
