#include "texswz.h"

uint32_t morton8(uint32_t x, uint32_t y) {
    /* interleave the low 3 bits: x0 y0 x1 y1 x2 y2 */
    uint32_t m = 0;
    for (int i = 0; i < 3; i++) {
        m |= ((x >> i) & 1) << (2 * i);
        m |= ((y >> i) & 1) << (2 * i + 1);
    }
    return m;
}

void tex_swizzle_rgba8(const uint8_t *src, int w, int h, uint32_t *dst) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint8_t *p = src + (y * w + x) * 4;
            /* rows are written top-down; the C2D subtexture uses top = 1.0 */
            int ty = y;
            uint32_t tile = (uint32_t)((ty >> 3) * (w >> 3) + (x >> 3));
            uint32_t idx = tile * 64 + morton8(x & 7, ty & 7);
            dst[idx] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
        }
    }
}

void img_crop_resize(const uint8_t *src, int sw, int sh, int comps, uint8_t *dst, int out_sz) {
    int side = sw < sh ? sw : sh;
    int ox = (sw - side) / 2, oy = (sh - side) / 2;
    for (int y = 0; y < out_sz; y++) {
        int y0 = oy + y * side / out_sz, y1 = oy + (y + 1) * side / out_sz;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < out_sz; x++) {
            int x0 = ox + x * side / out_sz, x1 = ox + (x + 1) * side / out_sz;
            if (x1 <= x0) x1 = x0 + 1;
            unsigned acc[4] = {0, 0, 0, 0}, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    const uint8_t *p = src + (yy * sw + xx) * comps;
                    acc[0] += p[0];
                    acc[1] += comps >= 3 ? p[1] : p[0];
                    acc[2] += comps >= 3 ? p[2] : p[0];
                    acc[3] += comps == 4 ? p[3] : (comps == 2 ? p[1] : 255);
                    n++;
                }
            uint8_t *d = dst + (y * out_sz + x) * 4;
            for (int c = 0; c < 4; c++) d[c] = (uint8_t)(acc[c] / n);
        }
    }
}
