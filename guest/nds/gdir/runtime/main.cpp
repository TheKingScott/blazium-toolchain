// Blazium DS GDIR guest - MIT. The PS2 guest's scene runtime (GDIR VM,
// sys_io, pack_io) on the Nintendo DS (DS mode, which a DSi runs too): pack
// 0 comes out of the ROM's NitroFS at boot, the scripts run every frame,
// nds_draw.cpp draws with the 3D engine on the top screen; the bottom
// screen shows the log.

#include "gdir_vm.h"
#include "gs_draw.h"
#include "iso_path.h"
#include "pack_io.h"
#include "pad_io.h"
#include "sfx_io.h"
#include "sys_io.h"

#include <fat.h>
#include <filesystem.h>
#include <malloc.h>
#include <nds.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t ru16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t ru32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* the clock: two cascaded timers at the bus clock, read as a difference */
static uint32_t s_ticks;
static unsigned ms_now(void)
{
	return (unsigned)(((uint64_t)cpuGetTiming() * 1000u) / BUS_CLOCK);
}

/* A whole file from NitroFS (iso_fopen: DATA/ first). */
static uint8_t *load_file(const char *name, unsigned *size)
{
	char path[64];
	snprintf(path, sizeof(path), "cdrom0:\\%s;1", name);
	FILE *f = iso_fopen(path, "rb");
	*size = 0;
	if (!f) {
		printf("DS: %s not in the ROM\n", name);
		return 0;
	}
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = (uint8_t *)memalign(32, (size_t)n + 4);
	if (b && fread(b, 1, (size_t)n, f) == (size_t)n) {
		*size = (unsigned)n;
	} else {
		printf("DS: %s (%ld bytes) did not load (%s)\n", name, n, b ? "read" : "out of memory");
		free(b);
		b = 0;
	}
	fclose(f);
	return b;
}

static unsigned char *reload_gtex0(const unsigned char *mesh, unsigned *size)
{
	(void)mesh; /* only pack 0 is set up without its GTEX */
	*size = 0;
	return load_file("GTEX00.BIN", size);
}

static void halt(const char *why)
{
	printf("DS: %s\n", why);
	for (;;) {
		gs_draw_set_clear(96, 16, 16);
		gs_draw_hud_quad(0, 40, 200, 560, 40, 0, 0, 0);
		gs_draw_hud_style(0, 3, 2, 1, 0, 255, 255, 255);
		gs_draw_hud_text(0, 40, 200, 0, 0, 0, why);
		gs_draw_scene();
	}
}

