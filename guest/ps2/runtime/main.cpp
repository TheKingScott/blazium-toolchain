// Blazium PS2 guest — MIT. Links AFL 2.0 ps2sdk graph/draw/dma/packet only.
// P4: double 16-bit frame + 32-bit Z, CPU GIF textured MESH/GTEX. Optional VU1 when linked.

#include "gdir_vm.h"
#include "iso_path.h"
#include "gs_draw.h"
#include "guest_hooks.h"
#include "pack_io.h"
#include "pad_io.h"
#include "script_vm.h"
#include "sfx_io.h"
#include "sys_io.h"
#include "vu1_draw.h"

#include <dma.h>
#include <draw.h>
#include <graph.h>
#include <gs_psm.h>
#include <kernel.h>
#include <packet.h>
#include <stdio.h>
#include <tamtypes.h>

#define BLAZIUM_PS2_COOK_ABI_VALUE 1

#ifndef BLAZIUM_PS2_COOK_ABI
#define BLAZIUM_PS2_COOK_ABI BLAZIUM_PS2_COOK_ABI_VALUE
#endif

#if defined(__has_include)
#if __has_include("cook_flags.h")
#include "cook_flags.h"
#endif
#endif

#ifdef BLAZIUM_PS2_HAS_NODE
extern "C" {
extern const unsigned char cooked_node[];
extern const unsigned int size_cooked_node;
}
#endif
#ifdef BLAZIUM_PS2_HAS_SCRIPT
extern "C" {
extern const unsigned char cooked_script[];
extern const unsigned int size_cooked_script;
}
#endif
#ifdef BLAZIUM_PS2_HAS_MESH
extern "C" {
extern const unsigned char cooked_mesh[];
extern const unsigned int size_cooked_mesh;
}
#endif
#ifdef BLAZIUM_PS2_HAS_GTEX
extern "C" {
extern const unsigned char cooked_gtex[];
extern const unsigned int size_cooked_gtex;
}
#endif

static int s_disp_w = 640;
static int s_disp_h = 448;
static int s_disp_region;

static unsigned ru16(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static int load_disp(void)
{
	static const char *paths[] = {
		"host:DISP.bin",
		"cdrom0:\\DISP.BIN;1",
		"cdrom0:DISP.BIN;1",
		NULL
	};
	for (int i = 0; paths[i]; i++) {
		FILE *f = iso_fopen(paths[i], "rb");
		if (!f) {
			continue;
		}
		unsigned char buf[12];
		const size_t n = fread(buf, 1, 12, f);
		fclose(f);
		if (n < 12 || buf[0] != 'D' || buf[1] != 'I' || buf[2] != 'S' || buf[3] != 'P') {
			continue;
		}
		if (ru16(buf + 4) != 1) {
			continue;
		}
		s_disp_region = (int)ru16(buf + 6);
		const int w = (int)ru16(buf + 8);
		const int h = (int)ru16(buf + 10);
		if (w == 320 || w == 512 || w == 640) {
			s_disp_w = w;
		}
		if (h == 224 || h == 448 || h == 512) {
			s_disp_h = h;
		}
		return 1;
	}
	return 0;
}

static void init_gs(framebuffer_t *frames, zbuffer_t *z)
{
	load_disp();
	for (int i = 0; i < 2; i++) {
		frames[i].width = s_disp_w;
		frames[i].height = s_disp_h;
		frames[i].mask = 0;
		frames[i].psm = GS_PSM_16;
		frames[i].address = graph_vram_allocate(frames[i].width, frames[i].height, frames[i].psm, GRAPH_ALIGN_PAGE);
	}

	z->enable = DRAW_ENABLE;
	z->mask = 0;
	z->method = ZTEST_METHOD_GREATER_EQUAL;
	z->zsm = GS_ZBUF_32;
	z->address = graph_vram_allocate(frames[0].width, frames[0].height, z->zsm, GRAPH_ALIGN_PAGE);

	int mode = 0;
#ifdef GRAPH_MODE_PAL
	mode = s_disp_region ? GRAPH_MODE_PAL : GRAPH_MODE_NTSC;
#endif
	graph_initialize(frames[0].address, frames[0].width, frames[0].height, frames[0].psm, 0, mode);
}

static void init_drawing_environment(framebuffer_t *frame, zbuffer_t *z)
{
	float ox = 0.0f;
	float oy = 0.0f;
	gs_draw_fb_origin(frame->width, frame->height, &ox, &oy);
	packet_t *packet = packet_init(16, PACKET_NORMAL);
	qword_t *q = packet->data;
	q = draw_setup_environment(q, 0, frame, z);
	q = draw_primitive_xyoffset(q, 0, ox, oy);
	q = draw_finish(q);
	FlushCache(0);
	dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data, q - packet->data, 0, 0);
	dma_wait_fast();
	packet_free(packet);
}

