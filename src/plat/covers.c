#include "covers.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"

#include "abs_client.h"
#include "color.h"
#include "strutil.h"
#include "texswz.h"
#include "worker.h"

#define COVER_DIR "sdmc:/3ds/shelf3ds/covers"
#define NTHUMB 48
#define NACCENT 160
#define THUMB 128
#define HERO 256
#define BLUR 32

typedef struct {
    char id[40];
    CoverState state;
    C3D_Tex tex;
    bool tex_ok;
    C2D_Image img;
    u32 last_used;
    u64 failed_at;
} Slot;

typedef struct CoverJob {
    char id[40];
    bool hero, ok;
    u32 *thumb;   /* THUMB^2 swizzled */
    u32 *big;     /* HERO^2 swizzled (hero only) */
    u32 *blur;    /* BLUR^2 swizzled (hero only) */
    u32 accent;
    struct CoverJob *next;
} CoverJob;

static Slot g_thumbs[NTHUMB];
static Slot g_hero, g_blur;
static struct { char id[40]; u32 color; } g_accent[NACCENT];
static int g_accent_next;
static u32 g_frame;
static Tex3DS_SubTexture g_sub = {0, 0, 0.0f, 1.0f, 1.0f, 0.0f};

/* request stack (newest first) */
static LightLock g_lock;
static struct { char id[40]; bool hero; } g_req[64];
static int g_nreq;
static CoverJob *g_done;
static LightEvent g_ev;
static Thread g_th;
static volatile bool g_quit;

static void set_accent(const char *id, u32 c) {
    for (int i = 0; i < NACCENT; i++)
        if (!strcmp(g_accent[i].id, id)) { g_accent[i].color = c; return; }
    str_copy(g_accent[g_accent_next].id, 40, id);
    g_accent[g_accent_next].color = c;
    g_accent_next = (g_accent_next + 1) % NACCENT;
}

bool covers_accent(const char *id, u32 *color) {
    for (int i = 0; i < NACCENT; i++)
        if (g_accent[i].id[0] && !strcmp(g_accent[i].id, id)) { *color = g_accent[i].color; return true; }
    return false;
}

static u8 *load_jpeg(const char *id, u32 *len) {
    char path[128];
    snprintf(path, sizeof path, COVER_DIR "/%s.jpg", id);
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        u8 *d = n > 0 ? malloc(n) : NULL;
        if (d && fread(d, 1, n, f) == (size_t)n) { fclose(f); *len = (u32)n; return d; }
        free(d);
        fclose(f);
    }
    u8 *d = NULL;
    if (absc_fetch_cover(id, HERO, &d, len) != ABS_OK) return NULL;
    mkdir(COVER_DIR, 0777);
    f = fopen(path, "wb");
    if (f) { fwrite(d, 1, *len, f); fclose(f); }
    return d;
}

static u32 *make_tex(const u8 *rgba, int w, int h, int size) {
    u8 *tmp = malloc((size_t)size * size * 4);
    u32 *out = malloc((size_t)size * size * 4);
    if (!tmp || !out) { free(tmp); free(out); return NULL; }
    img_crop_resize(rgba, w, h, 4, tmp, size);
    tex_swizzle_rgba8(tmp, size, size, out);
    free(tmp);
    return out;
}

static void decode_job(const char *id, bool hero) {
    CoverJob *r = calloc(1, sizeof *r);
    if (!r) return;
    str_copy(r->id, sizeof r->id, id);
    r->hero = hero;
    u32 len = 0;
    u8 *jpg = load_jpeg(id, &len);
    if (jpg) {
        int w, h, comp;
        u8 *px = stbi_load_from_memory(jpg, (int)len, &w, &h, &comp, 4);
        free(jpg);
        if (px && w > 0 && h > 0) {
            u8 small[BLUR * BLUR * 4];
            img_crop_resize(px, w, h, 4, small, BLUR);
            Rgb a = accent_from_rgba(small, BLUR, BLUR);
            r->accent = C2D_Color32(a.r, a.g, a.b, 255);
            r->thumb = make_tex(px, w, h, THUMB);
            if (hero) {
                r->big = make_tex(px, w, h, HERO);
                r->blur = malloc(BLUR * BLUR * 4);
                if (r->blur) tex_swizzle_rgba8(small, BLUR, BLUR, r->blur);
            }
            r->ok = r->thumb && (!hero || (r->big && r->blur));
        }
        stbi_image_free(px);
    }
    LightLock_Lock(&g_lock);
    r->next = g_done;
    g_done = r;
    LightLock_Unlock(&g_lock);
}

static void cover_thread(void *arg) {
    (void)arg;
    while (!g_quit) {
        LightEvent_Wait(&g_ev);
        for (;;) {
            LightLock_Lock(&g_lock);
            if (!g_nreq || g_quit) { LightLock_Unlock(&g_lock); break; }
            char id[40];
            bool hero = g_req[0].hero;
            str_copy(id, sizeof id, g_req[0].id);
            memmove(g_req, g_req + 1, sizeof g_req[0] * (--g_nreq));
            LightLock_Unlock(&g_lock);
            decode_job(id, hero);
        }
    }
}

static void request(const char *id, bool hero) {
    LightLock_Lock(&g_lock);
    for (int i = 0; i < g_nreq; i++) {
        if (!strcmp(g_req[i].id, id)) {
            hero |= g_req[i].hero;
            memmove(g_req + i, g_req + i + 1, sizeof g_req[0] * (g_nreq - i - 1));
            g_nreq--;
            break;
        }
    }
    if (g_nreq == 64) g_nreq--; /* drop the oldest request */
    memmove(g_req + 1, g_req, sizeof g_req[0] * g_nreq);
    str_copy(g_req[0].id, 40, id);
    g_req[0].hero = hero;
    g_nreq++;
    LightLock_Unlock(&g_lock);
    LightEvent_Signal(&g_ev);
}

