// Blazium Wii U GDIR guest - MIT. The PS2 guest's scene runtime (GDIR VM,
// sys_io, pack_io) on the Wii U: the cooked packs are files in the game's
// DATA folder (the .wuhb's content, or a folder on the SD card), pack 0
// loads at boot, the scripts run every frame and gx2_draw.cpp draws with
// the GX2 on the TV and the GamePad. ProcUI (libwhb's WHBProc) runs the
// HOME button menu and closing the software.

#include "gdir_vm.h"
#include "gs_draw.h"
#include "iso_path.h"
#include "pack_io.h"
#include "pad_io.h"
#include "sfx_io.h"
#include "sys_io.h"

#include <coreinit/core.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <dirent.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <whb/gfx.h>
#include <whb/log.h>
#include <whb/log_cafe.h>
#include <whb/log_udp.h>
#include <whb/proc.h>

extern "C" {
char wiiu_data[256];
char wiiu_save[256];
void wiiu_sfx_shutdown(void);
}

#define CONTENT "fs:/vol/content/"
#define SD "fs:/vol/external01/"

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
	return (uint64_t)OSTicksToMicroseconds(OSGetSystemTime());
}

/* printf to the system log (Cemu's log, or a PC on the network running
 * wut's udplogserver): newlib's stdout goes nowhere on the Wii U. The
 * Makefile links printf here (--wrap=printf). */
extern "C" int __wrap_printf(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	size_t k = strlen(buf);
	while (k && buf[k - 1] == '\n') {
		buf[--k] = 0;
	}
	if (k) {
		WHBLogWrite(buf);
	}
	return n;
}

/* A whole file from the game's DATA folder. */
static unsigned char *load_file(const char *name, unsigned *size)
{
	char path[64];
	snprintf(path, sizeof(path), "cdrom0:\\%s;1", name);
	FILE *f = iso_fopen(path, "rb");
	*size = 0;
	if (!f) {
		printf("Wii U: %s not in the game's folder\n", name);
		return 0;
	}
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *b = (unsigned char *)memalign(64, (size_t)n + 4);
	if (b && fread(b, 1, (size_t)n, f) == (size_t)n) {
		*size = (unsigned)n;
	} else {
		printf("Wii U: %s (%ld bytes) did not load\n", name, n);
		free(b);
		b = 0;
	}
	fclose(f);
	return b;
}