/* the heap, when pack_io finds no room for a pack */
extern "C" void nds_heap_report(void)
{
	const struct mallinfo mi = mallinfo();
	printf("DS heap: %d bytes in use, %d free of the arena, %u more to grow\n", (int)mi.uordblks, (int)mi.fordblks,
			(unsigned)(getHeapLimit() - getHeapEnd()));
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	gs_draw_boot(0);
#ifdef BLAZIUM_DSI
	printf("Blazium DSi GDIR guest (%s, %d MHz)\n", isDSiMode() ? "DSi mode" : "DS mode", isDSiMode() ? 133 : 67);
#else
	printf("Blazium DS GDIR guest\n");
#endif
	cpuStartTiming(0);
#ifdef BLAZIUM_DSI
	if (!isDSiMode()) {
		halt("This build is for the Nintendo DSi.");
	}
#endif
	/* the SD card (a DSi's, or a flashcart's through DLDI): NitroFS reads
	 * the .nds through it when the ROM is not in slot 1, and saves go there */
	const bool fat = fatInitDefault();
	printf("DS: SD card %s\n", fat ? "found" : "not found");
	if (!nitroFSInit(NULL)) {
		halt("NitroFS did not start (no game data).");
	}
	printf("DS: game data found\n");
	pad_io_init();

	unsigned mesh_sz = 0, gtex_sz = 0, node_sz = 0, scrp_sz = 0;
	uint8_t *scrp = load_file("SCRP00.BIN", &scrp_sz);
	uint8_t *mesh = load_file("MESH00.BIN", &mesh_sz);
	uint8_t *gtex = load_file("GTEX00.BIN", &gtex_sz);
	uint8_t *node = load_file("NODE00.BIN", &node_sz);
	if (!mesh || !gs_draw_init(mesh, mesh_sz, gtex, gtex_sz, node, node_sz)) {
		halt("No scene in the ROM (MESH00).");
	}
	/* pack 0's textures are in VRAM now: gs_draw_init reads them again
	 * when pack 0 comes back */
	gs_draw_set_gtex_reload(reload_gtex0);
	free(gtex);
	gtex = 0;
	gtex_sz = 0;
	printf("DS boot: pack 0 at %u ms\n", ms_now());
	pack_io_set_pack0(mesh, mesh_sz, gtex, gtex_sz, node, node_sz);
	sfx_io_init();
	pack_io_init();
	sys_io_init();
	printf("DS boot: ready at %u ms\n", ms_now());
	nds_heap_report();

	/* SCRP: "SCRP", u16 abi, u16 flags (bit 0: GDIR), u32 tape bytes, tape, GDIR */
	int gdir = 0;
	if (scrp && scrp_sz >= 12 && memcmp(scrp, "SCRP", 4) == 0 && (ru16(scrp + 6) & 1)) {
		const uint32_t tape = ru32(scrp + 8);
		if (12 + tape <= scrp_sz) {
			gdir = gdir_vm_init(scrp + 12 + tape, scrp_sz - 12 - tape);
		}
	}
	if (!gdir) {
		printf("DS: no GDIR program (%s)\n", gdir_vm_error());
	}

	s_ticks = cpuGetTiming();
	for (;;) {
		pad_io_poll(0, 0, 0);
		/* the frame's time from the clock, so a slow frame keeps time */
		const uint32_t now = cpuGetTiming();
		float dt = (float)(now - s_ticks) / (float)BUS_CLOCK;
		s_ticks = now;
		if (dt < 1.0f / 60.0f) {
			dt = 1.0f / 60.0f;
		}
		if (dt > 0.1f) {
			dt = 0.1f;
		}
#ifdef NDS_STATS
		const uint32_t t_vm0 = cpuGetTiming();
#endif
		if (gdir) {
#ifdef NDS_FIXED_DELTA
			gdir_vm_process(NDS_FIXED_DELTA);
#else
			gdir_vm_process(dt);
#endif
			const char *err = gdir_vm_error();
			if (err && err[0]) {
				halt(err);
			}
		}
#ifdef NDS_STATS
		static uint32_t s_vm_t;
		s_vm_t += cpuGetTiming() - t_vm0;
		extern uint32_t nds_dbg_draw_t;
#endif
		gs_draw_scene();
#ifdef NDS_STATS
		{
			/* dev: the time 60 frames took, the polygons of the last, the heap */
			extern unsigned nds_dbg_polys;
			static int s_n;
			static uint32_t s_t0;
			if (++s_n == 600) {
				const uint32_t t = cpuGetTiming();
				const struct mallinfo mi = mallinfo();
				printf("%u ms/600f vm %u draw %u, %u poly heap %dK\n", (unsigned)(((uint64_t)(t - s_t0) * 1000u) / BUS_CLOCK),
						(unsigned)(((uint64_t)s_vm_t * 1000u) / BUS_CLOCK), (unsigned)(((uint64_t)nds_dbg_draw_t * 1000u) / BUS_CLOCK),
						nds_dbg_polys, (int)(mi.uordblks / 1024));
				extern uint32_t nds_dbg_t[5], nds_dbg_items;
				printf(" setup %u back %u cull %u inst %u draw %u items %u\n", (unsigned)(((uint64_t)nds_dbg_t[0] * 1000u) / BUS_CLOCK),
						(unsigned)(((uint64_t)nds_dbg_t[1] * 1000u) / BUS_CLOCK), (unsigned)(((uint64_t)nds_dbg_t[2] * 1000u) / BUS_CLOCK),
						(unsigned)(((uint64_t)nds_dbg_t[3] * 1000u) / BUS_CLOCK), (unsigned)(((uint64_t)nds_dbg_t[4] * 1000u) / BUS_CLOCK),
						nds_dbg_items / 600);
				memset(nds_dbg_t, 0, sizeof(nds_dbg_t));
				nds_dbg_items = 0;
				s_vm_t = 0;
				nds_dbg_draw_t = 0;
				s_t0 = t;
				s_n = 0;
			}
		}
#endif
	}
	return 0;
}
