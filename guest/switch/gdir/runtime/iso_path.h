// MIT. The Switch guest's iso_fopen: the shared runtime asks for disc paths
// (cdrom0:\NAME;1, as the PS2 guest does); on the Switch the cooked files
// are in the game's DATA folder, under upper-case names: the .nro's RomFS
// (romfs:/DATA, switch build packs them in), or a folder on the SD card
// (main.cpp finds it and sets switch_data).
#pragma once

#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif
/* the folder with the packs, with a trailing '/' (main.cpp sets it) */
extern char switch_data[256];
/* the folder BLAZIUM.SAV goes in, with a trailing '/'; empty: no SD card */
extern char switch_save[256];
#ifdef __cplusplus
}
#endif

static inline FILE *iso_fopen(const char *path, const char *mode)
{
	if (!path || !switch_data[0]) {
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
	snprintf(buf, sizeof(buf), "%s%s", switch_data, base);
	return fopen(buf, mode);
}
