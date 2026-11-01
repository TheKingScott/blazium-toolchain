// Blazium PSP GDIR guest - MIT. The PS2 guest's scene runtime (GDIR VM,
// sys_io, pack_io) on the PlayStation Portable: the cooked packs are files
// in the game's folder on the Memory Stick (PSP/GAME/NAME/DATA), pack 0
// loads at boot, the scripts run every frame and gu_draw.cpp draws with the
// GE at 480x272. The HOME menu's "Exit Game" ends it.

#include "data_io.h"
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
#include <pspkernel.h>
#include <psppower.h>
#include <psprtc.h>

/* a user-mode PRX: the main thread with the VFPU, its stack (the VM
 * recurses), and all the memory the PSP model has left for the heap */
PSP_MODULE_INFO("Blazium GDIR", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(1024);
PSP_HEAP_SIZE_KB(-1024);

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
	return (uint64_t)sceKernelGetSystemTimeWide();
}

/* A whole file from the game's DATA folder. */
static unsigned char *load_file(const char *name, unsigned *size)
{
	char path[64];
	snprintf(path, sizeof(path), "cdrom0:\\%s;1", name);
	FILE *f = iso_fopen(path, "rb");
	*size = 0;
	if (!f) {
		printf("PSP: %s not in the game's DATA folder\n", name);
		return 0;
	}
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *b = (unsigned char *)memalign(64, (size_t)n + 4);
	if (b && fread(b, 1, (size_t)n, f) == (size_t)n) {
		*size = (unsigned)n;
	} else {
		printf("PSP: %s (%ld bytes) did not load\n", name, n);
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

/* the heap, when pack_io finds no room for a pack */
extern "C" void psp_heap_report(void)
{
	const struct mallinfo mi = mallinfo();
	printf("PSP heap: %d bytes in use, %d free of the arena, %u free in the system\n", (int)mi.uordblks, (int)mi.fordblks,
			(unsigned)sceKernelTotalFreeMemSize());
}

/* HOME > Exit Game: the exit callback, on its own thread */
static int on_exit(int arg1, int arg2, void *common)
{
	(void)arg1;
	(void)arg2;
	(void)common;
	s_quit = 1;
	return 0;
}

static int callback_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	const int cb = sceKernelCreateCallback("blazium_exit", on_exit, NULL);
	sceKernelRegisterExitCallback(cb);
	sceKernelSleepThreadCB();
	return 0;
}

static void callbacks_start(void)
{
	const int th = sceKernelCreateThread("blazium_callbacks", callback_thread, 0x11, 0xFA0, PSP_THREAD_ATTR_USER, 0);
	if (th >= 0) {
		sceKernelStartThread(th, 0, 0);
	}
}

static void leave(void)
{
	sfx_io_stop();
	sfx_io_music_stop();
	sceKernelExitGame();
}

static void halt(const char *why)
{
	printf("PSP: %s\n", why);
	while (!s_quit) {
		gs_draw_set_clear(96, 16, 16);
		gs_draw_hud_quad(0, 40, 200, 560, 40, 0, 0, 0);
		gs_draw_hud_style(0, 3, 2, 1, 0, 255, 255, 255);
		gs_draw_hud_text(0, 40, 200, 0, 0, 0, why);
		gs_draw_scene();
	}
	leave();
}

int main(int argc, char **argv)
{
	printf("Blazium PSP GDIR guest\n");
	callbacks_start();
	/* the full 333 MHz (the HOME menu's default is 222) */
	scePowerSetClockFrequency(333, 333, 166);
	const int found = data_init(argc, argv);
	int hud_w = 640, hud_h = 448;
	if (found) {
		load_disp(&hud_w, &hud_h);
	}
	if (!gs_draw_boot(hud_w, hud_h)) {
		printf("PSP: the GE did not start\n");
		return 1;
	}
	pad_io_init();
	if (!found) {
		halt("The game's folder is not on the Memory Stick (PSP/GAME/NAME/DATA).");
	}

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
	printf("PSP boot: ready in %u ms\n", (unsigned)((us_now() - t0) / 1000));
	psp_heap_report();

	/* SCRP: "SCRP", u16 abi, u16 flags (bit 0: GDIR), u32 tape bytes, tape, GDIR */
	int gdir = 0;
	if (scrp && scrp_sz >= 12 && memcmp(scrp, "SCRP", 4) == 0 && (ru16(scrp + 6) & 1)) {
		const uint32_t tape = ru32(scrp + 8);
		if (12 + tape <= scrp_sz) {
			gdir = gdir_vm_init(scrp + 12 + tape, scrp_sz - 12 - tape);
		}
	}
	if (!gdir) {
		printf("PSP: no GDIR program (%s)\n", gdir_vm_error());
	}

	uint64_t last = us_now();
	while (!s_quit) {
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
#ifdef PSP_FIXED_DELTA
			gdir_vm_process(PSP_FIXED_DELTA);
#else
			gdir_vm_process(dt);
#endif
			const char *err = gdir_vm_error();
			if (err && err[0]) {
				halt(err);
			}
		}
		gs_draw_scene();
#ifdef PSP_FRAME_STATS
		/* dev: the frame rate */
		{
			static unsigned frames;
			static uint64_t t_stat;
			if (!t_stat) {
				t_stat = now;
			}
			if (++frames == 300) {
				const uint64_t us = us_now() - t_stat;
				printf("PSP frames: %u.%u fps over %u frames\n", (unsigned)(300000000ull / us), (unsigned)(3000000000ull / us % 10), frames);
				frames = 0;
				t_stat = us_now();
			}
		}
#endif
	}
	leave();
	return 0;
}
