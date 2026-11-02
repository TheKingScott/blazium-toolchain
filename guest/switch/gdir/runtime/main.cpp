// Blazium Switch GDIR guest - MIT. The PS2 guest's scene runtime (GDIR VM,
// sys_io, pack_io) on the Nintendo Switch (libnx, as a homebrew .nro): the
// cooked packs are files in the game's DATA folder (the .nro's RomFS, or a
// folder on the SD card), pack 0 loads at boot, the scripts run every frame
// and dk_draw.cpp draws with deko3d, on the console's screen or the TV.
// applet's main loop runs the HOME Menu and closing the software.

#include "gdir_vm.h"
#include "gs_draw.h"
#include "iso_path.h"
#include "pack_io.h"
#include "pad_io.h"
#include "sfx_io.h"
#include "sys_io.h"

#include <dirent.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <sys/stat.h>

extern "C" {
char switch_data[256];
char switch_save[256];
void switch_sfx_shutdown(void);
void switch_gfx_shutdown(void);
}

#define ROMFS "romfs:/"
#define SD "sdmc:/"

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
	return armTicksToNs(armGetSystemTick()) / 1000;
}

/* printf to the debug log (svcOutputDebugString: an emulator's log at its
 * Debug level, or Atmosphere's with its logging on) and to blazium_log.txt
 * in the SD card's switch/ folder, written through line by line, so the
 * last line says how far a run got: newlib's stdout goes nowhere in an
 * .nro without nxlink. The Makefile links printf here (--wrap=printf). */
static FILE *s_log;

static void log_line(const char *buf, size_t k)
{
	svcOutputDebugString(buf, k);
	if (s_log) {
		fwrite(buf, 1, k, s_log);
		fputc('\n', s_log);
		fflush(s_log);
	}
}

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
		log_line(buf, k);
	}
	return n;
}

static void log_open(void)
{
	mkdir(SD "switch", 0777);
	s_log = fopen(SD "switch/blazium_log.txt", "w");
}

/* A fatal error from a library (deko3d raises its errors this way, the
 * result's module 359): into the log first, with who called (the
 * Makefile links diagAbortWithResult here, --wrap). */
extern "C" void __real_diagAbortWithResult(Result res);
extern "C" void __wrap_diagAbortWithResult(Result res)
{
	printf("Switch: fatal error 0x%x (module %u, description %u), from %p", (unsigned)res, (unsigned)R_MODULE(res),
			(unsigned)R_DESCRIPTION(res), __builtin_return_address(0));
	__real_diagAbortWithResult(res);
	for (;;) {
	}
}

/* A crash (a bad memory access, an undefined instruction): what and where
 * into the log, if the system or the emulator hands it to us. */
