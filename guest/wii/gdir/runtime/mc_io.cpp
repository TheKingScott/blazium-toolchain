// MIT. Saves for PS2.save_slot / load_slot / memcard_present on the Wii:
// BLAZIUM.SAV in the game's folder on the SD card (or USB drive), as the
// PS3 keeps BLAZIUM.SAV in USRDIR. It holds four slots of MC_BYTES:
// "BLZS", the byte count (u16) and the bytes; a save writes the whole
// file. With the game's folder found (data_io) there is a "memory card".

#include "mc_io.h"

#include "data_io.h"

#include <stdio.h>
#include <string.h>

#define MC_SLOTS 4
#define MC_BYTES 512

static int s_ok;
static unsigned char s_file[MC_SLOTS * MC_BYTES];

static void path(char *p, size_t n)
{
	snprintf(p, n, "%s/BLAZIUM.SAV", data_dir());
}

static void read_file(void)
{
	memset(s_file, 0, sizeof(s_file));
	char p[300];
	path(p, sizeof(p));
	FILE *f = fopen(p, "rb");
	if (f) {
		if (fread(s_file, 1, sizeof(s_file), f) == 0) {
			memset(s_file, 0, sizeof(s_file));
		}
		fclose(f);
	}
}

int mc_io_boot(void)
{
	s_ok = data_dir()[0] != 0;
	printf("Wii save: %s\n", s_ok ? "BLAZIUM.SAV in the game's folder" : "no game folder, no saves");
	return s_ok;
}

int mc_io_present(void)
{
	return s_ok;
}

int mc_io_save(int slot, const unsigned char *data, unsigned n)
{
	if (!s_ok || n > MC_BYTES - 8 || slot < 0 || slot >= MC_SLOTS) {
		return 0;
	}
	read_file();
	unsigned char *s = s_file + slot * MC_BYTES;
	memset(s, 0, MC_BYTES);
	memcpy(s, "BLZS", 4);
	s[4] = (unsigned char)n;
	s[5] = (unsigned char)(n >> 8);
	memcpy(s + 8, data, n);
	char p[300];
	path(p, sizeof(p));
	FILE *f = fopen(p, "wb");
	int ok = 0;
	if (f) {
		ok = fwrite(s_file, 1, sizeof(s_file), f) == sizeof(s_file);
		ok = (fclose(f) == 0) && ok;
	}
	printf("Wii save: slot %d, %u bytes, %s\n", slot, n, ok ? "done" : "failed");
	return ok;
}

int mc_io_load(int slot, unsigned char *data, unsigned maxn)
{
	memset(data, 0, maxn);
	if (!s_ok) {
		return -2;
	}
	if (slot < 0 || slot >= MC_SLOTS) {
		return -1;
	}
	read_file();
	const unsigned char *s = s_file + slot * MC_BYTES;
	if (memcmp(s, "BLZS", 4) != 0) {
		return -1;
	}
	unsigned n = (unsigned)s[4] | ((unsigned)s[5] << 8);
	if (n > MC_BYTES - 8) {
		return -1;
	}
	if (n > maxn) {
		n = maxn;
	}
	memcpy(data, s + 8, n);
	printf("Wii save: slot %d loaded, %u bytes\n", slot, n);
	return (int)n;
}
