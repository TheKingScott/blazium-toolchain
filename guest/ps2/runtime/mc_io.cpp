// MIT. Memory card saves (see mc_io.h).
//
// XMCMAN and XMCSERV come from the BIOS (rom0:), matching the rom0: XSIO2MAN
// and XPADMAN pad_io loads, and are only loaded the first time a script asks
// for the card. Files are written with libmc rather than fopen("mc0:..."): the
// newlib open flags are not the IOP's, and libmc reports why a call failed.

#include "mc_io.h"
#include "iso_path.h"

#include <kernel.h>
#include <libmc.h>
#include <loadfile.h>
#include <sjis.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// IOP open flags (io_common.h FIO_O_*), which libmc passes straight through
#define MC_O_RDONLY 0x0001
#define MC_O_WRONLY 0x0002
#define MC_O_CREAT 0x0200

// A save file: "BZSV", u32 length, then the bytes.
#define MC_SAVE_MAGIC "BZSV"

static int s_init; // 0 not tried, 1 ready, -1 the modules would not load
static int s_cfg_read;
static char s_dir[33] = "BADATA-BLAZIUM";
static char s_title[33] = "Blazium game";
static unsigned char *s_icon; // the browser icon (.ico), or 0
static unsigned s_icon_n;

static int mc_ready(void)
{
	if (s_init) {
		return s_init > 0;
	}
	s_init = -1;
	if (SifLoadModule("rom0:XMCMAN", 0, NULL) < 0) {
		printf("MC: rom0:XMCMAN did not load\n");
		return 0;
	}
	if (SifLoadModule("rom0:XMCSERV", 0, NULL) < 0) {
		printf("MC: rom0:XMCSERV did not load\n");
		return 0;
	}
	if (mcInit(MC_TYPE_XMC) < 0) {
		printf("MC: mcInit failed\n");
		return 0;
	}
	s_init = 1;
	return 1;
}

static unsigned rd32(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

// MCSAVE.BIN: "MCSV", u16 version (1), u16 header size, char dir[32],
// char title[32], u32 icon size, then the icon.
static void read_config(void)
{
	if (s_cfg_read) {
		return;
	}
	s_cfg_read = 1;
	FILE *f = fopen("host:MCSAVE.BIN", "rb");
	if (!f) {
		f = iso_fopen("cdrom0:\\MCSAVE.BIN;1", "rb");
	}
	if (!f) {
		printf("MC: no MCSAVE.BIN, saving as %s with no icon\n", s_dir);
		return;
	}
	unsigned char hdr[76];
	if (fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) && memcmp(hdr, "MCSV", 4) == 0) {
		memcpy(s_dir, hdr + 8, 32);
		s_dir[32] = 0;
		memcpy(s_title, hdr + 40, 32);
		s_title[32] = 0;
		const unsigned n = rd32(hdr + 72);
		if (n > 0 && n < 256 * 1024) {
			s_icon = (unsigned char *)malloc(n);
			if (s_icon && fread(s_icon, 1, n, f) == n) {
				s_icon_n = n;
			} else {
				free(s_icon);
				s_icon = 0;
			}
		}
	}
	fclose(f);
}

static int card_ok(void)
{
	if (!mc_ready()) {
		return 0;
	}
	int type = 0, free_kb = 0, format = 0, ret = 0;
	mcGetInfo(0, 0, &type, &free_kb, &format);
	mcSync(0, NULL, &ret);
	// 0: the same card as last time; -1: a card was swapped in
	return (ret == sceMcResSucceed || ret == sceMcResChangedCard) && type == MC_TYPE_PS2 && format;
}

static int write_file(const char *path, const void *a, unsigned an, const void *b, unsigned bn)
{
	int fd = 0;
	mcOpen(0, 0, path, MC_O_WRONLY | MC_O_CREAT);
	mcSync(0, NULL, &fd);
	if (fd < 0) {
		printf("MC: open %s failed (%d)\n", path, fd);
		return 0;
	}
	int ok = 1, ret = 0;
	if (a && an) {
		mcWrite(fd, a, (int)an);
		mcSync(0, NULL, &ret);
		ok = ok && ret == (int)an;
	}
	if (b && bn) {
		mcWrite(fd, b, (int)bn);
		mcSync(0, NULL, &ret);
		ok = ok && ret == (int)bn;
	}
	mcClose(fd);
	mcSync(0, NULL, &ret);
	return ok;
}

static int file_exists(const char *path)
{
	int fd = 0, ret = 0;
	mcOpen(0, 0, path, MC_O_RDONLY);
	mcSync(0, NULL, &fd);
	if (fd < 0) {
		return 0;
	}
	mcClose(fd);
	mcSync(0, NULL, &ret);
	return 1;
}

