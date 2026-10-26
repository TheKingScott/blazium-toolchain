// MIT. The PS3 guest's iso_fopen: the shared runtime asks for disc paths
// (cdrom0:\NAME;1, as the PS2 guest does); on the PS3 the cooked files are
// in the game's folder, USRDIR/DATA/ beside EBOOT.BIN, under upper-case
// names (`ps3 build --sample gdir` stages them). DATA/ first, then USRDIR.
#pragma once

#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif
/* USRDIR, with a trailing '/' (main.cpp sets it from argv[0]) */
extern char ps3_usrdir[256];
#ifdef __cplusplus
}
#endif

static inline FILE *iso_fopen(const char *path, const char *mode)
{
	if (!path) {
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
	char buf[352];
	snprintf(buf, sizeof(buf), "%sDATA/%s", ps3_usrdir, base);
	FILE *f = fopen(buf, mode);
	if (f) {
		return f;
	}
	snprintf(buf, sizeof(buf), "%s%s", ps3_usrdir, base);
	return fopen(buf, mode);
}
