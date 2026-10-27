// MIT. The game's folder on the SD card or a USB drive (data_io.cpp).
#pragma once

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// Mount the SD card and USB drives (libfat) and find the game's folder
// (apps/NAME, with DATA/MESH00.BIN): from argv[0] as the Homebrew Channel
// passes it, else the folder name wii build wrote into the DOL, else the
// first apps/ folder with the game's data. 1 when found.
int data_init(int argc, char **argv);
// The game's folder ("sd:/apps/penguin"), or "" before data_init finds it.
const char *data_dir(void);
// A file of the game's DATA folder ("MESH00.BIN") for reading, or NULL.
FILE *data_fopen(const char *name);

#ifdef __cplusplus
}
#endif
