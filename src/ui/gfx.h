/* Drawing layer on citro2d: palette, fonts, anti-aliased shapes (from
 * generated distance-field textures), soft shadows, icons and text. */
#ifndef SHELF3DS_GFX_H
#define SHELF3DS_GFX_H
#include <citro2d.h>
#include <stdbool.h>

/* palette (ABGR via C2D_Color32) */
#define RGBA(r, g, b, a) ((u32)(r) | (u32)(g) << 8 | (u32)(b) << 16 | (u32)(a) << 24)
#define COL_BG RGBA(14, 17, 22, 255)
#define COL_SURFACE RGBA(23, 27, 34, 255)
#define COL_SURFACE2 RGBA(32, 37, 47, 255)
#define COL_LINE RGBA(44, 50, 62, 255)
#define COL_TEXT RGBA(242, 238, 230, 255)
#define COL_DIM RGBA(154, 160, 171, 255)
#define COL_FAINT RGBA(92, 99, 112, 255)
#define COL_AMBER RGBA(247, 168, 55, 255)
#define COL_DANGER RGBA(240, 98, 92, 255)
#define COL_OK RGBA(98, 210, 140, 255)

typedef enum { F_UI, F_BOLD, F_SYS } FontId;
enum { AL_LEFT = 0, AL_CENTER = 1, AL_RIGHT = 2 };

#include "icons.h"

void gfx_init(void);
void gfx_exit(void);
void gfx_frame_begin(void);

u32 col_alpha(u32 c, float a);
u32 col_mix(u32 a, u32 b, float t);

void gfx_rect(float x, float y, float w, float h, u32 c);
void gfx_gradient_v(float x, float y, float w, float h, u32 top, u32 bottom);
void gfx_gradient_h(float x, float y, float w, float h, u32 left, u32 right);
void gfx_rrect(float x, float y, float w, float h, float r, u32 c);
void gfx_rrect_outline(float x, float y, float w, float h, float r, float t, u32 c, u32 inner);
/* Focus ring: a rounded rect t px larger than (x,y,w,h). Draw it *before*
 * the element so the element covers the middle. */
void gfx_focus_ring(float x, float y, float w, float h, float r, float t, u32 c);
void gfx_circle(float cx, float cy, float r, u32 c);
void gfx_shadow(float x, float y, float w, float h, float spread, float alpha);
void gfx_icon(IconId icon, float cx, float cy, float size, u32 c);
void gfx_image(const C2D_Image *img, float x, float y, float w, float h, u32 tint, float tint_blend);
void gfx_progress(float x, float y, float w, float h, float frac, u32 fg, u32 bg);

float gfx_text(FontId f, const char *s, float x, float y, float px, u32 c, int align);
/* Truncates with an ellipsis to fit maxw (cached). Returns drawn width. */
float gfx_text_fit(FontId f, const char *s, float x, float y, float px, float maxw, u32 c, int align);
float gfx_text_width(FontId f, const char *s, float px);
/* Word-wraps s into at most maxlines lines of width maxw (the last line gets
 * an ellipsis if cut). Writes into a caller buffer of lines. Returns count. */
int gfx_wrap(FontId f, const char *s, float px, float maxw, char (*lines)[160], int maxlines);

/* procedural placeholder cover: tinted gradient with the title's initial */
void gfx_placeholder_cover(const char *title, float x, float y, float size);

#endif
