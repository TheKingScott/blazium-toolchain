// MIT. Saves for PS2.save_slot / load_slot / memcard_present on the PS3:
// one file, BLAZIUM.SAV, in the game's own folder on the hard drive
// (USRDIR, beside EBOOT.BIN), made by the first save. It holds four slots of
// MC_BYTES: "BLZS", the byte count (u16) and the bytes. A save writes the
// whole file at once. The hard drive is always there, so the "memory card"
// always is.

#include "mc_io.h"

#include "iso_path.h"

#include <stdio.h>
#include <string.h>

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

int mc_io_boot(void)
{
	snprintf(s_path, sizeof(s_path), "%sBLAZIUM.SAV", ps3_usrdir);
	s_ok = ps3_usrdir[0] != 0;
	printf("PS3 save: %s\n", s_ok ? s_path : "no game folder");
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
	FILE *f = fopen(s_path, "wb");
	if (!f) {
		printf("PS3 save: cannot write %s\n", s_path);
		return 0;
	}
	int ok = fwrite(s_file, 1, sizeof(s_file), f) == sizeof(s_file);
	ok = fclose(f) == 0 && ok;
	printf("PS3 save: slot %d, %u bytes, %s\n", slot, n, ok ? "done" : "failed");
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