static void clear_loop(framebuffer_t *frame)
{
	float ox = 0.0f;
	float oy = 0.0f;
	gs_draw_fb_origin(frame->width, frame->height, &ox, &oy);
	packet_t *packet = packet_init(16, PACKET_NORMAL);
	for (;;) {
		qword_t *q = packet->data;
		q = draw_clear(q, 0, ox, oy, frame->width, frame->height, 32, 64, 160);
		q = draw_finish(q);
		FlushCache(0);
		dma_wait_fast();
		dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data, q - packet->data, 0, 0);
		draw_wait_finish();
		graph_wait_vsync();
	}
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
	dma_channel_fast_waits(DMA_CHANNEL_GIF);

	framebuffer_t frames[2];
	zbuffer_t z;
	init_gs(frames, &z);
	init_drawing_environment(&frames[0], &z);

	const unsigned char *mesh = 0;
	unsigned mesh_sz = 0;
	const unsigned char *gtex = 0;
	unsigned gtex_sz = 0;
	const unsigned char *node = 0;
	unsigned node_sz = 0;
#ifdef BLAZIUM_PS2_HAS_MESH
	mesh = cooked_mesh;
	mesh_sz = size_cooked_mesh;
#endif
#ifdef BLAZIUM_PS2_HAS_GTEX
	gtex = cooked_gtex;
	gtex_sz = size_cooked_gtex;
#endif
#ifdef BLAZIUM_PS2_HAS_NODE
	node = cooked_node;
	node_sz = size_cooked_node;
#endif

	if (!mesh || !gs_draw_init(mesh, mesh_sz, gtex, gtex_sz, node, node_sz) || !gs_draw_ready()) {
		clear_loop(&frames[0]);
		return 0;
	}

	pack_io_set_pack0(mesh, mesh_sz, gtex, gtex_sz, node, node_sz);
	pad_io_init();
	sfx_io_init();
	pack_io_init();
	sys_io_init();
#ifdef BLAZIUM_PS2_HAS_SCRIPT
	script_vm_init(cooked_script, size_cooked_script, node, node_sz);
#endif
	if (user_init) {
		user_init();
	}
	/* Scripted scenes move nodes per frame: the CPU path applies those. */
	const int use_vu1 = !gdir_vm_active() && vu1_draw_init(mesh, mesh_sz) && vu1_draw_ready();

	int context = 0;
	for (;;) {
		float yaw = 0.0f;
		float dolly = 0.0f;
		int cross = 0;
		pad_io_poll(&yaw, &dolly, &cross);
		if (user_pad) {
			user_pad();
		}
		if (gdir_vm_active()) {
			/* The scene's scripts own the pad: no orbit, no Cross jingle. */
			gdir_vm_process(1.0f / 60.0f);
		} else {
			gs_draw_orbit(yaw, dolly);
			if (cross) {
				pad_io_set_rumble(1, 0);
				sfx_io_play();
			} else {
				pad_io_set_rumble(0, 0);
			}
#ifdef BLAZIUM_PS2_HAS_SCRIPT
			script_vm_process(1.0f / 60.0f);
#endif
		}
		if (user_tick) {
			user_tick(1.0f / 60.0f);
		}
		if (use_vu1) {
			vu1_draw_scene(&frames[context], &z);
		} else {
			gs_draw_scene(&frames[context], &z);
		}
		draw_wait_finish();
		graph_wait_vsync();
		graph_set_framebuffer_filtered(frames[context].address, frames[context].width, frames[context].psm, 0, 0);
		context ^= 1;
	}
	return 0;
}
