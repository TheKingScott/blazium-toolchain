// Blazium PS1 GDIR guest - MIT. The PS2 guest's scene runtime (GDIR VM,
// sys_io, pack_io) on the PS1: pack 0 comes off the CD at boot, the scripts
// run every frame, gpu_draw.cpp draws.

#include "gdir_vm.h"
#include "gs_draw.h"
#include "iso_path.h"
#include "pack_io.h"
#include "pad_io.h"
#include "sfx_io.h"
#include "sys_io.h"

#include <psxapi.h>
#include <psxcd.h>
#include <psxetc.h>
#include <psxgpu.h>
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

/* A whole file from the disc (DATA\Pnn\ first, as iso_fopen does). */
static uint8_t *load_file(const char *name, unsigned *size)
{
	char path[64];
	snprintf(path, sizeof(path), "cdrom0:\\%s;1", name);
	FILE *f = iso_fopen(path, "rb");
	*size = 0;
	if (!f) {
		printf("P1: %s not on the disc\n", name);
		return 0;
	}
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = (uint8_t *)malloc((size_t)n + 4);
	if (b && fread(b, 1, (size_t)n, f) == (size_t)n) {
		*size = (unsigned)n;
	} else {
		printf("P1: %s (%ld bytes) did not load (%s)\n", name, n, b ? "read" : "out of memory");
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

static int read_disp_region(void)
{
	unsigned n = 0;
	uint8_t *b = load_file("DISP.BIN", &n);
	int pal = 0;
	if (b && n >= 12 && memcmp(b, "DISP", 4) == 0) {
		pal = ru16(b + 6) ? 1 : 0;
	}
	free(b);
	return pal;
}

static void halt(const char *why)
{
	printf("P1: %s\n", why);
	for (;;) {
		gs_draw_set_clear(96, 16, 16);
		gs_draw_hud_quad(0, 40, 200, 560, 40, 0, 0, 0);
		gs_draw_hud_style(0, 3, 2, 1, 0, 255, 255, 255);
		gs_draw_hud_text(0, 40, 200, 0, 0, 0, why);
		gs_draw_scene();
	}
}

int main(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	printf("Blazium PS1 GDIR guest\n");
	/* the interrupt setup CdInit needs comes with ResetGraph */
	ResetGraph(0);
	CdInit();
	printf("P1 boot: cd up at %d\n", VSync(-1));
	gs_draw_boot(read_disp_region());
	pad_io_init();

	unsigned mesh_sz = 0, gtex_sz = 0, node_sz = 0, scrp_sz = 0;
	/* the scripts stay for good: load them first, low in the heap, so pack 0
	 * (let go during other scenes) leaves its room next to the free top */
	uint8_t *scrp = load_file("SCRP00.BIN", &scrp_sz);
	uint8_t *mesh = load_file("MESH00.BIN", &mesh_sz);
	uint8_t *gtex = load_file("GTEX00.BIN", &gtex_sz);
	uint8_t *node = load_file("NODE00.BIN", &node_sz);
	if (!mesh || !gs_draw_init(mesh, mesh_sz, gtex, gtex_sz, node, node_sz)) {
		halt("No scene on the disc (MESH00).");
	}
	gs_draw_set_gtex_reload(reload_gtex0);
	/* VRAM has the textures now; pack 0 coming back reads them again */
	free(gtex);
	gtex = 0;
	gtex_sz = 0;
	printf("P1 boot: pack 0 at %d\n", VSync(-1));
	pack_io_set_pack0(mesh, mesh_sz, gtex, gtex_sz, node, node_sz);
	sfx_io_init();
	pack_io_init();
	printf("P1 boot: packs at %d\n", VSync(-1));
	sys_io_init();
	printf("P1 boot: sys_io at %d\n", VSync(-1));

	/* SCRP: "SCRP", u16 abi, u16 flags (bit 0: GDIR), u32 tape bytes, tape, GDIR */
	int gdir = 0;
	if (scrp && scrp_sz >= 12 && memcmp(scrp, "SCRP", 4) == 0 && (ru16(scrp + 6) & 1)) {
		const uint32_t tape = ru32(scrp + 8);
		if (12 + tape <= scrp_sz) {
			gdir = gdir_vm_init(scrp + 12 + tape, scrp_sz - 12 - tape);
		}
	}
	if (!gdir) {
		printf("P1: no GDIR program (%s)\n", gdir_vm_error());
	}

	int last = VSync(-1);
	for (;;) {
		pad_io_poll(0, 0, 0);
		/* the frame's time from the VSync count, so a slow frame keeps time */
		const int now = VSync(-1);
		int fields = now - last;
		last = now;
		if (fields < 1) {
			fields = 1;
		}
		if (fields > 6) {
			fields = 6;
		}
		const float hz = gs_draw_ready() ? 60.0f : 60.0f;
#ifdef P1_STATS
		const int t0 = VSync(-1);
#endif
		if (gdir) {
#ifdef P1_FIXED_DELTA
			gdir_vm_process(P1_FIXED_DELTA);
#else
			gdir_vm_process((float)fields / hz);
#endif
			const char *err = gdir_vm_error();
			if (err && err[0]) {
				halt(err);
			}
		}
#ifdef P1_STATS
		const int t1 = VSync(-1);
#endif
		gs_draw_scene();
#ifdef P1_STATS
		/* a printf to the TTY takes a field or more: only in stats builds */
		static int t_vm, t_draw, t_n;
		static char seq[61];
		const int t2 = VSync(-1);
		t_vm += t1 - t0;
		t_draw += t2 - t1;
		seq[t_n] = (char)('0' + (fields < 9 ? fields : 9));
		if (++t_n == 60) {
			HeapUsage hu;
			GetHeapUsage(&hu);
			seq[60] = 0;
			printf("P1 time: vm %d draw %d fields per 60 frames; heap %u of %u bytes in use (peak %u, %u past the peak)\n", t_vm,
					t_draw, (unsigned)hu.alloc, (unsigned)hu.heap, (unsigned)hu.alloc_max,
					(unsigned)(hu.total > hu.alloc_max ? hu.total - hu.alloc_max : 0));
			printf("P1 fields: %s\n", seq);
			t_vm = t_draw = t_n = 0;
		}
#endif
	}
	return 0;
}
