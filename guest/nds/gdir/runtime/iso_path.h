// MIT. The DS guest's iso_fopen: the shared runtime asks for disc paths
// (cdrom0:\NAME;1, as the PS2 and PS1 guests do); on the DS the cooked files
// are in the ROM's NitroFS, all in DATA/ under upper-case names (`nds build
// --sample gdir` stages them). DATA/ first, then the root.
#pragma once

#include <stdio.h>
#include <string.h>

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
	char buf[96];
	snprintf(buf, sizeof(buf), "nitro:/DATA/%s", base);
	FILE *f = fopen(buf, mode);
	if (f) {
		return f;
	}
	snprintf(buf, sizeof(buf), "nitro:/%s", base);
	return fopen(buf, mode);
}
