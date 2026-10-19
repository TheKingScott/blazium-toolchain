// Blazium N64 GDIR guest - MIT. The PS2 guest's scene runtime (GDIR VM,
// sys_io, pack_io) on the N64: pack 0 comes out of the cartridge's DFS at
// boot, the scripts run every frame, t3d_draw.cpp draws with Tiny3D.

#include "gdir_vm.h"
#include "gs_draw.h"
#include "iso_path.h"
#include "pack_io.h"
#include "pad_io.h"
#include "sfx_io.h"
#include "sys_io.h"

#include <libdragon.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>

static uint16_t ru16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t ru32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static unsigned ms_now(void)
{
	return (unsigned)(get_ticks() / (TICKS_PER_SECOND / 1000));
}

/* A whole file from the cartridge (iso_fopen: DATA/ first). */
static uint8_t *load_file(const char *name, unsigned *size)
{
	char path[64];
	snprintf(path, sizeof(path), "cdrom0:\\%s;1", name);
	FILE *f = iso_fopen(path, "rb");
	*size = 0;
	if (!f) {
		printf("N64: %s not in the ROM\n", name);
		return 0;
	}
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	/* 16-byte aligned: the renderer reads the mesh as structs */
	uint8_t *b = (uint8_t *)memalign(16, (size_t)n + 4);
	if (b && fread(b, 1, (size_t)n, f) == (size_t)n) {
		*size = (unsigned)n;
	} else {
		printf("N64: %s (%ld bytes) did not load (%s)\n", name, n, b ? "read" : "out of memory");
		free(b);
		b = 0;
	}
	fclose(f);
	return b;
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
	printf("N64: %s\n", why);
	for (;;) {
		gs_draw_set_clear(96, 16, 16);
		gs_draw_hud_quad(0, 40, 200, 560, 40, 0, 0, 0);
		gs_draw_hud_style(0, 3, 2, 1, 0, 255, 255, 255);
		gs_draw_hud_text(0, 40, 200, 0, 0, 0, why);
		gs_draw_scene();
	}
}

/* the heap, when pack_io finds no room for a pack */
extern "C" void n64_heap_report(void)
{
	heap_stats_t hs;
	sys_get_heap_stats(&hs);
	printf("N64 heap: %d of %d bytes in use\n", hs.used, hs.total);
}

/* The VR4300 traps a float op with a denormal operand ("unimplemented
 * operation") even with flush-to-zero on: that only flushes results. The
 * scripts' maths can still feed one in (a value decaying towards zero, a
 * float rebuilt from bits), so the trapping instruction's operands are
 * flushed to zero here and it runs again. Anything else goes to libdragon's
 * handler. */
static void fpu_handler(exception_t *ex)
{
	reg_block_t *r = ex->regs;
	if (ex->code == EXCEPTION_CODE_FLOATING_POINT && (r->fc31 & (1u << 17))) {
		uint32_t pc = r->epc;
		if (r->cr & 0x80000000u) {
			pc += 4; /* in a branch delay slot */
		}
		const uint32_t ins = *(const uint32_t *)(uintptr_t)pc;
		if ((ins >> 26) == 0x11) {
			const unsigned fmt = (ins >> 21) & 31, ft = (ins >> 16) & 31, fs = (ins >> 11) & 31;
			const unsigned regs[2] = { fs, ft };
			for (int k = 0; k < 2; k++) {
				uint64_t v = r->fpr[regs[k]];
				if (fmt == 16) {
					const uint32_t f = (uint32_t)v;
					if (!(f & 0x7f800000u) && (f & 0x007fffffu)) {
						v = (v & ~0xffffffffull) | (f & 0x80000000u);
					}
				} else if (fmt == 17) {
					if (!(v & 0x7ff0000000000000ull) && (v & 0x000fffffffffffffull)) {
						v &= 0x8000000000000000ull;
					}
				}
				r->fpr[regs[k]] = v;
			}
			r->fc31 &= ~0x0003f000u; /* the cause bits */
			return;
		}
	}
	exception_default_handler(ex);
}

int main(void)
{
	debug_init_emulog();
	debug_init_usblog();
	printf("Blazium N64 GDIR guest\n");
	register_exception_handler(fpu_handler);
	timer_init();
	dfs_init(DFS_DEFAULT_LOCATION);
	gs_draw_boot(read_disp_region());
	pad_io_init();

	unsigned mesh_sz = 0, gtex_sz = 0, node_sz = 0, scrp_sz = 0;
	uint8_t *scrp = load_file("SCRP00.BIN", &scrp_sz);
	uint8_t *mesh = load_file("MESH00.BIN", &mesh_sz);
	uint8_t *gtex = load_file("GTEX00.BIN", &gtex_sz);
	uint8_t *node = load_file("NODE00.BIN", &node_sz);
	if (!mesh || !gs_draw_init(mesh, mesh_sz, gtex, gtex_sz, node, node_sz)) {
		halt("No scene in the ROM (MESH00).");
	}
	printf("N64 boot: pack 0 at %u ms\n", ms_now());
	pack_io_set_pack0(mesh, mesh_sz, gtex, gtex_sz, node, node_sz);
	sfx_io_init();
	pack_io_init();
	sys_io_init();
	printf("N64 boot: ready at %u ms\n", ms_now());

	/* SCRP: "SCRP", u16 abi, u16 flags (bit 0: GDIR), u32 tape bytes, tape, GDIR */
	int gdir = 0;
	if (scrp && scrp_sz >= 12 && memcmp(scrp, "SCRP", 4) == 0 && (ru16(scrp + 6) & 1)) {
		const uint32_t tape = ru32(scrp + 8);
		if (12 + tape <= scrp_sz) {
			gdir = gdir_vm_init(scrp + 12 + tape, scrp_sz - 12 - tape);
		}
	}
	if (!gdir) {
		printf("N64: no GDIR program (%s)\n", gdir_vm_error());
#ifdef N64_STATS
		halt(gdir_vm_error());
#endif
	}

	uint64_t last = get_ticks();
	for (;;) {
		pad_io_poll(0, 0, 0);
		/* the frame's time from the clock, so a slow frame keeps time */
		const uint64_t now = get_ticks();
		float dt = (float)(now - last) / (float)TICKS_PER_SECOND;
		last = now;
		if (dt < 1.0f / 60.0f) {
			dt = 1.0f / 60.0f;
		}
		if (dt > 0.1f) {
			dt = 0.1f;
		}
		if (gdir) {
#ifdef N64_FIXED_DELTA
			gdir_vm_process(N64_FIXED_DELTA);
#else
			gdir_vm_process(dt);
#endif
			const char *err = gdir_vm_error();
			if (err && err[0]) {
				halt(err);
			}
		}
		gs_draw_scene();
#ifdef N64_STATS
		static int s_n;
		static uint64_t s_t0;
		if (++s_n == 60) {
			const uint64_t t = get_ticks();
			heap_stats_t hs;
			sys_get_heap_stats(&hs);
			printf("N64 time: %u ms per 60 frames; heap %d of %d\n", (unsigned)((t - s_t0) / (TICKS_PER_SECOND / 1000)), hs.used, hs.total);
			s_t0 = t;
			s_n = 0;
		}
#endif
	}
	return 0;
}
