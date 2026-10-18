// MIT. PSn00bSDK's stdio plus what the shared Blazium runtime (gdir_vm,
// sys_io, pack_io) expects: FILE over the CD-ROM (ps1_libc.c) and a
// snprintf that prints floats.
#pragma once
#include_next <stdio.h>
#include <stddef.h>
#include <psxapi.h> /* SEEK_SET / SEEK_CUR / SEEK_END */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct P1File FILE;
#define EOF (-1)

FILE *p1_fopen(const char *path, const char *mode);
size_t p1_fread(void *buf, size_t size, size_t n, FILE *f);
size_t p1_fwrite(const void *buf, size_t size, size_t n, FILE *f);
int p1_fseek(FILE *f, long off, int whence);
long p1_ftell(FILE *f);
int p1_fclose(FILE *f);
void p1_rewind(FILE *f);
int p1_vsnprintf(char *o, size_t n, const char *fmt, va_list ap);
int p1_snprintf(char *o, size_t n, const char *fmt, ...);
int p1_sprintf(char *o, const char *fmt, ...);
int p1_printf(const char *fmt, ...);

#define fopen p1_fopen
#define fread p1_fread
#define fwrite p1_fwrite
#define fseek p1_fseek
#define ftell p1_ftell
#define fclose p1_fclose
#define rewind p1_rewind
#define vsnprintf p1_vsnprintf
#define snprintf p1_snprintf
#define sprintf p1_sprintf
#define printf p1_printf

#ifdef __cplusplus
}
#endif
