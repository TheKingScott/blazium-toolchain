// Blazium Wii GDIR guest - MIT. The PS2 guest's scene runtime (GDIR VM,
// sys_io, pack_io) on the Nintendo Wii, as a Homebrew Channel app: the
// cooked packs are files in the app's folder on the SD card (DATA/), pack 0
// loads at boot, the scripts run every frame and gx_draw.cpp draws with the
// GX. HOME (or the console's RESET) goes back to the loader, POWER turns
// the Wii off.

#include "data_io.h"
#include "gdir_vm.h"
#include "gs_draw.h"
#include "iso_path.h"
#include "pack_io.h"
#include "pad_io.h"
#include "sfx_io.h"
#include "sys_io.h"
#include "wii_pad.h"

#include <gccore.h>
#include <malloc.h>
#include <ogc/lwp_watchdog.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
	return ticks_to_microsecs(gettime());
}

/* A whole file from the game's DATA folder. */
static unsigned char *load_file(const char *name, unsigned *size)
{
	char path[64];
	snprintf(path, sizeof(path), "cdrom0:\\%s;1", name);
	FILE *f = iso_fopen(path, "rb");
	*size = 0;
	if (!f) {
		printf("Wii: %s not in the game's DATA folder\n", name);
		return 0;
	}
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *b = (unsigned char *)memalign(32, (size_t)n + 4);
	if (b && fread(b, 1, (size_t)n, f) == (size_t)n) {
		*size = (unsigned)n;
	} else {
		printf("Wii: %s (%ld bytes) did not load\n", name, n);
		free(b);
		b = 0;
	}
	fclose(f);
	return b;
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

static void halt(const char *why)
{
	printf("Wii: %s\n", why);
	for (;;) {
		gs_draw_set_clear(96, 16, 16);
		gs_draw_hud_quad(0, 40, 200, 560, 40, 0, 0, 0);
		gs_draw_hud_style(0, 3, 2, 1, 0, 255, 255, 255);
		gs_draw_hud_text(0, 40, 200, 0, 0, 0, why);
		gs_draw_scene();
		pad_io_poll(0, 0, 0);
		if (pad_io_quit()) {
			wii_pad_leave();
		}
	}
}

/* the heap, when pack_io finds no room for a pack */
extern "C" void wii_heap_report(void)
{
	const struct mallinfo mi = mallinfo();
	printf("Wii heap: %d bytes in use, %d free of the arena, %u in MEM1 left\n", (int)mi.uordblks, (int)mi.fordblks,
			(unsigned)((u8 *)SYS_GetArena1Hi() - (u8 *)SYS_GetArena1Lo()));
	printf("Wii heap: %u in MEM2 left\n", (unsigned)((u8 *)SYS_GetArena2Hi() - (u8 *)SYS_GetArena2Lo()));
}

int main(int argc, char **argv)
{
	/* printf to the debug UART: Dolphin's log (OSReport), nothing on a retail
	 * console */
	SYS_STDIO_Report(true);
	printf("Blazium Wii GDIR guest\n");
	const int disc = data_init(argc, argv);
	int hud_w = 640, hud_h = 448;
	if (disc) {
		load_disp(&hud_w, &hud_h);
	}
	gs_draw_boot(hud_w, hud_h);
	if (!disc) {
		halt("The game's folder is not on the SD card (apps/NAME/DATA).");
	}
	pad_io_init();

	unsigned mesh_sz = 0, gtex_sz = 0, node_sz = 0, scrp_sz = 0;
	unsigned char *scrp = load_file("SCRP00.BIN", &scrp_sz);
	unsigned char *mesh = load_file("MESH00.BIN", &mesh_sz);
	unsigned char *gtex = load_file("GTEX00.BIN", &gtex_sz);
	unsigned char *node = load_file("NODE00.BIN", &node_sz);
	if (!mesh || !gs_draw_init(mesh, mesh_sz, gtex, gtex_sz, node, node_sz)) {
		halt("No scene in the game's folder (DATA/MESH00.BIN).");
	}
	const uint64_t t0 = us_now();
	pack_io_set_pack0(mesh, mesh_sz, gtex, gtex_sz, node, node_sz);
	sfx_io_init();
	pack_io_init();
	sys_io_init();
	printf("Wii boot: ready in %u ms\n", (unsigned)((us_now() - t0) / 1000));
	wii_heap_report();

	/* SCRP: "SCRP", u16 abi, u16 flags (bit 0: GDIR), u32 tape bytes, tape, GDIR */
	int gdir = 0;
	if (scrp && scrp_sz >= 12 && memcmp(scrp, "SCRP", 4) == 0 && (ru16(scrp + 6) & 1)) {
		const uint32_t tape = ru32(scrp + 8);
		if (12 + tape <= scrp_sz) {
			gdir = gdir_vm_init(scrp + 12 + tape, scrp_sz - 12 - tape);
		}
	}
	if (!gdir) {
		printf("Wii: no GDIR program (%s)\n", gdir_vm_error());
	}

	uint64_t last = us_now();
	for (;;) {
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
#ifdef WII_FIXED_DELTA
			gdir_vm_process(WII_FIXED_DELTA);
#else
			gdir_vm_process(dt);
#endif
			const char *err = gdir_vm_error();
			if (err && err[0]) {
				halt(err);
			}
		}
		if (pad_io_quit()) {
			/* back to the Homebrew Channel (or the loader that started us),
			 * or off */
			wii_pad_leave();
		}
		gs_draw_scene();
#ifdef WII_FRAME_STATS
		/* dev: the frame rate the console would see (the emulated clock) */
		{
			static unsigned frames;
			static uint64_t t_stat;
			if (!t_stat) {
				t_stat = now;
			}
			if (++frames == 300) {
				const uint64_t us = us_now() - t_stat;
				printf("Wii frames: %u.%u fps over %u frames\n", (unsigned)(300000000ull / us), (unsigned)(3000000000ull / us % 10), frames);
				frames = 0;
				t_stat = us_now();
			}
		}
#endif
	}
	return 0;
}