void covers_init(void) {
    LightLock_Init(&g_lock);
    LightEvent_Init(&g_ev, RESET_ONESHOT);
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/shelf3ds", 0777);
    mkdir(COVER_DIR, 0777);
    g_th = thread_spawn(cover_thread, NULL, 64 * 1024, 0x30, 1);
}

static void free_slot(Slot *s) {
    if (s->tex_ok) C3D_TexDelete(&s->tex);
    memset(s, 0, sizeof *s);
}

void covers_exit(void) {
    g_quit = true;
    LightEvent_Signal(&g_ev);
    if (g_th) { threadJoin(g_th, U64_MAX); threadFree(g_th); }
    for (int i = 0; i < NTHUMB; i++) free_slot(&g_thumbs[i]);
    free_slot(&g_hero);
    free_slot(&g_blur);
    while (g_done) { CoverJob *n = g_done->next; free(g_done->thumb); free(g_done->big); free(g_done->blur); free(g_done); g_done = n; }
}

static bool upload(Slot *s, const u32 *px, int size, bool linear) {
    if (!s->tex_ok) {
        if (!C3D_TexInit(&s->tex, size, size, GPU_RGBA8)) return false;
        s->tex_ok = true;
    }
    memcpy(s->tex.data, px, (size_t)size * size * 4);
    C3D_TexFlush(&s->tex);
    C3D_TexSetFilter(&s->tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&s->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    (void)linear;
    s->img.tex = &s->tex;
    s->img.subtex = &g_sub;
    return true;
}

static Slot *find_thumb(const char *id) {
    for (int i = 0; i < NTHUMB; i++)
        if (g_thumbs[i].state != COVER_NONE && !strcmp(g_thumbs[i].id, id)) return &g_thumbs[i];
    return NULL;
}

static Slot *alloc_thumb(const char *id) {
    Slot *best = NULL;
    for (int i = 0; i < NTHUMB; i++) {
        Slot *s = &g_thumbs[i];
        if (s->state == COVER_NONE) { best = s; break; }
        if (s->state == COVER_LOADING) continue;
        if (!best || s->last_used < best->last_used) best = s;
    }
    if (!best) return NULL;
    bool keep_tex = best->tex_ok;
    C3D_Tex tex = best->tex;
    memset(best, 0, sizeof *best);
    best->tex = tex;
    best->tex_ok = keep_tex; /* textures are reused between covers */
    str_copy(best->id, sizeof best->id, id);
    return best;
}

void covers_update(void) {
    g_frame++;
    LightLock_Lock(&g_lock);
    CoverJob *r = g_done;
    g_done = NULL;
    LightLock_Unlock(&g_lock);
    while (r) {
        CoverJob *next = r->next;
        if (r->ok) set_accent(r->id, r->accent);
        Slot *t = find_thumb(r->id);
        if (t) {
            if (r->ok && upload(t, r->thumb, THUMB, true)) t->state = COVER_READY;
            else { t->state = COVER_MISSING; t->failed_at = osGetTime(); }
        }
        if (r->hero && !strcmp(g_hero.id, r->id)) {
            if (r->ok && upload(&g_hero, r->big, HERO, true) && upload(&g_blur, r->blur, BLUR, true)) g_hero.state = COVER_READY;
            else { g_hero.state = COVER_MISSING; g_hero.failed_at = osGetTime(); }
        }
        free(r->thumb);
        free(r->big);
        free(r->blur);
        free(r);
        r = next;
    }
}

const C2D_Image *covers_thumb(const char *id, CoverState *st) {
    if (!id || !id[0]) { if (st) *st = COVER_MISSING; return NULL; }
    Slot *s = find_thumb(id);
    if (!s) {
        s = alloc_thumb(id);
        if (!s) { if (st) *st = COVER_LOADING; return NULL; }
        s->state = COVER_LOADING;
        request(id, false);
    }
    s->last_used = g_frame;
    if (s->state == COVER_MISSING && osGetTime() - s->failed_at > 30000) {
        s->state = COVER_LOADING;
        request(id, false);
    }
    if (st) *st = s->state;
    return s->state == COVER_READY ? &s->img : NULL;
}

const C2D_Image *covers_hero(const char *id, const C2D_Image **blur, CoverState *st) {
    if (blur) *blur = NULL;
    if (!id || !id[0]) { if (st) *st = COVER_MISSING; return NULL; }
    if (strcmp(g_hero.id, id) != 0) {
        str_copy(g_hero.id, sizeof g_hero.id, id);
        g_hero.state = COVER_LOADING;
        request(id, true);
    } else if (g_hero.state == COVER_MISSING && osGetTime() - g_hero.failed_at > 30000) {
        g_hero.state = COVER_LOADING;
        request(id, true);
    }
    if (st) *st = g_hero.state;
    if (g_hero.state != COVER_READY) return NULL;
    if (blur) *blur = &g_blur.img;
    return &g_hero.img;
}

void covers_retry_missing(void) {
    for (int i = 0; i < NTHUMB; i++)
        if (g_thumbs[i].state == COVER_MISSING) g_thumbs[i].failed_at = 0;
    if (g_hero.state == COVER_MISSING) g_hero.failed_at = 0;
}
