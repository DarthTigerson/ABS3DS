#include "gfx.h"

#include <3ds.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "icons.h"
#include "strutil.h"
#include "texswz.h"

/* mask atlas layout (512x256): icons in 48px cells (8 per row) from (0,0);
 * a hard disc at (384,0) and a soft (shadow) disc at (448,0), 64x64 each. */
#define AT_W 512
#define AT_H 256
#define CELL 48
#define DISC_X 384
#define SOFT_X 448
#define DISC 64

static C3D_Tex g_atlas;
static Tex3DS_SubTexture g_icon_sub[IC_COUNT];
static Tex3DS_SubTexture g_disc_q[4], g_soft_q[4], g_disc_full, g_soft_top, g_soft_bottom, g_soft_left, g_soft_right;
static C2D_Font g_font[2];
static C2D_TextBuf g_buf;

/* ---- colours ---------------------------------------------------------------- */

u32 col_alpha(u32 c, float a) {
    u32 al = (u32)((c >> 24) * (a < 0 ? 0 : a > 1 ? 1 : a));
    return (c & 0x00FFFFFF) | al << 24;
}

u32 col_mix(u32 a, u32 b, float t) {
    if (t <= 0) return a;
    if (t >= 1) return b;
    u32 out = 0;
    for (int i = 0; i < 32; i += 8) {
        float x = ((a >> i) & 255) * (1 - t) + ((b >> i) & 255) * t;
        out |= (u32)(x + 0.5f) << i;
    }
    return out;
}

/* ---- atlas -------------------------------------------------------------------- */

static void sub_px(Tex3DS_SubTexture *s, float x, float y, float w, float h) {
    s->width = (u16)w;
    s->height = (u16)h;
    s->left = x / AT_W;
    s->right = (x + w) / AT_W;
    s->top = 1.0f - y / AT_H;
    s->bottom = 1.0f - (y + h) / AT_H;
}

/* white RGBA8 with the mask in alpha: tints correctly in every C2D tint mode */
static void swizzle_mask(const u8 *src, int w, int h, u32 *dst) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            u32 tile = (u32)((y >> 3) * (w >> 3) + (x >> 3));
            dst[tile * 64 + morton8(x & 7, y & 7)] = 0xFFFFFF00u | src[y * w + x];
        }
}

static void build_atlas(void) {
    u8 *lin = calloc(AT_W * AT_H, 1);
    if (!lin) return;
    for (int i = 0; i < IC_COUNT; i++) {
        int cx = (i % 8) * CELL, cy = (i / 8) * CELL;
        icons_raster(i, lin + cy * AT_W + cx, AT_W, CELL);
        sub_px(&g_icon_sub[i], cx, cy, CELL, CELL);
    }
    float r = DISC / 2.0f;
    for (int y = 0; y < DISC; y++)
        for (int x = 0; x < DISC; x++) {
            float dx = x + 0.5f - r, dy = y + 0.5f - r;
            float d = sqrtf(dx * dx + dy * dy);
            float a = r - 0.5f - d + 0.5f; /* 1px AA edge */
            lin[y * AT_W + DISC_X + x] = (u8)(a <= 0 ? 0 : a >= 1 ? 255 : a * 255);
            /* soft disc: smooth falloff from the centre to the edge */
            float t = d / r;
            float s = t >= 1 ? 0 : (1 - t) * (1 - t) * (3 - 2 * (1 - t)) ;
            lin[y * AT_W + SOFT_X + x] = (u8)(s * 255);
        }
    if (C3D_TexInit(&g_atlas, AT_W, AT_H, GPU_RGBA8)) {
        swizzle_mask(lin, AT_W, AT_H, g_atlas.data);
        C3D_TexFlush(&g_atlas);
        C3D_TexSetFilter(&g_atlas, GPU_LINEAR, GPU_LINEAR);
    }
    free(lin);
    float h = DISC / 2.0f;
    /* quadrants: 0 TL, 1 TR, 2 BL, 3 BR (insetting half a texel avoids bleeding) */
    sub_px(&g_disc_q[0], DISC_X, 0, h, h);
    sub_px(&g_disc_q[1], DISC_X + h, 0, h, h);
    sub_px(&g_disc_q[2], DISC_X, h, h, h);
    sub_px(&g_disc_q[3], DISC_X + h, h, h, h);
    sub_px(&g_soft_q[0], SOFT_X, 0, h, h);
    sub_px(&g_soft_q[1], SOFT_X + h, 0, h, h);
    sub_px(&g_soft_q[2], SOFT_X, h, h, h);
    sub_px(&g_soft_q[3], SOFT_X + h, h, h, h);
    sub_px(&g_disc_full, DISC_X, 0, DISC, DISC);
    /* 1-texel strips through the centre of the soft disc, one per side */
    sub_px(&g_soft_top, SOFT_X + h - 0.5f, 0, 1, h);
    sub_px(&g_soft_bottom, SOFT_X + h - 0.5f, h, 1, h);
    sub_px(&g_soft_left, SOFT_X, h - 0.5f, h, 1);
    sub_px(&g_soft_right, SOFT_X + h, h - 0.5f, h, 1);
}

