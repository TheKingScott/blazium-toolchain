// MIT. The game disc's files (disc_io.cpp).
#pragma once

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// Read the disc's header and file table. 1 when the disc is there.
int disc_init(void);
// A file of the disc ("DATA/MESH00.BIN", case aside) for reading, or NULL.
FILE *disc_fopen(const char *path);

#ifdef __cplusplus
}
#endif
