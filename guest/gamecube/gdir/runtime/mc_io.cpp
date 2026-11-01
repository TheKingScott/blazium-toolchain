// MIT. Saves for PS2.save_slot / load_slot / memcard_present on the
// GameCube: one file, "BLAZIUM" (the disc's game code), on the Memory Card
// in slot A, made by the
// first save (one 8 KB block). It holds four slots of MC_BYTES: "BLZS", the
// byte count (u16) and the bytes. A save writes the whole file. Without a
// card in slot A there is no memory card, as on a PS2 without one.

#include "mc_io.h"

#include <gccore.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MC_SLOTS 4
#define MC_BYTES 512
#define MC_FILE "BLAZIUM"


static int s_ok;
static u8 *s_work; /* the card driver's work area */
static u8 *s_file; /* the file, a whole sector */
static u32 s_sector = 8192;

static int mount(void)
{
	/* libogc counts a card as there only 300 ms after it first sees it
	 * (CARD_ERROR_BUSY until then), and the game starts sooner than that
	 * after the boot: wait that out (a second at most) */
	for (int i = 0; i < 100 && CARD_ProbeEx(CARD_SLOTA, NULL, NULL) == CARD_ERROR_BUSY; i++) {
		usleep(10000);
	}
	s32 r = CARD_Mount(CARD_SLOTA, s_work, NULL);
	if (r < 0 && r != CARD_ERROR_BROKEN) {
		return 0;
	}
	u32 ss = 0;
	if (CARD_GetSectorSize(CARD_SLOTA, &ss) >= 0 && ss >= MC_SLOTS * MC_BYTES) {
		s_sector = ss;
	}
	return 1;
}

int mc_io_boot(void)
{
	/* the save belongs to the disc's game code and maker (the disc ID the
	 * boot left at 0x80000000) */
	char game[5] = "GBZE", maker[3] = "01";
	const char *id = (const char *)0x80000000;
	int ok = 1;
	for (int i = 0; i < 6; i++) {
		if (id[i] < '0' || id[i] > 'Z') {
			ok = 0;
		}
	}
	if (ok) {
		memcpy(game, id, 4);
		memcpy(maker, id + 4, 2);
	}
	CARD_Init(game, maker);
	s_work = (u8 *)memalign(32, CARD_WORKAREA_SIZE);
	s_file = (u8 *)memalign(32, 32 * 1024);
	if (!s_work || !s_file) {
		return 0;
	}
	s_ok = mount();
	if (s_ok) {
		CARD_Unmount(CARD_SLOTA);
	}
	printf("GC save: %s\n", s_ok ? "Memory Card in slot A" : "no Memory Card in slot A");
	return s_ok;
}

int mc_io_present(void)
{
	return s_ok;
}

/* the file into s_file (zeroes when there is none): 1 if it is there */
static int read_file(card_file *f, int *have)
{
	memset(s_file, 0, s_sector);
	*have = 0;
	if (CARD_Open(CARD_SLOTA, MC_FILE, f) < 0) {
		return 1;
	}
	*have = 1;
	DCInvalidateRange(s_file, s_sector);
	const int ok = CARD_Read(f, s_file, s_sector, 0) >= 0;
	return ok;
}

int mc_io_save(int slot, const unsigned char *data, unsigned n)
{
	if (!s_ok || n > MC_BYTES - 8 || slot < 0 || slot >= MC_SLOTS || !mount()) {
		return 0;
	}
	card_file f;
	int have = 0;
	int ok = read_file(&f, &have);
	if (ok && !have) {
		ok = CARD_Create(CARD_SLOTA, MC_FILE, s_sector, &f) >= 0;
		memset(s_file, 0, s_sector);
	}
	if (ok) {
		unsigned char *s = s_file + slot * MC_BYTES;
		memset(s, 0, MC_BYTES);
		memcpy(s, "BLZS", 4);
		s[4] = (unsigned char)n;
		s[5] = (unsigned char)(n >> 8);
		memcpy(s + 8, data, n);
		DCFlushRange(s_file, s_sector);
		ok = CARD_Write(&f, s_file, s_sector, 0) >= 0;
		CARD_Close(&f);
	}
	CARD_Unmount(CARD_SLOTA);
	printf("GC save: slot %d, %u bytes, %s\n", slot, n, ok ? "done" : "failed");
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
	if (!mount()) {
		return -2;
	}
	card_file f;
	int have = 0;
	const int ok = read_file(&f, &have);
	if (have) {
		CARD_Close(&f);
	}
	CARD_Unmount(CARD_SLOTA);
	if (!ok || !have) {
		return -1;
	}
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
	printf("GC save: slot %d loaded, %u bytes\n", slot, n);
	return (int)n;
}