void gfx_init(void) {
    build_atlas();
    g_font[0] = C2D_FontLoad("romfs:/ui.bcfnt");
    g_font[1] = C2D_FontLoad("romfs:/ui_bold.bcfnt");
    g_buf = C2D_TextBufNew(16384);
}

void gfx_exit(void) {
    C3D_TexDelete(&g_atlas);
    if (g_font[0]) C2D_FontFree(g_font[0]);
    if (g_font[1]) C2D_FontFree(g_font[1]);
    C2D_TextBufDelete(g_buf);
}

void gfx_frame_begin(void) { C2D_TextBufClear(g_buf); }

/* ---- shapes ----------------------------------------------------------------- */

void gfx_rect(float x, float y, float w, float h, u32 c) {
    if (w <= 0 || h <= 0) return;
    C2D_DrawRectSolid(x, y, 0.5f, w, h, c);
}

void gfx_gradient_v(float x, float y, float w, float h, u32 top, u32 bottom) {
    C2D_DrawRectangle(x, y, 0.5f, w, h, top, top, bottom, bottom);
}

void gfx_gradient_h(float x, float y, float w, float h, u32 left, u32 right) {
    C2D_DrawRectangle(x, y, 0.5f, w, h, left, right, left, right);
}

static void draw_sub(const Tex3DS_SubTexture *s, float x, float y, float w, float h, u32 c) {
    C2D_Image img = {&g_atlas, s};
    C2D_ImageTint t;
    C2D_PlainImageTint(&t, c, 1.0f);
    C2D_DrawParams p = {{x, y, w, h}, {0, 0}, 0.5f, 0};
    C2D_DrawImage(img, &p, &t);
}

void gfx_rrect(float x, float y, float w, float h, float r, u32 c) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 1) { gfx_rect(x, y, w, h, c); return; }
    gfx_rect(x + r, y, w - 2 * r, h, c);
    gfx_rect(x, y + r, r, h - 2 * r, c);
    gfx_rect(x + w - r, y + r, r, h - 2 * r, c);
    draw_sub(&g_disc_q[0], x, y, r, r, c);
    draw_sub(&g_disc_q[1], x + w - r, y, r, r, c);
    draw_sub(&g_disc_q[2], x, y + h - r, r, r, c);
    draw_sub(&g_disc_q[3], x + w - r, y + h - r, r, r, c);
}

void gfx_rrect_outline(float x, float y, float w, float h, float r, float t, u32 c, u32 inner) {
    gfx_rrect(x, y, w, h, r, c);
    gfx_rrect(x + t, y + t, w - 2 * t, h - 2 * t, r - t > 0 ? r - t : 0, inner);
}

void gfx_focus_ring(float x, float y, float w, float h, float r, float t, u32 c) {
    gfx_rrect(x - t, y - t, w + 2 * t, h + 2 * t, r + t, c);
}

void gfx_circle(float cx, float cy, float r, u32 c) {
    draw_sub(&g_disc_full, cx - r, cy - r, 2 * r, 2 * r, c);
}

void gfx_shadow(float x, float y, float w, float h, float s, float alpha) {
    u32 c = RGBA(0, 0, 0, (u8)(alpha * 255));
    /* corners */
    draw_sub(&g_soft_q[0], x - s, y - s, 2 * s, 2 * s, c);
    draw_sub(&g_soft_q[1], x + w - s, y - s, 2 * s, 2 * s, c);
    draw_sub(&g_soft_q[2], x - s, y + h - s, 2 * s, 2 * s, c);
    draw_sub(&g_soft_q[3], x + w - s, y + h - s, 2 * s, 2 * s, c);
    /* edges */
    if (w > 2 * s) {
        draw_sub(&g_soft_top, x + s, y - s, w - 2 * s, 2 * s, c);
        draw_sub(&g_soft_bottom, x + s, y + h - s, w - 2 * s, 2 * s, c);
    }
    if (h > 2 * s) {
        draw_sub(&g_soft_left, x - s, y + s, 2 * s, h - 2 * s, c);
        draw_sub(&g_soft_right, x + w - s, y + s, 2 * s, h - 2 * s, c);
    }
    if (w > 2 * s && h > 2 * s) gfx_rect(x + s, y + s, w - 2 * s, h - 2 * s, c);
}