extern "C" {
alignas(16) u8 __nx_exception_stack[0x4000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
void __libnx_exception_handler(ThreadExceptionDump *ctx)
{
	printf("Switch: crash: exception %u at pc %p (lr %p, sp %p, address %p)", (unsigned)ctx->error_desc,
			(void *)ctx->pc.x, (void *)ctx->lr.x, (void *)ctx->sp.x, (void *)ctx->far.x);
	if (s_log) {
		fclose(s_log);
		s_log = 0;
	}
	svcReturnFromException(MAKERESULT(Module_Kernel, KernelError_UnhandledUserInterrupt));
}
}

/* A whole file from the game's DATA folder. */
static unsigned char *load_file(const char *name, unsigned *size)
{
	char path[64];
	snprintf(path, sizeof(path), "cdrom0:\\%s;1", name);
	FILE *f = iso_fopen(path, "rb");
	*size = 0;
	if (!f) {
		printf("Switch: %s not in the game's folder\n", name);
		return 0;
	}
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *b = (unsigned char *)memalign(64, (size_t)n + 4);
	if (b && fread(b, 1, (size_t)n, f) == (size_t)n) {
		*size = (unsigned)n;
	} else {
		printf("Switch: %s (%ld bytes) did not load\n", name, n);
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

/* The game's folders. An .nro from switch build has the packs in its
 * RomFS's DATA/ and the name of its folder on the SD card in APPDIR.TXT,
 * where the save goes (switch/NAME/). Started without them, the packs are
 * in DATA/ beside the .nro (argv[0]), else in the first switch/ folder that
 * has them. */
static void find_dirs(int argc, char **argv)
{
	switch_data[0] = 0;
	switch_save[0] = 0;
	if (has_data(ROMFS "DATA/")) {
		snprintf(switch_data, sizeof(switch_data), ROMFS "DATA/");
		char name[64] = "";
		FILE *f = fopen(ROMFS "APPDIR.TXT", "rb");
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
			snprintf(switch_save, sizeof(switch_save), SD "switch/%s/", name);
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
				snprintf(switch_data, sizeof(switch_data), "%s", p);
				snprintf(switch_save, sizeof(switch_save), "%s", dir);
				return;
			}
		}
	}
	DIR *d = opendir(SD "switch");
	if (!d) {
		return;
	}
	struct dirent *e;
	while ((e = readdir(d)) != 0) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char p[256];
		snprintf(p, sizeof(p), SD "switch/%s/DATA/", e->d_name);
		if (has_data(p)) {
			snprintf(switch_data, sizeof(switch_data), "%s", p);
			snprintf(switch_save, sizeof(switch_save), SD "switch/%s/", e->d_name);
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
	switch_sfx_shutdown();
	switch_gfx_shutdown();
	romfsExit();
}

/* An error on screen until the software is closed; then everything goes
 * back and game_main returns (exit() would leave from this thread, on the
 * main thread's stack, which is still waiting for it). */
static void halt(const char *why)
{
	printf("Switch: %s\n", why);
	while (appletMainLoop()) {
		gs_draw_set_clear(96, 16, 16);
		gs_draw_hud_quad(0, 40, 200, 560, 40, 0, 0, 0);
		gs_draw_hud_style(0, 3, 2, 1, 0, 255, 255, 255);
		gs_draw_hud_text(0, 40, 200, 0, 0, 0, why);
		gs_draw_scene();
	}
	leave();
}

static int game_main(int argc, char **argv)
{
	log_open();
	printf("Blazium Switch GDIR guest\n");
	const Result rc = romfsInit();
	if (R_FAILED(rc)) {
		printf("Switch: no RomFS (0x%x)\n", (unsigned)rc);
	}
	find_dirs(argc, argv);
	printf("Switch: data %s, save %s\n", switch_data[0] ? switch_data : "(none)", switch_save[0] ? switch_save : "(none)");
	int hud_w = 640, hud_h = 448;
	load_disp(&hud_w, &hud_h);
	if (!gs_draw_boot(hud_w, hud_h)) {
		printf("Switch: deko3d did not start\n");
		romfsExit();
		return 1;
	}
	pad_io_init();

	unsigned mesh_sz = 0, gtex_sz = 0, node_sz = 0, scrp_sz = 0;
	unsigned char *scrp = load_file("SCRP00.BIN", &scrp_sz);
	unsigned char *mesh = load_file("MESH00.BIN", &mesh_sz);
	unsigned char *gtex = load_file("GTEX00.BIN", &gtex_sz);
	unsigned char *node = load_file("NODE00.BIN", &node_sz);
	if (!mesh || !gs_draw_init(mesh, mesh_sz, gtex, gtex_sz, node, node_sz)) {
		halt("No scene: the game's DATA folder (MESH00.BIN) is not in the .nro or on the SD card.");
		return 1;
	}
	const uint64_t t0 = us_now();
	pack_io_set_pack0(mesh, mesh_sz, gtex, gtex_sz, node, node_sz);
	sfx_io_init();
	pack_io_init();
	sys_io_init();
	printf("Switch boot: ready in %u ms\n", (unsigned)((us_now() - t0) / 1000));

	/* SCRP: "SCRP", u16 abi, u16 flags (bit 0: GDIR), u32 tape bytes, tape, GDIR */
	int gdir = 0;
	if (scrp && scrp_sz >= 12 && memcmp(scrp, "SCRP", 4) == 0 && (ru16(scrp + 6) & 1)) {
		const uint32_t tape = ru32(scrp + 8);
		if (12 + tape <= scrp_sz) {
			gdir = gdir_vm_init(scrp + 12 + tape, scrp_sz - 12 - tape);
		}
	}
	if (!gdir) {
		printf("Switch: no GDIR program (%s)\n", gdir_vm_error());
	}

	uint64_t last = us_now();
	while (appletMainLoop()) {
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
#ifdef SWITCH_FIXED_DELTA
			gdir_vm_process(SWITCH_FIXED_DELTA);
#else
			gdir_vm_process(dt);
#endif
			const char *err = gdir_vm_error();
			if (err && err[0]) {
				halt(err);
				return 1;
			}
		}
		gs_draw_scene();
	}
	leave();
	return 0;
}

/* The VM recurses as deep as the scripts do: the game runs on a thread of
 * its own with 2 MB of stack (an emulator may give the main thread less),
 * at the main thread's priority, on its core. */
#define GAME_STACK (2 * 1024 * 1024)

static int s_argc;
static char **s_argv;
static int s_ret;

static void game_thread(void *arg)
{
	(void)arg;
	s_ret = game_main(s_argc, s_argv);
}

int main(int argc, char **argv)
{
	s_argc = argc;
	s_argv = argv;
	Thread th;
	s32 prio = 0x2C;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	if (R_FAILED(threadCreate(&th, game_thread, 0, NULL, GAME_STACK, prio, -2))) {
		return game_main(argc, argv);
	}
	if (R_FAILED(threadStart(&th))) {
		threadClose(&th);
		return game_main(argc, argv);
	}
	threadWaitForExit(&th);
	threadClose(&th);
	return s_ret;
}
