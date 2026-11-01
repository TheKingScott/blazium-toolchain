// MIT. The game's folder on the Memory Stick (data_io.cpp).
#pragma once

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// Find the game's folder (PSP/GAME/NAME, with DATA/MESH00.BIN): from
// argv[0] (the EBOOT.PBP's path), else the folder name psp build wrote into
// the EBOOT, else the first PSP/GAME folder with the game's data. 1 when
// found.
int data_init(int argc, char **argv);
// The game's folder ("ms0:/PSP/GAME/PENGUIN"), or "" before data_init finds it.
const char *data_dir(void);
// A file of the game's DATA folder ("MESH00.BIN") for reading, or NULL.
FILE *data_fopen(const char *name);

#ifdef __cplusplus
}
#endif
