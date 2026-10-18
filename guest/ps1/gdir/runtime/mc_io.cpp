// MIT. Memory card saves for PS2.save_slot / load_slot / memcard_present on
// the PS1, through the BIOS card driver (the pads' serial port, shared).
//
// Each save slot is one file of one block (8 KiB) on the card in slot 1,
// bu00:<name><slot>. Its first 256 bytes are the PS1 save header the BIOS
// card browser reads (frame 0: "SC", one icon frame, one block, the title in
// Shift-JIS, the icon's palette; frame 1: the 16x16 icon), then "BLZS", the
// byte count and the bytes. The name and that header come from MCPS1.BIN,
// which `ps1 iso` makes from the export's MCSAVE.BIN (the PS2 save's name,
// title and icon); without it a save still works, under a generic name and
// title with a blank icon.

#include "mc_io.h"

#include "iso_path.h"

#include <psxapi.h>
#include <psxetc.h>
#include <psxgpu.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/fcntl.h>

#define MC_HEADER 256                 /* the save header: title frame and one icon frame */
#define MC_BLOCK 8192                 /* a card block: the file's size */
#define MC_DATA (MC_BLOCK - MC_HEADER) /* "BLZS", the count, the bytes */
#define MC_WAIT 180                   /* fields to wait for the card (3 s) */

static int s_boot;
static int s_ev_sw[4], s_ev_hw[4]; /* IOE, ERROR, TIMOUT, NEW */
static char s_name[24] = "BABLAZ-00001SAVE";
static uint8_t s_header[MC_HEADER];
static uint8_t s_buf[MC_BLOCK];

/* the header with no MCPS1.BIN: title "Blazium game" (full-width), blank icon */
static void default_header(void)
{
	static const char title[] = "Blazium game";
	memset(s_header, 0, sizeof(s_header));
	s_header[0] = 'S';
	s_header[1] = 'C';
	s_header[2] = 0x11; /* one icon frame */
	s_header[3] = 1;    /* one block */
	for (int i = 0; title[i] && i < 32; i++) {
		const char ch = title[i];
		unsigned sj = 0x8140; /* full-width space */
		if (ch >= 'A' && ch <= 'Z') {
			sj = 0x8260 + (unsigned)(ch - 'A');
		} else if (ch >= 'a' && ch <= 'z') {
			sj = 0x8281 + (unsigned)(ch - 'a');
		}
		s_header[4 + i * 2] = (uint8_t)(sj >> 8);
		s_header[5 + i * 2] = (uint8_t)sj;
	}
}

/* MCPS1.BIN: "MCP1", u16 1, u16 0, name[20] (the file name but the slot
 * digit), header[256] */
static void read_mcps1(void)
{
	default_header();
	FILE *f = iso_fopen("cdrom0:\\MCPS1.BIN;1", "rb");
	if (!f) {
		return;
	}
	uint8_t b[8 + 20 + MC_HEADER];
	const size_t n = fread(b, 1, sizeof(b), f);
	fclose(f);
	if (n == sizeof(b) && memcmp(b, "MCP1", 4) == 0 && b[28] == 'S' && b[29] == 'C') {
		memcpy(s_name, b + 8, 20);
		s_name[19] = 0; /* at most 19 + the slot digit */
		memcpy(s_header, b + 28, MC_HEADER);
	}
}

static void drain(void)
{
	for (int i = 0; i < 4; i++) {
		TestEvent(s_ev_sw[i]);
		TestEvent(s_ev_hw[i]);
	}
}

/* which of the four events fired (0 done, 1 error, 2 no card, 3 new card) */
static int wait_events(const int *ev)
{
	const int t0 = VSync(-1);
	while (VSync(-1) - t0 < MC_WAIT) {
		for (int i = 0; i < 4; i++) {
			if (TestEvent(ev[i])) {
				return i;
			}
		}
	}
	return 2;
}

int mc_io_boot(void)
{
	/* pad_io_init's InitPAD/StartPAD come first: InitCARD(1) keeps the pads */
	FastEnterCriticalSection();
	const uint32_t spec[4] = { EvSpIOE, EvSpERROR, EvSpTIMOUT, EvSpNEW };
	for (int i = 0; i < 4; i++) {
		s_ev_sw[i] = OpenEvent(SwCARD, spec[i], EvMdNOINTR, NULL);
		s_ev_hw[i] = OpenEvent(HwCARD, spec[i], EvMdNOINTR, NULL);
		EnableEvent(s_ev_sw[i]);
		EnableEvent(s_ev_hw[i]);
	}
	FastExitCriticalSection();
	InitCARD(1);
	StartCARD();
	_bu_init();
	ChangeClearPAD(0);
	read_mcps1();
	s_boot = 1;
	printf("P1 card: driver up, saves as bu00:%s<n>\n", s_name);
	return 1;
}

