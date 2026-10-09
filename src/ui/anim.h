/* Small animation helpers. */
#ifndef ABS3DS_ANIM_H
#define ABS3DS_ANIM_H
#include <math.h>

/* Frame-rate independent exponential approach: rate ~ 1/seconds to settle. */
static inline float approach(float cur, float target, float dt, float rate) {
    float k = 1.0f - expf(-rate * dt);
    float v = cur + (target - cur) * k;
    return fabsf(v - target) < 0.001f ? target : v;
}
static inline float ease_out_cubic(float t) {
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    float u = 1 - t;
    return 1 - u * u * u;
}
static inline float ease_in_out(float t) {
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    return t < 0.5f ? 4 * t * t * t : 1 - powf(-2 * t + 2, 3) / 2;
}
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

#endif