// icon.sys: what the BIOS browser shows for the directory (libmc.h mcIcon,
// laid out as ps2sdk's memory card sample does it).
static int write_icon_sys(void)
{
	static iconIVECTOR bg[4] = {
		{ 40, 30, 90, 0 }, { 40, 30, 90, 0 }, { 10, 10, 30, 0 }, { 10, 10, 30, 0 },
	};
	static iconFVECTOR light_dir[3] = {
		{ 0.5f, 0.5f, 0.5f, 0.0f }, { 0.0f, -0.4f, -0.1f, 0.0f }, { -0.5f, -0.5f, 0.5f, 0.0f },
	};
	static iconFVECTOR light_col[3] = {
		{ 0.3f, 0.3f, 0.3f, 0.0f }, { 0.4f, 0.4f, 0.4f, 0.0f }, { 0.5f, 0.5f, 0.5f, 0.0f },
	};
	static iconFVECTOR ambient = { 0.5f, 0.5f, 0.5f, 0.0f };
	static mcIcon sys __attribute__((aligned(64)));
	memset(&sys, 0, sizeof(sys));
	memcpy(sys.head, "PS2D", 4);
	strcpy_sjis((short *)sys.title, s_title);
	sys.nlOffset = 0;
	sys.trans = 0x60;
	memcpy(sys.bgCol, bg, sizeof(bg));
	memcpy(sys.lightDir, light_dir, sizeof(light_dir));
	memcpy(sys.lightCol, light_col, sizeof(light_col));
	memcpy(sys.lightAmbient, ambient, sizeof(ambient));
	strcpy((char *)sys.view, "icon.ico");
	strcpy((char *)sys.copy, "icon.ico");
	strcpy((char *)sys.del, "icon.ico");
	char path[64];
	snprintf(path, sizeof(path), "/%s/icon.sys", s_dir);
	return write_file(path, &sys, sizeof(sys), 0, 0);
}

int mc_io_present(void)
{
	return card_ok();
}

int mc_io_save(int slot, const unsigned char *data, unsigned n)
{
	if (slot < 0 || slot > 3) {
		slot = 0;
	}
	read_config();
	if (!card_ok()) {
		printf("MC: save: no formatted card in slot 1\n");
		return 0;
	}
	char path[64];
	snprintf(path, sizeof(path), "/%s", s_dir);
	int ret = 0;
	mcMkDir(0, 0, path);
	mcSync(0, NULL, &ret);
	if (ret < 0 && ret != sceMcResNoEntry) {
		// -4 is "already there"; anything else is a real failure
		printf("MC: mkdir %s failed (%d)\n", path, ret);
		return 0;
	}
	// the browser files: written with a new directory, or if they went missing
	snprintf(path, sizeof(path), "/%s/icon.sys", s_dir);
	if (ret == 0 || !file_exists(path)) {
		write_icon_sys();
		if (s_icon) {
			snprintf(path, sizeof(path), "/%s/icon.ico", s_dir);
			write_file(path, s_icon, s_icon_n, 0, 0);
		}
	}
	unsigned char hdr[8];
	memcpy(hdr, MC_SAVE_MAGIC, 4);
	hdr[4] = (unsigned char)(n & 0xff);
	hdr[5] = (unsigned char)((n >> 8) & 0xff);
	hdr[6] = (unsigned char)((n >> 16) & 0xff);
	hdr[7] = (unsigned char)((n >> 24) & 0xff);
	snprintf(path, sizeof(path), "/%s/SAVE%d.DAT", s_dir, slot);
	const int ok = write_file(path, hdr, sizeof(hdr), data, n);
	printf("MC: saved %u bytes to mc0:%s (%s)\n", n, path, ok ? "ok" : "failed");
	return ok;
}

int mc_io_load(int slot, unsigned char *data, unsigned maxn)
{
	if (slot < 0 || slot > 3) {
		slot = 0;
	}
	read_config();
	if (!card_ok()) {
		return -2;
	}
	char path[64];
	snprintf(path, sizeof(path), "/%s/SAVE%d.DAT", s_dir, slot);
	int fd = 0, ret = 0;
	mcOpen(0, 0, path, MC_O_RDONLY);
	mcSync(0, NULL, &fd);
	if (fd < 0) {
		return -1;
	}
	static unsigned char hdr[8] __attribute__((aligned(64)));
	mcRead(fd, hdr, sizeof(hdr));
	mcSync(0, NULL, &ret);
	int got = -1;
	if (ret == (int)sizeof(hdr) && memcmp(hdr, MC_SAVE_MAGIC, 4) == 0) {
		unsigned n = rd32(hdr + 4);
		if (n > maxn) {
			n = maxn;
		}
		memset(data, 0, maxn);
		mcRead(fd, data, (int)n);
		mcSync(0, NULL, &ret);
		got = ret < 0 ? -1 : ret;
	}
	mcClose(fd);
	mcSync(0, NULL, &ret);
	printf("MC: loaded %d bytes from mc0:%s\n", got, path);
	return got;
}
