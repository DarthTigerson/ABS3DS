/* Converts linear RGBA8 pixels into the PICA200's tiled layout. */
#ifndef ABS3DS_TEXSWZ_H
#define ABS3DS_TEXSWZ_H
#include <stdint.h>

/* Morton index of (x, y) inside an 8x8 tile */
uint32_t morton8(uint32_t x, uint32_t y);
/* src: w*h RGBA (r,g,b,a bytes, top row first); dst: w*h u32 in GPU_RGBA8
 * tiled order (u32 = r<<24|g<<16|b<<8|a), top row first; use a
 * subtexture with top = 1.0. w and h must
 * be multiples of 8. */
void tex_swizzle_rgba8(const uint8_t *src, int w, int h, uint32_t *dst);
/* Box/bilinear resize + centre crop to a square of size out_sz (RGBA8). */
void img_crop_resize(const uint8_t *src, int sw, int sh, int comps, uint8_t *dst, int out_sz);

#endif
