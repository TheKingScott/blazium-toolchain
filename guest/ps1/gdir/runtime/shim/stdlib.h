// MIT. PSn00bSDK's stdlib plus atof / atoi (ps1_libc.c).
#pragma once
#include_next <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif
double atof(const char *s);
int atoi(const char *s);
#ifdef __cplusplus
}
#endif
