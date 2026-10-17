// MIT. Cooked data on disc lives under cdrom0:\DATA\ (the boot ROM's file
// search only reads the first sector of a directory, so the ISO root holds
// just SYSTEM.CNF and the ELF), each pack's files in DATA\Pnn\ (CDVDMAN's
// search has the same one-sector limit). iso_fopen tries DATA\Pnn, then
// DATA, then the path as given, so older discs still load.
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
		if (!strchr(name, '\\') && !strchr(name, '/') && strlen(name) + 24 < 160) {
			char buf[160];
			/* a numbered file (MESH04.BIN) sits in its pack's folder,
			 * DATA\P04: CDVDMAN only searches a directory's first sector,
			 * so the toolchain keeps each folder to one */
			const char *dot = strchr(name, '.');
			const size_t stem = dot ? (size_t)(dot - name) : strlen(name);
			if (stem >= 3 && name[stem - 2] >= '0' && name[stem - 2] <= '9' && name[stem - 1] >= '0' &&
					name[stem - 1] <= '9') {
				snprintf(buf, sizeof(buf), "cdrom0:\\DATA\\P%c%c\\%s", name[stem - 2], name[stem - 1], name);
				FILE *f = fopen(buf, mode);
				if (f) {
					return f;
				}
			}
			snprintf(buf, sizeof(buf), "cdrom0:\\DATA\\%s", name);
			FILE *f = fopen(buf, mode);
			if (f) {
				return f;
			}
		}
	}
	return fopen(path, mode);
}
