// MIT. The PSP guest's iso_fopen: the shared runtime asks for disc paths
// (cdrom0:\NAME;1, as the PS2 guest does); on the PSP the cooked files are
// in the game's folder on the Memory Stick, in DATA/ under upper-case names
// (`psp build --sample gdir` writes them there). Only reading: other modes
// (the PS2's host: saves) get NULL.
#pragma once

#include "data_io.h"

#include <stdio.h>
#include <string.h>

static inline FILE *iso_fopen(const char *path, const char *mode)
{
	if (!path || !mode || mode[0] != 'r') {
		return 0;
	}
	/* the bare name: after the device and any folders, without ";1" */
	const char *name = path;
	for (const char *p = path; *p; p++) {
		if (*p == ':' || *p == '\\' || *p == '/') {
			name = p + 1;
		}
	}
	char base[64];
	size_t n = 0;
	while (name[n] && name[n] != ';' && n + 1 < sizeof(base)) {
		const char c = name[n];
		base[n] = (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
		n++;
	}
	base[n] = 0;
	return n ? data_fopen(base) : 0;
}

/* The shared runtime also tries the PS2's own devices ("host:SAVE0.BIN",
 * "mc0:SAVE.BIN": the user:// stub). A PSP has neither, but PPSSPP makes
 * such a name a file in the game's folder; so here they are no file at
 * all, as on a PSP. */
static inline FILE *psp_fopen(const char *path, const char *mode)
{
	if (path && (strncmp(path, "host:", 5) == 0 || strncmp(path, "mc0:", 4) == 0 || strncmp(path, "mc1:", 4) == 0)) {
		return 0;
	}
	return fopen(path, mode);
}
#define fopen psp_fopen
