// MIT. Saves for PS2.save_slot / load_slot / memcard_present on the Switch:
// one file, BLAZIUM.SAV, in the game's folder on the SD card
// (switch/NAME/, beside NAME.nro; main.cpp finds it), made by the first
// save. It holds four slots of MC_BYTES: "BLZS", the byte count (u16) and
// the bytes. A save writes the whole file at once. With the SD card there
// is a "memory card"; without one the game runs without saving.

#include "mc_io.h"

#include "iso_path.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define MC_SLOTS 4
#define MC_BYTES 512

static int s_ok;
static char s_path[300];
static unsigned char s_file[MC_SLOTS * MC_BYTES];

/* the save file as it is (zeroes where it has nothing) */
static void read_file(void)
{
	memset(s_file, 0, sizeof(s_file));
	FILE *f = fopen(s_path, "rb");
	if (f) {
		const size_t n = fread(s_file, 1, sizeof(s_file), f);
		(void)n;
		fclose(f);
	}
}

/* the save's folder and the ones above it (the first save makes them) */
static void make_dirs(void)
{
	char p[256];
	snprintf(p, sizeof(p), "%s", switch_save);
	for (char *q = p + 1; *q; q++) {
		if (*q == '/' && q[-1] != ':') {
			*q = 0;
			mkdir(p, 0777);
			*q = '/';
		}
	}
}

int mc_io_boot(void)
{
	snprintf(s_path, sizeof(s_path), "%sBLAZIUM.SAV", switch_save);
	s_ok = switch_save[0] != 0;
	printf("Switch save: %s\n", s_ok ? s_path : "no SD card");
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
	make_dirs();
	FILE *f = fopen(s_path, "wb");
	if (!f) {
		printf("Switch save: cannot write %s\n", s_path);
		return 0;
	}
	int ok = fwrite(s_file, 1, sizeof(s_file), f) == sizeof(s_file);
	ok = fclose(f) == 0 && ok;
	printf("Switch save: slot %d, %u bytes, %s\n", slot, n, ok ? "done" : "failed");
	return ok ? 1 : 0;
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
	return (int)n;
}
