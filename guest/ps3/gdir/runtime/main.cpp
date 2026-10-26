// Blazium PS3 GDIR guest - MIT. The PS2 guest's scene runtime (GDIR VM,
// sys_io, pack_io) on the PlayStation 3: the cooked packs are files in the
// game's folder (USRDIR/DATA), pack 0 loads at boot, the scripts run every
// frame and rsx_draw.cpp draws with the RSX at the TV's resolution. The XMB's
// "Quit Game" ends it.

#include "gdir_vm.h"
#include "gs_draw.h"
#include "iso_path.h"
#include "pack_io.h"
#include "pad_io.h"
#include "sfx_io.h"
#include "sys_io.h"

#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h>
#include <sys/systime.h>
#include <sysutil/sysutil.h>

/* the main thread's priority and its stack (the VM recurses) */
SYS_PROCESS_PARAM(1001, 0x100000);

#ifndef PS3_TITLE_ID
#define PS3_TITLE_ID "BLAZ00001"
#endif

extern "C" {
char ps3_usrdir[256];
}

static volatile int s_quit;

static unsigned ru16(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static uint32_t ru32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t us_now(void)
{
	return sysGetSystemTime();
}

/* A whole file from the game's DATA folder. */
static unsigned char *load_file(const char *name, unsigned *size)
{
	char path[64];
	snprintf(path, sizeof(path), "cdrom0:\\%s;1", name);
	FILE *f = iso_fopen(path, "rb");
	*size = 0;
	if (!f) {
		printf("PS3: %s not in the game's folder\n", name);
		return 0;
	}
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *b = (unsigned char *)memalign(64, (size_t)n + 4);
	if (b && fread(b, 1, (size_t)n, f) == (size_t)n) {
		*size = (unsigned)n;
	} else {
		printf("PS3: %s (%ld bytes) did not load\n", name, n);
		free(b);
		b = 0;
	}
	fclose(f);
	return b;
}

/* USRDIR from argv[0] (".../USRDIR/EBOOT.BIN"), else the installed game's */
static void find_usrdir(int argc, char **argv)
{
	ps3_usrdir[0] = 0;
	if (argc > 0 && argv && argv[0] && argv[0][0] == '/') {
		snprintf(ps3_usrdir, sizeof(ps3_usrdir), "%s", argv[0]);
		char *slash = strrchr(ps3_usrdir, '/');
		if (slash) {
			slash[1] = 0;
			return;
		}
	}
	snprintf(ps3_usrdir, sizeof(ps3_usrdir), "/dev_hdd0/game/%s/USRDIR/", PS3_TITLE_ID);
}

/* the HUD's space: DISP.BIN as the PS2 export wrote it */
static void load_disp(int *w, int *h)
{
	*w = 640;
	*h = 448;
	unsigned n = 0;
	unsigned char *b = load_file("DISP.BIN", &n);
	if (b && n >= 12 && memcmp(b, "DISP", 4) == 0 && ru16(b + 4) == 1) {
		const int dw = (int)ru16(b + 8), dh = (int)ru16(b + 10);
		if (dw >= 256 && dw <= 1024) {
			*w = dw;
		}
		if (dh >= 192 && dh <= 1024) {
			*h = dh;
		}
	}
	free(b);
}

static void on_sysutil(u64 status, u64 param, void *user)
{
	(void)param;
	(void)user;
	if (status == SYSUTIL_EXIT_GAME) {
		s_quit = 1;
	}
}

static void halt(const char *why)
{
	printf("PS3: %s\n", why);
	while (!s_quit) {
		sysUtilCheckCallback();
		gs_draw_set_clear(96, 16, 16);
		gs_draw_hud_quad(0, 40, 200, 560, 40, 0, 0, 0);
		gs_draw_hud_style(0, 3, 2, 1, 0, 255, 255, 255);
		gs_draw_hud_text(0, 40, 200, 0, 0, 0, why);
		gs_draw_scene();
	}
	exit(0);
}

int main(int argc, char **argv)
{
	printf("Blazium PS3 GDIR guest\n");
	find_usrdir(argc, argv);
	printf("PS3: game folder %s\n", ps3_usrdir);
	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, on_sysutil, NULL);
	int hud_w = 640, hud_h = 448;
	load_disp(&hud_w, &hud_h);
	if (!gs_draw_boot(hud_w, hud_h)) {
		printf("PS3: the RSX did not start\n");
		return 1;
	}
	pad_io_init();

	unsigned mesh_sz = 0, gtex_sz = 0, node_sz = 0, scrp_sz = 0;
	unsigned char *scrp = load_file("SCRP00.BIN", &scrp_sz);
	unsigned char *mesh = load_file("MESH00.BIN", &mesh_sz);
	unsigned char *gtex = load_file("GTEX00.BIN", &gtex_sz);
	unsigned char *node = load_file("NODE00.BIN", &node_sz);
	if (!mesh || !gs_draw_init(mesh, mesh_sz, gtex, gtex_sz, node, node_sz)) {
		halt("No scene in the game's folder (USRDIR/DATA/MESH00.BIN).");
	}
	const uint64_t t0 = us_now();
	pack_io_set_pack0(mesh, mesh_sz, gtex, gtex_sz, node, node_sz);
	sfx_io_init();
	pack_io_init();
	sys_io_init();
	printf("PS3 boot: ready in %u ms\n", (unsigned)((us_now() - t0) / 1000));

	/* SCRP: "SCRP", u16 abi, u16 flags (bit 0: GDIR), u32 tape bytes, tape, GDIR */
	int gdir = 0;
	if (scrp && scrp_sz >= 12 && memcmp(scrp, "SCRP", 4) == 0 && (ru16(scrp + 6) & 1)) {
		const uint32_t tape = ru32(scrp + 8);
		if (12 + tape <= scrp_sz) {
			gdir = gdir_vm_init(scrp + 12 + tape, scrp_sz - 12 - tape);
		}
	}
	if (!gdir) {
		printf("PS3: no GDIR program (%s)\n", gdir_vm_error());
	}

	uint64_t last = us_now();
	while (!s_quit) {
		sysUtilCheckCallback();
		pad_io_poll(0, 0, 0);
		/* the frame's time from the clock, so a slow frame keeps time */
		const uint64_t now = us_now();
		float dt = (float)(now - last) * 1.0e-6f;
		last = now;
		if (dt < 1.0f / 60.0f) {
			dt = 1.0f / 60.0f;
		}
		if (dt > 0.1f) {
			dt = 0.1f;
		}
		if (gdir) {
#ifdef PS3_FIXED_DELTA
			gdir_vm_process(PS3_FIXED_DELTA);
#else
			gdir_vm_process(dt);
#endif
			const char *err = gdir_vm_error();
			if (err && err[0]) {
				halt(err);
			}
		}
		gs_draw_scene();
	}
	sfx_io_stop();
	sfx_io_music_stop();
	return 0;
}