/* the card in slot 1, ready for file calls: 1, or 0 (none, or unusable) */
static int card_ready(void)
{
	if (!s_boot) {
		return 0;
	}
	drain();
	_card_info(0x00);
	int r = wait_events(s_ev_sw);
	if (r == 3) {
		/* a card not seen before (or swapped): acknowledge it, then read
		 * its directory so the file calls see it */
		drain();
		_card_clear(0x00);
		wait_events(s_ev_hw);
		r = 0;
	}
	if (r != 0) {
		return 0;
	}
	drain();
	_card_load(0x00);
	return wait_events(s_ev_sw) == 0;
}

int mc_io_present(void)
{
	/* the TTY hears when the card comes or goes, not every check */
	static int s_last = -1;
	const int ok = card_ready();
	if (ok != s_last) {
		printf(ok ? "P1 card: a card in slot 1\n" : "P1 card: no usable card in slot 1\n");
		s_last = ok;
	}
	return ok;
}

static void path_for(int slot, char *out)
{
	snprintf(out, 32, "bu00:%s%d", s_name, slot & 7);
}

int mc_io_save(int slot, const unsigned char *data, unsigned n)
{
	if (n > MC_DATA - 8 || !card_ready()) {
		return 0;
	}
	char path[32];
	path_for(slot, path);
	int fd = open(path, FWRITE);
	if (fd < 0) {
		/* a new file of one block */
		fd = open(path, FCREATE | FNBLOCKS(1));
		if (fd >= 0) {
			close(fd);
		}
		fd = open(path, FWRITE);
	}
	if (fd < 0) {
		printf("P1 card: %s could not be made (the card full?)\n", path);
		return 0;
	}
	/* the header, "BLZS", the count and the bytes, in whole 128-byte
	 * sectors (the BIOS writes no less) */
	const unsigned used = (MC_HEADER + 8 + n + 127) & ~127u;
	memset(s_buf, 0, used);
	memcpy(s_buf, s_header, MC_HEADER);
	memcpy(s_buf + MC_HEADER, "BLZS", 4);
	s_buf[MC_HEADER + 4] = (uint8_t)n;
	s_buf[MC_HEADER + 5] = (uint8_t)(n >> 8);
	s_buf[MC_HEADER + 6] = (uint8_t)(n >> 16);
	s_buf[MC_HEADER + 7] = (uint8_t)(n >> 24);
	memcpy(s_buf + MC_HEADER + 8, data, n);
	const int w = write(fd, s_buf, (int)used);
	close(fd);
	if (w != (int)used) {
		printf("P1 card: writing %s failed (%d of %u)\n", path, w, used);
		return 0;
	}
	return 1;
}

int mc_io_load(int slot, unsigned char *data, unsigned maxn)
{
	memset(data, 0, maxn);
	if (!card_ready()) {
		return -2;
	}
	char path[32];
	path_for(slot, path);
	const int fd = open(path, FREAD);
	if (fd < 0) {
		return -1;
	}
	/* the header and the first sector of data, then the rest as needed */
	int r = read(fd, s_buf, MC_HEADER + 128);
	if (r != MC_HEADER + 128 || memcmp(s_buf + MC_HEADER, "BLZS", 4) != 0) {
		close(fd);
		return -1;
	}
	unsigned n = (unsigned)s_buf[MC_HEADER + 4] | ((unsigned)s_buf[MC_HEADER + 5] << 8) |
			((unsigned)s_buf[MC_HEADER + 6] << 16) | ((unsigned)s_buf[MC_HEADER + 7] << 24);
	if (n > MC_DATA - 8) {
		close(fd);
		return -1;
	}
	const unsigned used = (MC_HEADER + 8 + n + 127) & ~127u;
	if (used > MC_HEADER + 128) {
		const int more = (int)(used - (MC_HEADER + 128));
		r = read(fd, s_buf + MC_HEADER + 128, more);
		if (r != more) {
			close(fd);
			return -1;
		}
	}
	close(fd);
	if (n > maxn) {
		n = maxn;
	}
	memcpy(data, s_buf + MC_HEADER + 8, n);
	return (int)n;
}