void gfx_icon(IconId icon, float cx, float cy, float size, u32 c) {
    if (icon >= IC_COUNT) return;
    draw_sub(&g_icon_sub[icon], cx - size / 2, cy - size / 2, size, size, c);
}

void gfx_image(const C2D_Image *img, float x, float y, float w, float h, u32 tint, float blend) {
    if (!img) return;
    C2D_DrawParams p = {{x, y, w, h}, {0, 0}, 0.5f, 0};
    /* tint == 0 means "no tint" (not "transparent black") */
    if (tint != 0 && (blend > 0 || (tint >> 24) != 255)) {
        C2D_ImageTint t;
        C2D_PlainImageTint(&t, tint, blend);
        C2D_DrawImage(*img, &p, &t);
    } else {
        C2D_DrawImage(*img, &p, NULL);
    }
}

void gfx_progress(float x, float y, float w, float h, float frac, u32 fg, u32 bg) {
    frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
    gfx_rrect(x, y, w, h, h / 2, bg);
    if (frac > 0) gfx_rrect(x, y, fmaxf(h, w * frac), h, h / 2, fg);
}

/* ---- text ----------------------------------------------------------------------- */

static C2D_Font font_of(FontId f) {
    if (f == F_SYS) return NULL;
    C2D_Font ft = g_font[f == F_BOLD ? 1 : 0];
    return ft; /* NULL (system font) if the bundled font failed to load */
}

/* px is the em size. Divisors calibrated on screen for the mkbcfnt output
 * (ui: -s 17, bold: -s 24) so both give a ~10px cap height at px = 14;
 * the system font's line feed is ~1.3 em. */
#define UI_PT 20.5f
#define BOLD_PT 18.8f
static float scale_of(FontId f, float px) {
    C2D_Font ft = font_of(f);
    if (ft && f == F_UI) return px / UI_PT;
    if (ft && f == F_BOLD) return px / BOLD_PT;
    FINF_s *info = C2D_FontGetInfo(ft);
    float lf = info && info->lineFeed ? info->lineFeed : 30;
    return px * 1.3f / lf;
}

static bool parse(FontId f, const char *s, C2D_Text *t) {
    if (!s || !*s) return false;
    if (!C2D_TextFontParse(t, font_of(f), g_buf, s)) return false;
    C2D_TextOptimize(t);
    return true;
}

float gfx_text_width(FontId f, const char *s, float px) {
    C2D_Text t;
    if (!parse(f, s, &t)) return 0;
    float sc = scale_of(f, px), w, h;
    C2D_TextGetDimensions(&t, sc, sc, &w, &h);
    return w;
}

float gfx_text(FontId f, const char *s, float x, float y, float px, u32 c, int align) {
    C2D_Text t;
    if (!parse(f, s, &t)) return 0;
    float sc = scale_of(f, px), w, h;
    C2D_TextGetDimensions(&t, sc, sc, &w, &h);
    float dx = align == AL_CENTER ? x - w / 2 : align == AL_RIGHT ? x - w : x;
    C2D_DrawText(&t, C2D_WithColor, dx, y, 0.5f, sc, sc, c);
    return w;
}

/* Measured on screen (bundled fonts, citro2d): capitals start ~0.33 em below the
 * line-box top and are ~0.715 em tall, so their middle is ~0.69 em below it. */
#define CAP_MID_EM 0.69f

float gfx_text_mid(FontId f, const char *s, float x, float cy, float px, u32 c, int align) {
    return gfx_text(f, s, x, cy - px * CAP_MID_EM, px, c, align);
}

/* cache of truncation points: hash(font, px, maxw, string) -> byte length */
#define FIT_CACHE 512
static struct { u32 key; u16 len; } g_fit[FIT_CACHE];

static u32 fnv(const char *s, u32 h) {
    while (*s) { h ^= (u8)*s++; h *= 16777619u; }
    return h;
}

static int utf8_prev(const char *s, int i) {
    if (i <= 0) return 0;
    i--;
    while (i > 0 && ((u8)s[i] & 0xC0) == 0x80) i--;
    return i;
}