static int is_file(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* dir (with its trailing '/') holds the game's data */
static int has_data(const char *dir)
{
	char p[300];
	snprintf(p, sizeof(p), "%sMESH00.BIN", dir);
	return is_file(p);
}

/* The game's folders. A .wuhb (wiiu build) has the packs in its content's
 * DATA/ and the name of its folder on the SD card in APPDIR.TXT, where the
 * save goes (wiiu/apps/NAME/). Started as a bare .rpx, the packs are in
 * DATA/ beside it (argv[0]), else in the first wiiu/apps/ folder that has
 * them. */
static void find_dirs(int argc, char **argv)
{
	wiiu_data[0] = 0;
	wiiu_save[0] = 0;
	if (has_data(CONTENT "DATA/")) {
		snprintf(wiiu_data, sizeof(wiiu_data), CONTENT "DATA/");
		char name[64] = "";
		FILE *f = fopen(CONTENT "APPDIR.TXT", "rb");
		if (f) {
			const size_t n = fread(name, 1, sizeof(name) - 1, f);
			name[n] = 0;
			fclose(f);
			for (size_t i = 0; i < n; i++) {
				if (name[i] == '\r' || name[i] == '\n') {
					name[i] = 0;
				}
			}
		}
		struct stat st;
		if (name[0] && stat(SD, &st) == 0) {
			snprintf(wiiu_save, sizeof(wiiu_save), SD "wiiu/apps/%s/", name);
		}
		return;
	}
	if (argc > 0 && argv && argv[0]) {
		char dir[200];
		snprintf(dir, sizeof(dir), "%s", argv[0]);
		char *slash = strrchr(dir, '/');
		if (slash) {
			slash[1] = 0;
			char p[256];
			snprintf(p, sizeof(p), "%sDATA/", dir);
			if (has_data(p)) {
				snprintf(wiiu_data, sizeof(wiiu_data), "%s", p);
				snprintf(wiiu_save, sizeof(wiiu_save), "%s", dir);
				return;
			}
		}
	}
	DIR *d = opendir(SD "wiiu/apps");
	if (!d) {
		return;
	}
	struct dirent *e;
	while ((e = readdir(d)) != 0) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char p[256];
		snprintf(p, sizeof(p), SD "wiiu/apps/%s/DATA/", e->d_name);
		if (has_data(p)) {
			snprintf(wiiu_data, sizeof(wiiu_data), "%s", p);
			snprintf(wiiu_save, sizeof(wiiu_save), SD "wiiu/apps/%s/", e->d_name);
			break;
		}
	}
	closedir(d);
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

static void leave(void)
{
	sfx_io_stop();
	sfx_io_music_stop();
	wiiu_sfx_shutdown();
	WHBGfxShutdown();
	WHBProcShutdown();
	WHBLogUdpDeinit();
	WHBLogCafeDeinit();
}

static void halt(const char *why)
{
	printf("Wii U: %s\n", why);
	while (WHBProcIsRunning()) {
		gs_draw_set_clear(96, 16, 16);
		gs_draw_hud_quad(0, 40, 200, 560, 40, 0, 0, 0);
		gs_draw_hud_style(0, 3, 2, 1, 0, 255, 255, 255);
		gs_draw_hud_text(0, 40, 200, 0, 0, 0, why);
		gs_draw_scene();
	}
	leave();
	exit(0);
}

static int game_main(int argc, char **argv)
{
	WHBProcInit();
	WHBLogCafeInit();
	WHBLogUdpInit();
	printf("Blazium Wii U GDIR guest\n");
	find_dirs(argc, argv);
	printf("Wii U: data %s, save %s\n", wiiu_data[0] ? wiiu_data : "(none)", wiiu_save[0] ? wiiu_save : "(none)");
	int hud_w = 640, hud_h = 448;
	load_disp(&hud_w, &hud_h);
	if (!gs_draw_boot(hud_w, hud_h)) {
		printf("Wii U: the GX2 did not start\n");
		WHBProcShutdown();
		return 1;
	}
	pad_io_init();

	unsigned mesh_sz = 0, gtex_sz = 0, node_sz = 0, scrp_sz = 0;
	unsigned char *scrp = load_file("SCRP00.BIN", &scrp_sz);
	unsigned char *mesh = load_file("MESH00.BIN", &mesh_sz);
	unsigned char *gtex = load_file("GTEX00.BIN", &gtex_sz);
	unsigned char *node = load_file("NODE00.BIN", &node_sz);
	if (!mesh || !gs_draw_init(mesh, mesh_sz, gtex, gtex_sz, node, node_sz)) {
		halt("No scene: the game's DATA folder (MESH00.BIN) is not in the .wuhb or on the SD card.");
	}
	const uint64_t t0 = us_now();
	pack_io_set_pack0(mesh, mesh_sz, gtex, gtex_sz, node, node_sz);
	sfx_io_init();
	pack_io_init();
	sys_io_init();
	printf("Wii U boot: ready in %u ms\n", (unsigned)((us_now() - t0) / 1000));

	/* SCRP: "SCRP", u16 abi, u16 flags (bit 0: GDIR), u32 tape bytes, tape, GDIR */
	int gdir = 0;
	if (scrp && scrp_sz >= 12 && memcmp(scrp, "SCRP", 4) == 0 && (ru16(scrp + 6) & 1)) {
		const uint32_t tape = ru32(scrp + 8);
		if (12 + tape <= scrp_sz) {
			gdir = gdir_vm_init(scrp + 12 + tape, scrp_sz - 12 - tape);
		}
	}
	if (!gdir) {
		printf("Wii U: no GDIR program (%s)\n", gdir_vm_error());
	}

	uint64_t last = us_now();
	while (WHBProcIsRunning()) {
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
#ifdef WIIU_FIXED_DELTA
			gdir_vm_process(WIIU_FIXED_DELTA);
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
	leave();
	return 0;
}

/* The VM recurses as deep as the scripts do, and an RPX's main thread has
 * 64 KB of stack: the game runs on a thread of its own with 1 MB (as the
 * PS3 guest asks for), on the main thread's core (the GX2 and ProcUI's). */
#define GAME_STACK (1024 * 1024)

static int s_argc;
static char **s_argv;

static int game_thread(int argc, void *argv)
{
	(void)argc;
	(void)argv;
	return game_main(s_argc, s_argv);
}

int main(int argc, char **argv)
{
	s_argc = argc;
	s_argv = argv;
	OSThread *th = (OSThread *)memalign(16, sizeof(OSThread));
	void *stack = memalign(16, GAME_STACK);
	if (!th || !stack) {
		return game_main(argc, argv);
	}
	memset(th, 0, sizeof(OSThread));
	const OSThreadAttributes core = (OSThreadAttributes)(1 << OSGetCoreId());
	if (!OSCreateThread(th, game_thread, 0, 0, (char *)stack + GAME_STACK, GAME_STACK, OSGetThreadPriority(OSGetCurrentThread()), core)) {
		return game_main(argc, argv);
	}
	OSSetThreadName(th, "blazium");
	OSResumeThread(th);
	int ret = 0;
	OSJoinThread(th, &ret);
	return ret;
}
