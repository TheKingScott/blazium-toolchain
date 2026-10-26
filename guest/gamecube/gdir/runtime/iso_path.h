// MIT. The GameCube guest's iso_fopen: the shared runtime asks for disc
// paths (cdrom0:\NAME;1, as the PS2 guest does); on the GameCube the
// cooked files are on the game's disc, in DATA/ under upper-case names
// (`gamecube build --sample gdir` writes them there). DATA/ first, then the
// disc's root. Only reading: other modes (the PS2's host: saves) get NULL.
#pragma once

#include "disc_io.h"

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
	if (!n) {
		return 0;
	}
	char buf[96];
	snprintf(buf, sizeof(buf), "DATA/%s", base);
	FILE *f = disc_fopen(buf);
	return f ? f : disc_fopen(base);
}
