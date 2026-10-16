// MIT. Cooked data on disc lives under cdrom0:\DATA\ (the boot ROM's file
// search only reads the first sector of a directory, so the ISO root holds
// just SYSTEM.CNF and the ELF). iso_fopen tries DATA first, then the path
// as given, so older discs with everything in the root still load.
#pragma once

#include <stdio.h>
#include <string.h>

static inline FILE *iso_fopen(const char *path, const char *mode)
{
	if (path && strncmp(path, "cdrom0:", 7) == 0) {
		const char *name = path + 7;
		while (*name == '\\' || *name == '/') {
			name++;
		}
		if (!strchr(name, '\\') && !strchr(name, '/')) {
			char buf[160];
			if (strlen(name) + 16 < sizeof(buf)) {
				snprintf(buf, sizeof(buf), "cdrom0:\\DATA\\%s", name);
				FILE *f = fopen(buf, mode);
				if (f) {
					return f;
				}
			}
		}
	}
	return fopen(path, mode);
}
