// MIT. Single-precision maths for the PS1 (no FPU, no libm in PSn00bSDK):
// ps1_libc.c. Accurate to about 1e-5, which is what the game logic needs.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif
float sqrtf(float x);
float sinf(float x);
float cosf(float x);
float tanf(float x);
float atanf(float x);
float atan2f(float y, float x);
float asinf(float x);
float acosf(float x);
float floorf(float x);
float ceilf(float x);
float fabsf(float x);
float fmodf(float x, float y);
float roundf(float x);
float expf(float x);
float logf(float x);
float powf(float x, float y);
#ifdef __cplusplus
}
#endif
#define M_PI 3.14159265358979323846