static int fit_len(FontId f, const char *s, float px, float maxw) {
    int n = (int)strlen(s);
    if (gfx_text_width(f, s, px) <= maxw) return n;
    float ell = gfx_text_width(f, "\xE2\x80\xA6", px);
    /* binary search on byte length, snapped to UTF-8 boundaries */
    int lo = 0, hi = n;
    char tmp[256];
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        int m = mid;
        while (m > 0 && ((u8)s[m] & 0xC0) == 0x80) m--;
        if (m >= (int)sizeof tmp - 4) m = sizeof tmp - 5;
        memcpy(tmp, s, m);
        tmp[m] = 0;
        if (gfx_text_width(f, tmp, px) + ell <= maxw) lo = mid; else hi = mid - 1;
    }
    while (lo > 0 && ((u8)s[lo] & 0xC0) == 0x80) lo--;
    while (lo > 0 && s[lo - 1] == ' ') lo = utf8_prev(s, lo);
    return lo;
}

float gfx_text_fit(FontId f, const char *s, float x, float y, float px, float maxw, u32 c, int align) {
    if (!s || !*s) return 0;
    u32 key = fnv(s, 2166136261u ^ (u32)f * 7919u ^ (u32)(px * 16) * 31u ^ (u32)(maxw * 4) * 131u) | 1;
    u32 slot = key % FIT_CACHE;
    int len;
    if (g_fit[slot].key == key) len = g_fit[slot].len;
    else {
        len = fit_len(f, s, px, maxw);
        g_fit[slot].key = key;
        g_fit[slot].len = (u16)len;
    }
    if (s[len] == 0) return gfx_text(f, s, x, y, px, c, align);
    char tmp[260];
    if (len > 250) len = 250;
    memcpy(tmp, s, len);
    memcpy(tmp + len, "\xE2\x80\xA6", 4);
    return gfx_text(f, tmp, x, y, px, c, align);
}

int gfx_wrap(FontId f, const char *s, float px, float maxw, char (*lines)[160], int maxlines) {
    int n = 0;
    if (!s) return 0;
    const char *p = s;
    while (*p && n < maxlines) {
        while (*p == ' ') p++;
        if (!*p) break;
        /* take words while they fit */
        char line[160] = "";
        const char *q = p, *last_fit = NULL;
        while (*q) {
            const char *e = q;
            while (*e && *e != ' ' && *e != '\n') e++;
            int l = (int)(e - p);
            if (l >= (int)sizeof line) break;
            memcpy(line, p, l);
            line[l] = 0;
            if (gfx_text_width(f, line, px) > maxw) break;
            last_fit = e;
            if (*e == '\n' || !*e) break;
            q = e + 1;
        }
        if (!last_fit) {
            /* a single long word: hard-cut it */
            int l = fit_len(f, p, px, maxw);
            if (l <= 0) l = 1;
            last_fit = p + l;
        }
        int l = (int)(last_fit - p);
        if (l >= 159) l = 159;
        memcpy(lines[n], p, l);
        lines[n][l] = 0;
        n++;
        p = last_fit;
        if (*p == '\n') p++;
    }
    if (*p && n == maxlines && n > 0) {
        /* more text remains: ellipsise the last line */
        char *last = lines[n - 1];
        int keep = fit_len(f, last, px, maxw - gfx_text_width(f, "\xE2\x80\xA6", px));
        if (keep > 155) keep = 155;
        last[keep] = 0;
        strcat(last, "\xE2\x80\xA6");
    }
    return n;
}

/* ---- placeholder cover ---------------------------------------------------------- */

void gfx_placeholder_cover(const char *title, float x, float y, float size) {
    u32 h = fnv(title ? title : "", 2166136261u);
    static const u32 tints[] = {RGBA(84, 64, 140, 255), RGBA(38, 104, 120, 255), RGBA(150, 72, 60, 255),
                                RGBA(60, 110, 70, 255), RGBA(140, 100, 40, 255), RGBA(100, 52, 96, 255)};
    u32 a = tints[h % 6], b = col_mix(a, COL_BG, 0.55f);
    C2D_DrawRectangle(x, y, 0.5f, size, size, a, col_mix(a, b, 0.5f), col_mix(a, b, 0.5f), b);
    char initial[8] = "?";
    if (title && *title) {
        int l = 1;
        while (title[l] && ((u8)title[l] & 0xC0) == 0x80) l++;
        if (l > 6) l = 6;
        memcpy(initial, title, l);
        initial[l] = 0;
    }
    bool ascii = (u8)initial[0] < 0x80;
    gfx_text(ascii ? F_BOLD : F_SYS, initial, x + size / 2, y + size * 0.24f, size * 0.42f, col_alpha(COL_TEXT, 0.85f), AL_CENTER);
    gfx_icon(IC_BOOK, x + size - size * 0.16f, y + size - size * 0.16f, size * 0.16f, col_alpha(COL_TEXT, 0.5f));
}
