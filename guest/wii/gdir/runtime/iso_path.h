// MIT. The Wii guest's iso_fopen: the shared runtime asks for disc paths
// (cdrom0:\NAME;1, as the PS2 guest does); on the Wii the cooked files are
// in the game's folder on the SD card, in DATA/ under upper-case names
// (`wii build --sample gdir` writes them there). Only reading: other modes
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
