// MIT. Saves for PS2.save_slot / load_slot / memcard_present on the DS: one
// file, BLAZIUM.SAV, on the SD card (a DSi's own, or a flashcart's through
// DLDI), at the card's root, made by the first save. It holds four slots of MC_BYTES: "BLZS", the byte count (u16)
// and the bytes. Without a card (an emulator with no SD image) there is no
// memory card, as on a PS2 without one.

#include "mc_io.h"

#include <fat.h>
#include <nds.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>

#define MC_SLOTS 4
#define MC_BYTES 512

static int s_ok;
static char s_path[64];
static unsigned char s_buf[MC_BYTES];

/* the save file opened for update, made (zeroed) when it is not there */
static FILE *open_save(void)
{
	FILE *f = fopen(s_path, "r+b");
	if (f) {
		return f;
	}
	f = fopen(s_path, "w+b");
	if (!f) {
		return 0;
	}
	memset(s_buf, 0, sizeof(s_buf));
	for (int i = 0; i < MC_SLOTS; i++) {
		fwrite(s_buf, 1, sizeof(s_buf), f);
	}
	fflush(f);
	return f;
}

int mc_io_boot(void)
{
	/* main has run fatInitDefault (NitroFS needs it on a DSi or a flashcart).
	 * Nothing is written here: the file is made by the first save. The
	 * DSi's own SD card first, then a flashcart's (DLDI). */
	static const char *const dirs[] = { "sd:/", "fat:/" };
	for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		DIR *d = opendir(dirs[i]);
		if (!d) {
			continue;
		}
		closedir(d);
		snprintf(s_path, sizeof(s_path), "%sBLAZIUM.SAV", dirs[i]);
		s_ok = 1;
		printf("DS save: %s\n", s_path);
		return 1;
	}
	printf("DS save: no SD card\n");
	return 0;
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
	FILE *f = open_save();
	if (!f) {
		return 0;
	}
	memset(s_buf, 0, sizeof(s_buf));
	memcpy(s_buf, "BLZS", 4);
	s_buf[4] = (unsigned char)n;
	s_buf[5] = (unsigned char)(n >> 8);
	memcpy(s_buf + 8, data, n);
	int ok = fseek(f, (long)slot * MC_BYTES, SEEK_SET) == 0 && fwrite(s_buf, 1, sizeof(s_buf), f) == sizeof(s_buf);
	ok = fclose(f) == 0 && ok;
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
	FILE *f = fopen(s_path, "rb");
	if (!f) {
		return -1;
	}
	const int ok = fseek(f, (long)slot * MC_BYTES, SEEK_SET) == 0 && fread(s_buf, 1, sizeof(s_buf), f) == sizeof(s_buf);
	fclose(f);
	if (!ok || memcmp(s_buf, "BLZS", 4) != 0) {
		return -1;
	}
	unsigned n = (unsigned)s_buf[4] | ((unsigned)s_buf[5] << 8);
	if (n > MC_BYTES - 8) {
		return -1;
	}
	if (n > maxn) {
		n = maxn;
	}
	memcpy(data, s_buf + 8, n);
	return (int)n;
}
