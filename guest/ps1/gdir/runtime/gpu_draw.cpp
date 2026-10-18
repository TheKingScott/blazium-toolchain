// MIT. The PS2 guest's draw API (gs_draw.h) on the PS1: the scene's P1MS
// triangles through the GTE into an ordering table, P1TX textures in VRAM,
// and the HUD (rects, the 5x7 font and a 3x5 one for small text) on top.
// Within one depth bin triangles keep the mesh order (layered UI quads).
//
// Space: P1MS positions are Godot's (Y up, metres) times the mesh's scale.
// The camera is a float look-at like the PS2 guest's; each chunk's
// view x node matrix becomes one GTE matrix (rotation 4.12, translation in
// mesh units) with Y and Z flipped for the GTE (Y down, Z into the screen).
// The GPU has no depth buffer: triangles sort by average depth.
//
// The HUD keeps the PS2's 640x448 layout and is scaled onto 320x240.

#include "gs_draw.h"

#include <hwregs_c.h>
#include <inline_c.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <math.h>
#include <stdio.h>

#define SCR_W 320
#define SCR_H 240
#define HUD_W 640.0f
#define HUD_H 448.0f
/* The PS2's 448 lines go onto the middle 224 of the PS1's 240: a TV (and
 * an emulator's overscan crop) hides about 8 lines top and bottom. With
 * that, one PS2 pixel is half a PS1 pixel both ways, and the 3D framing
 * needs no squeeze: the projection distance is 112 lines over tan(fov/2). */
#define SAFE_Y 8
#define OT_LEN 1024
#define PRIM_BYTES (96 * 1024)
#define MAX_TEX 64
#define MAX_NODES 512
#define NEAR_M 0.2f

struct Frame {
	DISPENV disp;
	DRAWENV draw;
	uint32_t ot[OT_LEN];
	uint8_t pri[PRIM_BYTES];
};
static Frame g_fb[2];
static int g_cur;
static uint8_t *g_pp;
static uint8_t *g_pend;
static int g_pal;

/* ------------------------------------------------------------ state */
struct Tex {
	int ready;
	int w, h, psm;
	int tiled; /* UVs run past one tile: drawn through the texture window */
	uint32_t twin; /* GP0(E2) for that window */
	uint16_t tpage, clut;
	uint8_t u0, v0;
};
static Tex g_tex[MAX_TEX];
static int g_tex_n;

static const uint8_t *g_mesh;
static unsigned g_mesh_sz;
static int g_scale = 1024;
static unsigned g_vert_n, g_tri_n, g_chunk_n;
/* P1MS v2: both 4-byte aligned, the vertex position is a GTE SVECTOR */
struct P1Vert {
	SVECTOR p;
	uint8_t r, g, b, pad;
};
struct P1Tri {
	uint16_t a, b, c;
	uint8_t uv[6];
	uint8_t tex, flags;
	uint16_t pad;
};
static const P1Vert *g_verts;
static const P1Tri *g_tris;
static const uint8_t *g_chunks;
/* each chunk's sphere in mesh units, for culling with the GTE */
struct ChunkI {
	SVECTOR c; /* pad holds nothing */
	int32_t r;
};
static ChunkI *g_cint;
static const uint8_t *g_inst;
static unsigned g_inst_n;
static uint8_t g_isrc[MAX_NODES];
static uint16_t g_isrc_c0[MAX_NODES], g_isrc_c1[MAX_NODES];
static uint8_t g_nfog[MAX_NODES];
static int g_ready;

static float g_nm[MAX_NODES][12];
static uint8_t g_nm_on[MAX_NODES];
static uint8_t g_nhide[MAX_NODES];
static int g_nodes_live;


static float g_cam_x, g_cam_y = 3.0f, g_cam_z = 8.0f;
static float g_look_x, g_look_y = 1.5f, g_look_z;
static float g_fov = 55.0f;
static float g_pitch, g_yaw, g_roll;
static float g_attach_x, g_attach_y, g_attach_z;
static float g_shake_x, g_shake_y, g_shake_z;
static float g_eye_h;
static float g_world_yaw;
static int g_cam_locked;
static int g_clear_r = 32, g_clear_g = 64, g_clear_b = 160;
static int g_fade_a, g_fade_r, g_fade_g, g_fade_b;
static int g_fog_on, g_fog_r, g_fog_g, g_fog_b;
static float g_fog_near = 40.0f, g_fog_far = 160.0f;
static float g_nofs[32][3];
static float g_nrot[32][3];

struct HudQuad {
	int used;
	int x, y, w, h;
	int r, g, b;
	int kind, scale, align, hidden;
	int tr, tg, tb;
	int styled;
	char text[65];
};
static HudQuad g_hudq[16];

/* font: the PS2 guest's 5x7 table, uploaded as a 4-bit texture */
static const unsigned char kFont5x7[95][7] = {
	{0, 0, 0, 0, 0, 0, 0}, {4, 4, 4, 4, 4, 0, 4}, {10, 10, 0, 0, 0, 0, 0}, {10, 31, 10, 31, 10, 0, 0},
	{4, 14, 20, 14, 5, 14, 4}, {17, 18, 4, 8, 19, 0, 0}, {8, 20, 8, 21, 18, 13, 0}, {4, 4, 0, 0, 0, 0, 0},
	{4, 8, 8, 8, 8, 8, 4}, {4, 2, 2, 2, 2, 2, 4}, {0, 10, 4, 31, 4, 10, 0}, {0, 4, 4, 31, 4, 4, 0},
	{0, 0, 0, 0, 4, 4, 8}, {0, 0, 0, 31, 0, 0, 0}, {0, 0, 0, 0, 0, 4, 4}, {1, 2, 4, 8, 16, 0, 0},
	{14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14}, {14, 17, 1, 6, 8, 16, 31}, {14, 17, 1, 6, 1, 17, 14},
	{2, 6, 10, 18, 31, 2, 2}, {31, 16, 30, 1, 1, 17, 14}, {6, 8, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},
	{14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 2, 12}, {0, 4, 4, 0, 4, 4, 0}, {0, 4, 4, 0, 4, 4, 8},
	{2, 4, 8, 16, 8, 4, 2}, {0, 0, 31, 0, 31, 0, 0}, {8, 4, 2, 1, 2, 4, 8}, {14, 17, 1, 6, 4, 0, 4},
	{14, 17, 19, 21, 23, 16, 14}, {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14},
	{30, 17, 17, 17, 17, 17, 30}, {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16}, {14, 17, 16, 23, 17, 17, 15},
	{17, 17, 17, 31, 17, 17, 17}, {14, 4, 4, 4, 4, 4, 14}, {1, 1, 1, 1, 17, 17, 14}, {17, 18, 20, 24, 20, 18, 17},
	{16, 16, 16, 16, 16, 16, 31}, {17, 27, 21, 21, 17, 17, 17}, {17, 25, 21, 19, 17, 17, 17}, {14, 17, 17, 17, 17, 17, 14},
	{30, 17, 17, 30, 16, 16, 16}, {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17}, {14, 17, 16, 14, 1, 17, 14},
	{31, 4, 4, 4, 4, 4, 4}, {17, 17, 17, 17, 17, 17, 14}, {17, 17, 17, 17, 17, 10, 4}, {17, 17, 17, 21, 21, 21, 10},
	{17, 17, 10, 4, 10, 17, 17}, {17, 17, 10, 4, 4, 4, 4}, {31, 1, 2, 4, 8, 16, 31}, {14, 8, 8, 8, 8, 8, 14},
	{16, 8, 4, 2, 1, 0, 0}, {14, 2, 2, 2, 2, 2, 14}, {4, 10, 17, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 31},
	{8, 4, 0, 0, 0, 0, 0}, {0, 0, 14, 1, 15, 17, 15}, {16, 16, 22, 25, 17, 17, 30}, {0, 0, 14, 16, 16, 17, 14},
	{1, 1, 13, 19, 17, 17, 15}, {0, 0, 14, 17, 31, 16, 14}, {6, 9, 8, 28, 8, 8, 8}, {0, 15, 17, 17, 15, 1, 14},
	{16, 16, 22, 25, 17, 17, 17}, {4, 0, 12, 4, 4, 4, 14}, {2, 0, 6, 2, 2, 18, 12}, {16, 16, 18, 20, 24, 20, 18},
	{12, 4, 4, 4, 4, 4, 14}, {0, 0, 26, 21, 21, 17, 17}, {0, 0, 22, 25, 17, 17, 17}, {0, 0, 14, 17, 17, 17, 14},
	{0, 0, 30, 17, 30, 16, 16}, {0, 0, 13, 19, 15, 1, 1}, {0, 0, 22, 25, 16, 16, 16}, {0, 0, 14, 16, 14, 1, 30},
	{8, 8, 28, 8, 8, 9, 6}, {0, 0, 17, 17, 17, 19, 13}, {0, 0, 17, 17, 17, 10, 4}, {0, 0, 17, 17, 21, 21, 10},
	{0, 0, 17, 10, 4, 10, 17}, {0, 0, 17, 17, 15, 1, 14}, {0, 0, 31, 2, 4, 8, 31}, {2, 4, 4, 8, 4, 4, 2},
	{4, 4, 4, 4, 4, 4, 4}, {8, 4, 4, 2, 4, 4, 8}, {0, 0, 8, 21, 2, 0, 0},
};
/* The PS2's smallest text (scale 1) is 3 pixels a character at 320 wide:
 * that size gets this 3x5 font (lower case as small capitals). */
static const unsigned char kFont3x5[95][5] = {
	{0, 0, 0, 0, 0}, {2, 2, 2, 0, 2}, {5, 5, 0, 0, 0}, {5, 7, 5, 7, 5}, {3, 6, 2, 3, 6}, {5, 1, 2, 4, 5}, {2, 5, 2, 5, 3}, {2, 2, 0, 0, 0},
	{1, 2, 2, 2, 1}, {4, 2, 2, 2, 4}, {0, 5, 2, 5, 0}, {0, 2, 7, 2, 0}, {0, 0, 0, 2, 4}, {0, 0, 7, 0, 0}, {0, 0, 0, 0, 2}, {1, 1, 2, 4, 4},
	{7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {6, 1, 2, 4, 7}, {6, 1, 2, 1, 6}, {5, 5, 7, 1, 1}, {7, 4, 6, 1, 6}, {3, 4, 7, 5, 7}, {7, 1, 2, 2, 2},
	{7, 5, 7, 5, 7}, {7, 5, 7, 1, 6}, {0, 2, 0, 2, 0}, {0, 2, 0, 2, 4}, {1, 2, 4, 2, 1}, {0, 7, 0, 7, 0}, {4, 2, 1, 2, 4}, {6, 1, 2, 0, 2},
	{2, 5, 7, 4, 3}, {2, 5, 7, 5, 5}, {6, 5, 6, 5, 6}, {3, 4, 4, 4, 3}, {6, 5, 5, 5, 6}, {7, 4, 6, 4, 7}, {7, 4, 6, 4, 4}, {3, 4, 5, 5, 3},
	{5, 5, 7, 5, 5}, {7, 2, 2, 2, 7}, {1, 1, 1, 5, 2}, {5, 5, 6, 5, 5}, {4, 4, 4, 4, 7}, {5, 7, 7, 5, 5}, {6, 5, 5, 5, 5}, {2, 5, 5, 5, 2},
	{6, 5, 6, 4, 4}, {2, 5, 5, 6, 3}, {6, 5, 6, 5, 5}, {3, 4, 2, 1, 6}, {7, 2, 2, 2, 2}, {5, 5, 5, 5, 7}, {5, 5, 5, 5, 2}, {5, 5, 7, 7, 5},
	{5, 5, 2, 5, 5}, {5, 5, 2, 2, 2}, {7, 1, 2, 4, 7}, {3, 2, 2, 2, 3}, {4, 4, 2, 1, 1}, {6, 2, 2, 2, 6}, {2, 5, 0, 0, 0}, {0, 0, 0, 0, 7},
	{4, 2, 0, 0, 0}, {0, 2, 7, 5, 5}, {0, 6, 6, 5, 6}, {0, 3, 4, 4, 3}, {0, 6, 5, 5, 6}, {0, 7, 6, 4, 7}, {0, 7, 6, 4, 4}, {3, 4, 5, 5, 3},
	{0, 5, 7, 5, 5}, {0, 7, 2, 2, 7}, {1, 1, 1, 5, 2}, {0, 5, 6, 5, 5}, {0, 4, 4, 4, 7}, {0, 5, 7, 5, 5}, {0, 6, 5, 5, 5}, {0, 2, 5, 5, 2},
	{6, 5, 6, 4, 4}, {2, 5, 5, 6, 3}, {0, 6, 6, 5, 5}, {0, 3, 2, 1, 6}, {0, 7, 2, 2, 2}, {0, 5, 5, 5, 7}, {0, 5, 5, 5, 2}, {0, 5, 7, 7, 5},
	{0, 5, 2, 5, 5}, {5, 5, 2, 2, 2}, {0, 7, 2, 4, 7}, {3, 2, 6, 2, 3}, {2, 2, 2, 2, 2}, {6, 2, 3, 2, 6}, {0, 3, 6, 0, 0},
};

/* font texture, 4-bit: the 5x7 glyphs in 8x8 cells (16 a row, 6 rows),
 * then the 3x5 ones in 4x6 cells (32 a row, 3 rows): 128x66 texels */
#define SMALL_Y 48
#define FONT_VX 960
#define FONT_VY 256
#define FONT_CLUT_X 0
#define FONT_CLUT_Y 496
static uint16_t g_font_tpage, g_font_clut;

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static float rdf(const uint8_t *p)
{
	const uint32_t u = rd32(p);
	float f;
	memcpy(&f, &u, 4);
	return f;
}

/* ------------------------------------------------------------ VRAM */
/* Framebuffers: 320x240 at (0,0) and (0,240). Textures go in the pages to the
 * right (x 320..959), the font at x 960, palettes in rows 480..511 under the
 * framebuffers. Each texture sits at the top-left of a run of pages, stacked
 * down a page column until it is full. */
#define TEX_X0 320
#define TEX_X1 960
#define PAGE_W 64
static uint16_t g_page_used[2][16];
static int g_clut_x, g_clut_y;

static void vram_reset(void)
{
	memset(g_page_used, 0, sizeof(g_page_used));
	g_clut_x = 0;
	g_clut_y = 480;
}

static int vram_place(int vw, int h, int *x, int *y, int align)
{
	const int pages = (vw + PAGE_W - 1) / PAGE_W;
	for (int row = 0; row < 2; row++) {
		for (int p = TEX_X0 / PAGE_W; p + pages <= TEX_X1 / PAGE_W; p++) {
			int top = 0;
			for (int k = 0; k < pages; k++) {
				if (g_page_used[row][p + k] > top) {
					top = g_page_used[row][p + k];
				}
			}
			/* a texture window repeats from a multiple of its own size */
			if (align > 1) {
				top = (top + align - 1) / align * align;
			}
			if (top + h > 256) {
				continue;
			}
			for (int k = 0; k < pages; k++) {
				g_page_used[row][p + k] = (uint16_t)(top + h);
			}
			*x = p * PAGE_W;
			*y = row * 256 + top;
			return 1;
		}
	}
	return 0;
}

static int clut_place(int n, int *x, int *y)
{
	const int w = n > 16 ? 256 : 16;
	if (g_clut_x + w > 320) {
		g_clut_x = 0;
		g_clut_y++;
	}
	if (g_clut_y >= 496) {
		return 0;
	}
	*x = g_clut_x;
	*y = g_clut_y;
	g_clut_x += w;
	return 1;
}

static void upload(int x, int y, int w, int h, const void *data)
{
	RECT r;
	setRECT(&r, x, y, w, h);
	LoadImage(&r, (const uint32_t *)data);
	DrawSync(0);
}

/* P1TX: "P1TX", u16 abi, u16 count, then per texture u16 w, h, u8 psm
 * (0 T4, 1 T8, 2 CT16), u8 flags, u16 clut entries, u32 data bytes, data,
 * clut (RGB555, PS1 colours). */
static void parse_textures(const uint8_t *b, unsigned sz)
{
	g_tex_n = 0;
	memset(g_tex, 0, sizeof(g_tex));
	vram_reset();
	if (!b || sz < 8 || memcmp(b, "P1TX", 4) != 0) {
		return;
	}
	const unsigned n = rd16(b + 6);
	unsigned off = 8;
	for (unsigned i = 0; i < n && i < MAX_TEX && off + 12 <= sz; i++) {
		Tex *t = &g_tex[i];
		t->w = rd16(b + off);
		t->h = rd16(b + off + 2);
		t->psm = b[off + 4];
		t->tiled = (b[off + 5] & 1) != 0;
		const unsigned clut_n = rd16(b + off + 6);
		const unsigned data_n = rd32(b + off + 8);
		off += 12;
		const uint8_t *data = b + off;
		off += data_n;
		const uint8_t *clut = b + off;
		off += clut_n * 2;
		g_tex_n = (int)i + 1;
		if (off > sz || t->w <= 0 || t->h <= 0 || t->w > 256 || t->h > 256) {
			continue;
		}
		const int mode = t->psm == 0 ? 0 : (t->psm == 1 ? 1 : 2);
		const int vw = mode == 0 ? t->w / 4 : (mode == 1 ? t->w / 2 : t->w);
		int x = 0, y = 0;
		if (vw <= 0 || !vram_place(vw, t->h, &x, &y, t->tiled ? t->h : 1)) {
			printf("P1: no VRAM for texture %u (%dx%d)\n", i, t->w, t->h);
			continue;
		}
		/* LoadImage wants 4-byte aligned data */
		void *tmp = malloc(data_n + 4);
		if (!tmp) {
			continue;
		}
		memcpy(tmp, data, data_n);
		upload(x, y, vw, t->h, tmp);
		free(tmp);
		t->tpage = getTPage(mode, 0, x, y);
		t->u0 = 0;
		t->v0 = (uint8_t)(y & 255);
		{
			/* window: texcoord = (u & ~mask) | (offset & mask), in 8-texel steps */
			const uint32_t mx = (uint32_t)((256 - t->w) >> 3) & 0x1f;
			const uint32_t my = (uint32_t)((256 - t->h) >> 3) & 0x1f;
			const uint32_t ox = (uint32_t)(t->u0 >> 3) & 0x1f;
			const uint32_t oy = (uint32_t)(t->v0 >> 3) & 0x1f;
			t->twin = 0xe2000000u | mx | (my << 5) | (ox << 10) | (oy << 15);
		}
		if (mode < 2 && clut_n > 0) {
			int cx = 0, cy = 0;
			const int cells = mode == 0 ? 16 : 256;
			uint16_t *pal = (uint16_t *)malloc(cells * 2);
			if (pal && clut_place(cells, &cx, &cy)) {
				memset(pal, 0, cells * 2);
				memcpy(pal, clut, (clut_n < (unsigned)cells ? clut_n : (unsigned)cells) * 2);
				upload(cx, cy, cells, 1, pal);
				t->clut = getClut(cx, cy);
			}
			free(pal);
		}
		t->ready = 1;
	}
}

static void upload_font(void)
{
	/* 128x66 texels at 4 bits: 32x66 VRAM pixels */
	static uint16_t px[32 * 66];
	memset(px, 0, sizeof(px));
	for (int c = 0; c < 95; c++) {
		const int cx = (c % 16) * 8;
		const int cy = (c / 16) * 8;
		for (int row = 0; row < 7; row++) {
			const unsigned bits = kFont5x7[c][row];
			for (int col = 0; col < 5; col++) {
				if (!(bits & (1u << (4 - col)))) {
					continue;
				}
				const int tx = cx + col;
				const int ty = cy + row;
				px[ty * 32 + tx / 4] |= (uint16_t)(1u << ((tx & 3) * 4));
			}
		}
	}
	for (int c = 0; c < 95; c++) {
		const int cx = (c % 32) * 4;
		const int cy = SMALL_Y + (c / 32) * 6;
		for (int row = 0; row < 5; row++) {
			const unsigned bits = kFont3x5[c][row];
			for (int col = 0; col < 3; col++) {
				if (!(bits & (1u << (2 - col)))) {
					continue;
				}
				const int tx = cx + col;
				const int ty = cy + row;
				px[ty * 32 + tx / 4] |= (uint16_t)(1u << ((tx & 3) * 4));
			}
		}
	}
	upload(FONT_VX, FONT_VY, 32, 66, px);
	static uint16_t pal[16];
	pal[0] = 0;
	for (int i = 1; i < 16; i++) {
		pal[i] = 0x7fff;
	}
	upload(FONT_CLUT_X, FONT_CLUT_Y, 16, 1, pal);
	g_font_tpage = getTPage(0, 0, FONT_VX, FONT_VY);
	g_font_clut = getClut(FONT_CLUT_X, FONT_CLUT_Y);
}

/* ------------------------------------------------------------ mesh */
/* P1MS v2 (gdirpack.go): 32-byte header, verts (i16 x y z pad, u8 r g b pad: 12 bytes),
 * tris (u16 a b c, u8 u0 v0 u1 v1 u2 v2, u8 tex, u8 flags, pad: 16 bytes), chunks (u32
 * first, u16 n, u16 node, f32 cx cy cz r), then the PS2 MESH trailer
 * (INST / NFOG). */
static void parse_extras(const uint8_t *b, unsigned sz)
{
	g_inst = 0;
	g_inst_n = 0;
	memset(g_isrc, 0, sizeof(g_isrc));
	memset(g_nfog, 0, sizeof(g_nfog));
	unsigned off = 0;
	while (off + 8 <= sz) {
		const uint8_t *h = b + off;
		const unsigned n = rd32(h + 4);
		off += 8;
		if (memcmp(h, "INST", 4) == 0) {
			if (off + n * 52 > sz) {
				break;
			}
			for (unsigned i = 0; i < n; i++) {
				const unsigned node = rd16(b + off + i * 52);
				if (node < MAX_NODES) {
					g_isrc[node] = 1;
				}
			}
			if (!g_inst) {
				g_inst = b + off;
				g_inst_n = n;
			}
			off += n * 52;
		} else if (memcmp(h, "NFOG", 4) == 0) {
			if (off + n * 2 > sz) {
				break;
			}
			for (unsigned i = 0; i < n; i++) {
				const unsigned node = rd16(b + off + i * 2);
				if (node < MAX_NODES) {
					g_nfog[node] = 1;
				}
			}
			off += (n * 2 + 3) & ~3u;
		} else {
			break;
		}
	}
	for (unsigned ci = 0; ci < g_chunk_n; ci++) {
		const unsigned nd = rd16(g_chunks + ci * 24 + 6);
		if (nd >= MAX_NODES || !g_isrc[nd]) {
			continue;
		}
		if (g_isrc[nd] == 1) {
			g_isrc_c0[nd] = (uint16_t)ci;
			g_isrc[nd] = 2;
		}
		g_isrc_c1[nd] = (uint16_t)(ci + 1);
	}
}

static int parse_mesh(const uint8_t *b, unsigned sz)
{
	if (!b || sz < 32 || memcmp(b, "P1MS", 4) != 0 || rd16(b + 4) != 2 || ((uintptr_t)b & 3)) {
		return 0;
	}
	g_scale = rd16(b + 6);
	if (g_scale <= 0) {
		g_scale = 1;
	}
	g_vert_n = rd32(b + 8);
	g_tri_n = rd32(b + 12);
	g_chunk_n = rd32(b + 16);
	const unsigned extras = rd32(b + 20);
	unsigned off = 32;
	g_verts = (const P1Vert *)(b + off);
	off += g_vert_n * sizeof(P1Vert);
	g_tris = (const P1Tri *)(b + off);
	off += g_tri_n * sizeof(P1Tri);
	off = (off + 3) & ~3u;
	g_chunks = b + off;
	off += g_chunk_n * 24;
	if (off + extras > sz) {
		return 0;
	}
	g_mesh = b;
	g_mesh_sz = sz;
	free(g_cint);
	g_cint = (ChunkI *)malloc(sizeof(ChunkI) * (g_chunk_n ? g_chunk_n : 1));
	if (!g_cint) {
		return 0;
	}
	for (unsigned ci = 0; ci < g_chunk_n; ci++) {
		const uint8_t *ch = g_chunks + ci * 24;
		const float sc = (float)g_scale;
		g_cint[ci].c.vx = (int16_t)(rdf(ch + 8) * sc);
		g_cint[ci].c.vy = (int16_t)(rdf(ch + 12) * sc);
		g_cint[ci].c.vz = (int16_t)(rdf(ch + 16) * sc);
		g_cint[ci].c.pad = 0;
		const float r = rdf(ch + 20);
		g_cint[ci].r = r < 0.0f ? -1 : (int32_t)(r * sc) + 1;
	}
	parse_extras(b + off, extras);
	return 1;
}

static void parse_camera(const uint8_t *b, unsigned sz)
{
	g_cam_x = 0.0f;
	g_cam_y = 3.0f;
	g_cam_z = 8.0f;
	g_look_x = 0.0f;
	g_look_y = 1.5f;
	g_look_z = 0.0f;
	g_fov = 55.0f;
	g_pitch = g_yaw = g_roll = 0.0f;
	g_attach_x = g_attach_y = g_attach_z = 0.0f;
	g_shake_x = g_shake_y = g_shake_z = 0.0f;
	if (!b || sz < 8 || memcmp(b, "NODE", 4) != 0) {
		return;
	}
	const unsigned count = rd16(b + 6);
	const unsigned rec = (8 + count * 138u <= sz) ? 138u : 74u;
	if (8 + count * rec > sz) {
		return;
	}
	for (unsigned i = 0; i < count; i++) {
		const uint8_t *n = b + 8 + i * rec;
		if (n[4] == 3) {
			g_cam_x = rdf(n + 6);
			g_cam_y = rdf(n + 10);
			g_cam_z = rdf(n + 14);
			g_pitch = rdf(n + 18);
			g_yaw = rdf(n + 22);
			g_roll = rdf(n + 26);
			const float cx = cosf(g_pitch), sx = sinf(g_pitch);
			const float cy = cosf(g_yaw), sy = sinf(g_yaw);
			const float fz = -cx;
			g_look_x = g_cam_x + sy * fz * 8.0f;
			g_look_y = g_cam_y + sx * 8.0f;
			g_look_z = g_cam_z + cy * fz * 8.0f;
			return;
		}
	}
}

/* ------------------------------------------------------------ boot */
void gs_draw_boot(int pal)
{
	g_pal = pal;
	ResetGraph(0);
	SetVideoMode(pal ? MODE_PAL : MODE_NTSC);
	for (int i = 0; i < 2; i++) {
		SetDefDispEnv(&g_fb[i].disp, 0, i == 0 ? SCR_H : 0, SCR_W, SCR_H);
		SetDefDrawEnv(&g_fb[i].draw, 0, i == 0 ? 0 : SCR_H, SCR_W, SCR_H);
		if (pal) {
			g_fb[i].disp.screen.y = 24;
		}
		g_fb[i].draw.isbg = 1;
		g_fb[i].draw.dtd = 1;
		setRGB0(&g_fb[i].draw, 0, 0, 0);
	}
	g_cur = 0;
	PutDispEnv(&g_fb[0].disp);
	PutDrawEnv(&g_fb[0].draw);
	InitGeom();
	gte_SetGeomOffset(SCR_W / 2, SCR_H / 2);
	upload_font();
	SetDispMask(1);
}

/* ------------------------------------------------------------ matrices */
struct M34 {
	float m[12]; /* row-major 3x4 */
};

static void m34_mul(const float *a, const float *b, float *o)
{
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 4; c++) {
			float v = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] + a[r * 4 + 2] * b[2 * 4 + c];
			if (c == 3) {
				v += a[r * 4 + 3];
			}
			o[r * 4 + c] = v;
		}
	}
}

static void eye_now(float *x, float *y, float *z)
{
	*x = g_cam_x + g_attach_x + g_shake_x;
	*y = g_cam_y + g_attach_y + g_shake_y + g_eye_h;
	*z = g_cam_z + g_attach_z + g_shake_z;
}

/* view: world (Y up) to the GTE's camera space (X right, Y down, Z ahead) */
static void view_matrix(float *v)
{
	float ex, ey, ez;
	eye_now(&ex, &ey, &ez);
	float zx = ex - g_look_x, zy = ey - g_look_y, zz = ez - g_look_z;
	float zl = sqrtf(zx * zx + zy * zy + zz * zz);
	if (zl < 1e-5f) {
		zl = 1.0f;
	}
	zx /= zl;
	zy /= zl;
	zz /= zl;
	float xx = zz, xy = 0.0f, xz = -zx;
	float xl = sqrtf(xx * xx + xz * xz);
	if (xl < 1e-5f) {
		xx = 1.0f;
		xz = 0.0f;
		xl = 1.0f;
	}
	xx /= xl;
	xz /= xl;
	const float yx = zy * xz - zz * xy;
	const float yy = zz * xx - zx * xz;
	const float yz = zx * xy - zy * xx;
	/* rows: x, -y, -z */
	v[0] = xx;
	v[1] = xy;
	v[2] = xz;
	v[3] = -(xx * ex + xy * ey + xz * ez);
	v[4] = -yx;
	v[5] = -yy;
	v[6] = -yz;
	v[7] = (yx * ex + yy * ey + yz * ez);
	v[8] = -zx;
	v[9] = -zy;
	v[10] = -zz;
	v[11] = (zx * ex + zy * ey + zz * ez);
	if (g_world_yaw != 0.0f) {
		const float c = cosf(g_world_yaw), s = sinf(g_world_yaw);
		const float w[12] = { c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0 };
		float o[12];
		m34_mul(v, w, o);
		memcpy(v, o, sizeof(o));
	}
}

static int16_t fx12(float f)
{
	float v = f * 4096.0f;
	if (v > 32767.0f) {
		v = 32767.0f;
	}
	if (v < -32768.0f) {
		v = -32768.0f;
	}
	return (int16_t)v;
}

static void to_gte(const float *mv, MATRIX *o)
{
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++) {
			o->m[r][c] = fx12(mv[r * 4 + c]);
		}
		o->t[r] = (int32_t)(mv[r * 4 + 3] * (float)g_scale);
	}
}

/* ------------------------------------------------------------ drawing */
struct DrawCtx {
	float h; /* projection distance in pixels */
	float tan_x, tan_y;
	float far_m;
	int ot_shift;
	unsigned tris;
	unsigned tested;
	/* integer culling, in mesh units with 12-bit tangents */
	int32_t near_u, far_u, tanx_fx, tany_fx;
};

static void *alloc_prim(unsigned n)
{
	if (g_pp + n > g_pend) {
		return 0;
	}
	void *p = g_pp;
	g_pp += n;
	return p;
}

/* The chunk's centre through the matrix already in the GTE (rotate and
 * translate, no projection): view-space units, tested against the frustum
 * grown by the radius (times sc for scaled instances, 12-bit). */
static int chunk_visible(const DrawCtx *c, unsigned ci, int32_t sc_fx)
{
	const ChunkI *k = &g_cint[ci];
	if (k->r < 0) {
		return 0;
	}
	const int32_t r = (int32_t)(((int64_t)k->r * sc_fx) >> 12);
	VECTOR v;
	gte_ldv0(&k->c);
	gte_rt();
	gte_stlvnl(&v);
	if (v.vz + r < c->near_u || v.vz - r > c->far_u) {
		return 0;
	}
	const int32_t zr = v.vz + r;
	const int32_t lim_x = (int32_t)(((int64_t)zr * c->tanx_fx) >> 12) + r;
	const int32_t lim_y = (int32_t)(((int64_t)zr * c->tany_fx) >> 12) + r;
	if (v.vx > lim_x || -v.vx > lim_x || v.vy > lim_y || -v.vy > lim_y) {
		return 0;
	}
	return 1;
}

/* Each OT bin's last primitive: triangles go in after it, so a bin draws in
 * mesh order. Layered UI quads a centimetre apart share a bin, and the cook
 * writes them back to front (the PS2 sorts them with its Z buffer). */
static void *g_tail[OT_LEN];

static void tails_reset(uint32_t *ot)
{
	for (int i = 0; i < OT_LEN; i++) {
		g_tail[i] = ot + i;
	}
}

/* the chain first..last (already linked) after bin idx's tail */
static inline void ot_append(int idx, void *first, void *last)
{
	void *t = g_tail[idx];
	setaddr(last, getaddr(t));
	setaddr(t, first);
	g_tail[idx] = last;
}

static void draw_tris(DrawCtx *c, unsigned first, unsigned n)
{
	const int32_t near_z = c->near_u;
	const int ot_shift = c->ot_shift;
	const P1Vert *const verts = g_verts;
	const unsigned vn = g_vert_n;
	unsigned end = first + n;
	if (end > g_tri_n) {
		end = g_tri_n;
	}
	c->tested += n;
	unsigned drawn = 0;
	for (const P1Tri *tr = g_tris + first, *te = g_tris + end; tr < te; tr++) {
		const unsigned ia = tr->a, ib = tr->b, ic = tr->c;
		if ((tr->flags & 1) || ia >= vn || ib >= vn || ic >= vn) {
			continue;
		}
		const P1Vert *va = verts + ia;
		const P1Vert *vb = verts + ib;
		const P1Vert *vc = verts + ic;
		gte_ldv3(&va->p, &vb->p, &vc->p);
		gte_rtpt();
		int32_t nclip;
		gte_nclip();
		gte_stopz(&nclip);
		if (nclip <= 0) {
			continue; /* back face or edge on */
		}
		int32_t z0, z1, z2;
		gte_stsz3(&z0, &z1, &z2);
		if (z0 < near_z || z1 < near_z || z2 < near_z) {
			continue;
		}
		int32_t otz;
		gte_avsz3();
		gte_stotz(&otz);
		int idx = otz >> ot_shift;
		if (idx >= OT_LEN) {
			continue;
		}
		if (idx < 1) {
			idx = 1;
		}
		const unsigned tex = tr->tex;
		if (tex < (unsigned)g_tex_n && g_tex[tex].ready) {
			const Tex *tx = &g_tex[tex];
			const unsigned need = sizeof(POLY_GT3) + (tx->tiled ? 2 * sizeof(DR_TWIN) : 0);
			if (g_pp + need > g_pend) {
				break;
			}
			POLY_GT3 *p = (POLY_GT3 *)g_pp;
			g_pp += sizeof(POLY_GT3);
			setPolyGT3(p);
			gte_stsxy3(&p->x0, &p->x1, &p->x2);
			setRGB0(p, va->r, va->g, va->b);
			setRGB1(p, vb->r, vb->g, vb->b);
			setRGB2(p, vc->r, vc->g, vc->b);
			const uint8_t *uv = tr->uv;
			const int u0 = tx->u0, v0 = tx->v0;
			setUV3(p, uv[0] + u0, uv[1] + v0, uv[2] + u0, uv[3] + v0, uv[4] + u0, uv[5] + v0);
			p->tpage = tx->tpage;
			p->clut = tx->clut;
			if (tx->tiled) {
				/* window on, the triangle, window off */
				DR_TWIN *on = (DR_TWIN *)g_pp;
				DR_TWIN *off = on + 1;
				g_pp += 2 * sizeof(DR_TWIN);
				setlen(off, 1);
				off->code[0] = 0xe2000000u;
				setlen(on, 1);
				on->code[0] = tx->twin;
				catPrim(on, p);
				catPrim(p, off);
				ot_append(idx, on, off);
			} else {
				ot_append(idx, p, p);
			}
		} else {
			if (g_pp + sizeof(POLY_G3) > g_pend) {
				break;
			}
			POLY_G3 *p = (POLY_G3 *)g_pp;
			g_pp += sizeof(POLY_G3);
			setPolyG3(p);
			gte_stsxy3(&p->x0, &p->x1, &p->x2);
			setRGB0(p, va->r, va->g, va->b);
			setRGB1(p, vb->r, vb->g, vb->b);
			setRGB2(p, vc->r, vc->g, vc->b);
			ot_append(idx, p, p);
		}
		drawn++;
	}
	c->tris += drawn;
}

static void draw_scene_mesh(DrawCtx *c, const float *view)
{
	int last_node = -2;
	float mv[12];
	for (unsigned ci = 0; ci < g_chunk_n; ci++) {
		const uint8_t *ch = g_chunks + ci * 24;
		const unsigned node = rd16(ch + 6);
		if (node < MAX_NODES && (g_isrc[node] || (g_nodes_live && g_nhide[node]))) {
			continue;
		}
		if ((int)node != last_node) {
			last_node = (int)node;
			if (node < MAX_NODES && g_nodes_live && g_nm_on[node]) {
				m34_mul(view, g_nm[node], mv);
			} else {
				memcpy(mv, view, sizeof(mv));
			}
			MATRIX m;
			to_gte(mv, &m);
			gte_SetRotMatrix(&m);
			gte_SetTransMatrix(&m);
		}
		if (!chunk_visible(c, ci, 4096)) {
			continue;
		}
		draw_tris(c, rd32(ch), rd16(ch + 4));
	}
	/* MultiMesh instances: each one draws its source node's chunks */
	for (unsigned i = 0; i < g_inst_n; i++) {
		const uint8_t *row = g_inst + i * 52;
		const unsigned nd = rd16(row);
		if (nd >= MAX_NODES || g_isrc[nd] != 2 || g_nhide[nd]) {
			continue;
		}
		float d[12], m[12];
		for (int k = 0; k < 12; k++) {
			d[k] = rdf(row + 4 + k * 4);
		}
		if (g_nm_on[nd]) {
			m34_mul(g_nm[nd], d, m);
		} else {
			memcpy(m, d, sizeof(m));
		}
		m34_mul(view, m, mv);
		float sc = 0.0f;
		for (int k = 0; k < 3; k++) {
			const float l = m[k] * m[k] + m[4 + k] * m[4 + k] + m[8 + k] * m[8 + k];
			if (l > sc) {
				sc = l;
			}
		}
		sc = sqrtf(sc);
		MATRIX gm;
		to_gte(mv, &gm);
		gte_SetRotMatrix(&gm);
		gte_SetTransMatrix(&gm);
		for (unsigned ci = g_isrc_c0[nd]; ci < g_isrc_c1[nd]; ci++) {
			const uint8_t *ch = g_chunks + ci * 24;
			if (chunk_visible(c, ci, (int32_t)(sc * 4096.0f))) {
				draw_tris(c, rd32(ch), rd16(ch + 4));
			}
		}
	}
}

/* ------------------------------------------------------------ HUD */
static int hx(float x)
{
	return (int)(x * ((float)SCR_W / HUD_W));
}

static int hy(float y)
{
	/* full-height panels still reach the screen's edges */
	if (y <= 0.0f) {
		return 0;
	}
	if (y >= HUD_H) {
		return SCR_H;
	}
	return SAFE_Y + (int)(y * ((float)(SCR_H - 2 * SAFE_Y) / HUD_H));
}

static void hud_rect(uint32_t *ot, int x0, int y0, int x1, int y1, int r, int g, int b)
{
	if (x1 <= x0 || y1 <= y0) {
		return;
	}
	TILE *t = (TILE *)alloc_prim(sizeof(TILE));
	if (!t) {
		return;
	}
	setTile(t);
	setXY0(t, x0, y0);
	setWH(t, x1 - x0, y1 - y0);
	setRGB0(t, r, g, b);
	addPrim(ot, t);
}

/* one glyph, px screen pixels per font pixel; px 0 is the 3x5 font */
static void hud_glyph(uint32_t *ot, int x, int y, int ch, int r, int g, int b, int px)
{
	if (ch < 32 || ch > 126) {
		ch = '?';
	}
	const int c = ch - 32;
	int u = (c % 16) * 8, v = (c / 16) * 8, gw = 5, gh = 7;
	if (px == 0) {
		u = (c % 32) * 4;
		v = SMALL_Y + (c / 32) * 6;
		gw = 3;
		gh = 5;
		px = 1;
	}
	POLY_FT4 *p = (POLY_FT4 *)alloc_prim(sizeof(POLY_FT4));
	if (!p) {
		return;
	}
	setPolyFT4(p);
	const int w = gw * px, h = gh * px;
	setXY4(p, x, y, x + w, y, x, y + h, x + w, y + h);
	setUV4(p, u, v, u + gw, v, u, v + gh, u + gw, v + gh);
	/* texel 31 x colour / 128: the colour's half is full brightness */
	setRGB0(p, r >> 1, g >> 1, b >> 1);
	p->tpage = g_font_tpage;
	p->clut = g_font_clut;
	addPrim(ot, p);
}

/* A slot's text, laid out in the PS2's 640x448 space and drawn at 320x240.
 * Glyphs are half the PS2 size (at least one pixel per font pixel). */
static void hud_text(uint32_t *ot, const HudQuad *h, int r, int g, int b)
{
	const int sc = h->scale > 0 ? h->scale : 1;
	const int px = sc / 2; /* 0: the 3x5 font */
	const int adv = px ? 6 * px : 4; /* screen pixels */
	const char *t = h->text;
	int lines = 1;
	for (const char *p = t; *p; p++) {
		if (*p == '\n') {
			lines++;
		}
	}
	const int bx = hx((float)h->x);
	const int bw = hx((float)h->w);
	/* lines sit where the PS2 puts them (9 px a scale step at 448 lines), so
	 * text lines up with the 3D parts the cook placed beside it */
	const int line_ps2 = 9 * sc;
	int y_ps2 = h->y + 2;
	if (h->styled && h->kind != 1) {
		y_ps2 = h->y + (h->h - (lines * line_ps2 - 2 * sc)) / 2;
	}
	/* 3x5 text sits a line up: its 5 rows are taller than the PS2's 9-line
	 * pitch at half scale, and it must clear outlines drawn round it */
	const int lift = px ? 0 : 1;
	const int min_pitch = px ? 8 * px : 6;
	int last_y = -1000;
	while (*t) {
		int n = 0;
		while (t[n] && t[n] != '\n') {
			n++;
		}
		/* lines keep the PS2's place, but never closer than the font needs */
		int ly = hy((float)y_ps2) - lift;
		if (n > 0 && ly < last_y + min_pitch) {
			ly = last_y + min_pitch;
		}
		if (n > 0) {
			last_y = ly;
		}
		const int width = n * adv - (px ? px : 1);
		int x = bx + 1;
		if (h->align == 1) {
			x = bx + (bw - width) / 2;
		} else if (h->align == 2) {
			x = bx + bw - width - 1;
		}
		for (int k = 0; k < n; k++) {
			if (t[k] != ' ') {
				hud_glyph(ot, x + k * adv, ly, (unsigned char)t[k], r, g, b, px);
			}
		}
		t += n;
		if (*t == '\n') {
			t++;
		}
		y_ps2 += line_ps2;
	}
}

static void draw_hud(void)
{
	/* ot[0] is drawn last, and addPrim puts a primitive in front of the
	 * ones already there: add the top-most things first */
	uint32_t *ot = g_fb[g_cur].ot;
	if (g_fade_a > 0) {
		/* the GPU's blends are fixed: darken by subtraction, brighten by addition */
		const int a = g_fade_a > 128 ? 128 : g_fade_a;
		const int dark = g_fade_r + g_fade_g + g_fade_b < 384;
		const int k = dark ? 255 : 1;
		TILE *t = (TILE *)alloc_prim(sizeof(TILE));
		DR_TPAGE *tp = (DR_TPAGE *)alloc_prim(sizeof(DR_TPAGE));
		if (t && tp) {
			setTile(t);
			setSemiTrans(t, 1);
			setXY0(t, 0, 0);
			setWH(t, SCR_W, SCR_H);
			if (dark) {
				setRGB0(t, k * a / 128, k * a / 128, k * a / 128);
			} else {
				setRGB0(t, g_fade_r * a / 128, g_fade_g * a / 128, g_fade_b * a / 128);
			}
			addPrim(ot, t);
			setDrawTPage(tp, 0, 1, getTPage(0, dark ? 2 : 1, 0, 0));
			addPrim(ot, tp);
		}
	}
	for (int i = 15; i >= 0; i--) {
		const HudQuad *h = &g_hudq[i];
		if (!h->used || h->hidden) {
			continue;
		}
		const int x0 = hx((float)h->x), y0 = hy((float)h->y);
		const int x1 = hx((float)(h->x + h->w)), y1 = hy((float)(h->y + h->h));
		if (h->text[0]) {
			if (h->styled) {
				hud_text(ot, h, h->tr, h->tg, h->tb);
			} else {
				hud_text(ot, h, 255, 255, 255);
			}
		}
		if (!h->styled || h->kind != 1) {
			hud_rect(ot, x0, y0, x1, y1, h->r, h->g, h->b);
		}
	}
}

/* ------------------------------------------------------------ frame */
void gs_draw_scene(void)
{
	Frame *f = &g_fb[g_cur];
	static int s_tinit;
	if (!s_tinit) {
		TIMER_CTRL(1) = 0x0100; /* hblank clock */
		s_tinit = 1;
	}
	const uint16_t h0 = (uint16_t)TIMER_VALUE(1);
	ClearOTagR(f->ot, OT_LEN);
	tails_reset(f->ot);
	g_pp = f->pri;
	g_pend = f->pri + PRIM_BYTES;
	unsigned tris = 0;
	if (g_ready) {
		DrawCtx c;
		memset(&c, 0, sizeof(c));
		const float half = tanf(g_fov * 0.5f * 3.14159265f / 180.0f);
		const float half_px = (float)SCR_H * 0.5f - (float)SAFE_Y;
		c.h = half_px / (half > 0.01f ? half : 0.01f);
		/* the whole 320x240 is drawn: cull against all of it */
		c.tan_y = half * ((float)SCR_H * 0.5f) / half_px;
		c.tan_x = half * ((float)SCR_W * 0.5f) / half_px;
		c.far_m = g_fog_on ? g_fog_far : 400.0f;
		const float max_far = 60000.0f / (float)g_scale;
		if (c.far_m > max_far) {
			c.far_m = max_far;
		}
		/* average-depth OT index: otz = sz / 4, spread over OT_LEN */
		const int otz_max = (int)(c.far_m * (float)g_scale) / 4;
		c.ot_shift = 0;
		while ((otz_max >> c.ot_shift) >= OT_LEN && c.ot_shift < 16) {
			c.ot_shift++;
		}
		gte_SetGeomScreen((int)c.h);
		c.near_u = (int32_t)(NEAR_M * (float)g_scale);
		c.far_u = (int32_t)(c.far_m * (float)g_scale);
		c.tanx_fx = (int32_t)(c.tan_x * 4096.0f);
		c.tany_fx = (int32_t)(c.tan_y * 4096.0f);
		float view[12];
		view_matrix(view);
		draw_scene_mesh(&c, view);
		tris = c.tris;
		static unsigned frames;
		if ((frames++ % 120) == 0) {
			float ex, ey, ez;
			eye_now(&ex, &ey, &ez);
			printf("P1 frame: %u tris, prims %u bytes, eye %d %d %d look %d %d %d (cm), fov %d, scale %d, chunks %u\n", tris,
					(unsigned)(g_pp - f->pri), (int)(ex * 100), (int)(ey * 100), (int)(ez * 100), (int)(g_look_x * 100),
					(int)(g_look_y * 100), (int)(g_look_z * 100), (int)g_fov, g_scale, g_chunk_n);
		}
	}
	(void)tris;
	const uint16_t h1 = (uint16_t)TIMER_VALUE(1);
	draw_hud();
	const uint16_t h2 = (uint16_t)TIMER_VALUE(1);
	/* the last frame's drawing done and shown: start this one's */
	DrawSync(0);
	const uint16_t h3 = (uint16_t)TIMER_VALUE(1);
	static unsigned s_hs, s_hh, s_hg, s_hn;
	s_hs += (uint16_t)(h1 - h0);
	s_hh += (uint16_t)(h2 - h1);
	s_hg += (uint16_t)(h3 - h2);
	if (++s_hn == 120) {
		printf("P1 hblanks/frame: scene %u hud %u gpu wait %u\n", s_hs / 120, s_hh / 120, s_hg / 120);
		s_hs = s_hh = s_hg = s_hn = 0;
	}
	VSync(0);
	PutDispEnv(&f->disp);
	setRGB0(&f->draw, g_clear_r, g_clear_g, g_clear_b);
	PutDrawEnv(&f->draw);
	DrawOTag(f->ot + OT_LEN - 1);
	g_cur ^= 1;
}

/* ------------------------------------------------------------ API */
static unsigned char *(*g_gtex_reload)(const unsigned char *, unsigned *);

void gs_draw_set_gtex_reload(unsigned char *(*fn)(const unsigned char *mesh, unsigned *size))
{
	g_gtex_reload = fn;
}

int gs_draw_init(const unsigned char *mesh, unsigned mesh_sz, const unsigned char *gtex, unsigned gtex_sz,
		const unsigned char *node, unsigned node_sz)
{
	g_ready = 0;
	g_fade_a = g_fade_r = g_fade_g = g_fade_b = 0;
	memset(g_hudq, 0, sizeof(g_hudq));
	memset(g_nofs, 0, sizeof(g_nofs));
	memset(g_nrot, 0, sizeof(g_nrot));
	DrawSync(0);
	parse_camera(node, node_sz);
	if (!gtex && g_gtex_reload) {
		/* back to pack 0: its textures left RAM after the first upload */
		unsigned sz = 0;
		unsigned char *again = g_gtex_reload(mesh, &sz);
		parse_textures(again, sz);
		free(again);
	} else {
		parse_textures(gtex, gtex_sz);
	}
	if (!parse_mesh(mesh, mesh_sz)) {
		printf("P1: mesh did not parse\n");
		return 0;
	}
	g_ready = 1;
	return 1;
}

int gs_draw_layer_add(const unsigned char *mesh, unsigned mesh_sz)
{
	(void)mesh;
	(void)mesh_sz;
	return 0;
}

void gs_draw_layer_remove(const unsigned char *mesh)
{
	(void)mesh;
}

int gs_draw_ready(void)
{
	return g_ready;
}

void gs_draw_orbit(float yaw, float dolly)
{
	(void)yaw;
	(void)dolly;
}

void gs_draw_set_world_yaw(float yaw)
{
	g_world_yaw = yaw;
}

void gs_draw_nudge(float dx, float dz)
{
	gs_draw_nudge3(dx, 0.0f, dz);
}

void gs_draw_nudge3(float dx, float dy, float dz)
{
	g_look_x += dx;
	g_look_y += dy;
	g_look_z += dz;
	g_cam_x += dx;
	g_cam_y += dy;
	g_cam_z += dz;
}

void gs_draw_look_point(float *x, float *y, float *z)
{
	if (x) {
		*x = g_look_x;
	}
	if (y) {
		*y = g_look_y;
	}
	if (z) {
		*z = g_look_z;
	}
}

void gs_draw_set_eye(float x, float y, float z)
{
	g_cam_x = x;
	g_cam_y = y;
	g_cam_z = z;
}

static void euler_yxz_negz(float pitch, float yaw, float roll, float *fx, float *fy, float *fz)
{
	const float cz = cosf(roll), sz = sinf(roll);
	const float x1 = 0.0f * cz - 0.0f * sz;
	const float y1 = 0.0f * sz + 0.0f * cz;
	const float z1 = -1.0f;
	const float cx = cosf(pitch), sx = sinf(pitch);
	const float y2 = y1 * cx - z1 * sx;
	const float z2 = y1 * sx + z1 * cx;
	const float cy = cosf(yaw), sy = sinf(yaw);
	*fx = x1 * cy + z2 * sy;
	*fy = y2;
	*fz = -x1 * sy + z2 * cy;
}

static void apply_look_from_euler(void)
{
	float fx, fy, fz;
	euler_yxz_negz(g_pitch, g_yaw, g_roll, &fx, &fy, &fz);
	g_look_x = g_cam_x + fx * 8.0f;
	g_look_y = g_cam_y + fy * 8.0f;
	g_look_z = g_cam_z + fz * 8.0f;
}

void gs_draw_set_camera(float x, float y, float z, float pitch, float yaw, float roll, float fov_deg)
{
	g_cam_x = x;
	g_cam_y = y;
	g_cam_z = z;
	g_pitch = pitch;
	g_yaw = yaw;
	g_roll = roll;
	apply_look_from_euler();
	if (fov_deg > 1.0f && fov_deg < 170.0f) {
		g_fov = fov_deg;
	}
}

void gs_draw_set_ortho(int on)
{
	(void)on;
}

void gs_draw_set_eye_height(float h)
{
	g_eye_h = h;
}

void gs_draw_look(float yaw, float pitch, float roll)
{
	g_yaw = yaw;
	g_pitch = pitch;
	g_roll = roll;
	apply_look_from_euler();
}

void gs_draw_look_delta(float dyaw, float dpitch)
{
	g_yaw += dyaw;
	g_pitch += dpitch;
	if (g_pitch > 1.4f) {
		g_pitch = 1.4f;
	}
	if (g_pitch < -1.4f) {
		g_pitch = -1.4f;
	}
	apply_look_from_euler();
}

void gs_draw_orbit_sph(float yaw, float pitch, float dist)
{
	g_yaw = yaw;
	g_pitch = pitch;
	g_roll = 0.0f;
	dist = dist < 1.0f ? 1.0f : (dist > 80.0f ? 80.0f : dist);
	float fx, fy, fz;
	euler_yxz_negz(pitch, yaw, 0.0f, &fx, &fy, &fz);
	g_cam_x = g_look_x - fx * dist;
	g_cam_y = g_look_y - fy * dist;
	g_cam_z = g_look_z - fz * dist;
}

void gs_draw_attach_offset(float x, float y, float z)
{
	g_attach_x = x;
	g_attach_y = y;
	g_attach_z = z;
}

void gs_draw_shake(float x, float y, float z)
{
	g_shake_x = x;
	g_shake_y = y;
	g_shake_z = z;
}

void gs_draw_hud_quad(int slot, int x, int y, int w, int h, int r, int g, int b)
{
	if (slot < 0 || slot >= 16) {
		return;
	}
	HudQuad *q = &g_hudq[slot];
	q->used = 1;
	q->x = x;
	q->y = y;
	q->w = w > 0 ? w : 8;
	q->h = h > 0 ? h : 8;
	q->r = r;
	q->g = g;
	q->b = b;
}

void gs_draw_hud_text(int slot, int x, int y, int r, int g, int b, const char *str)
{
	if (slot < 0 || slot >= 16) {
		return;
	}
	HudQuad *q = &g_hudq[slot];
	q->used = 1;
	q->x = x;
	q->y = y;
	if (q->w < 8) {
		q->w = 96;
	}
	if (q->h < 8) {
		q->h = 16;
	}
	q->r = r;
	q->g = g;
	q->b = b;
	unsigned i = 0;
	while (str && str[i] && i < 64) {
		q->text[i] = str[i];
		i++;
	}
	q->text[i] = 0;
}

void gs_draw_clear_hud(void)
{
	memset(g_hudq, 0, sizeof(g_hudq));
}

void gs_draw_hud_style(int slot, int kind, int scale, int align, int hidden, int tr, int tg, int tb)
{
	if (slot < 0 || slot >= 16) {
		return;
	}
	HudQuad *q = &g_hudq[slot];
	q->styled = 1;
	q->kind = kind;
	q->scale = scale > 0 ? scale : 1;
	q->align = align;
	q->hidden = hidden ? 1 : 0;
	q->tr = tr;
	q->tg = tg;
	q->tb = tb;
}

/* 2D sprites and tile maps are not on the PS1 runtime yet */
void gs_draw_sprt(int slot, int x, int y, int w, int h, int tex, int flip)
{
	(void)slot;
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	(void)tex;
	(void)flip;
}

void gs_draw_clear_sprt(void) {}

void gs_draw_tile(int slot, int x, int y, int atlas)
{
	(void)slot;
	(void)x;
	(void)y;
	(void)atlas;
}

void gs_draw_clear_tiles(void) {}

void gs_draw_set_fade(int a, int r, int g, int b)
{
	g_fade_a = a;
	g_fade_r = r;
	g_fade_g = g;
	g_fade_b = b;
}

void gs_draw_set_node_ofs(int node, float x, float y, float z)
{
	if (node < 0 || node >= 32) {
		return;
	}
	g_nofs[node][0] = x;
	g_nofs[node][1] = y;
	g_nofs[node][2] = z;
}

void gs_draw_set_node_rot(int node, float rx, float ry, float rz)
{
	if (node < 0 || node >= 32) {
		return;
	}
	g_nrot[node][0] = rx;
	g_nrot[node][1] = ry;
	g_nrot[node][2] = rz;
}

void gs_draw_apply_node(int node, float *x, float *y, float *z)
{
	if (node < 0 || node >= 32) {
		return;
	}
	if (x) {
		*x += g_nofs[node][0];
	}
	if (y) {
		*y += g_nofs[node][1];
	}
	if (z) {
		*z += g_nofs[node][2];
	}
}

int gs_draw_primary_mesh_node(void)
{
	return 0;
}

void gs_draw_nodes_begin(void)
{
	g_nodes_live = 1;
}

void gs_draw_node_matrix(int node, const float *m)
{
	if (node < 0 || node >= MAX_NODES) {
		return;
	}
	if (m) {
		memcpy(g_nm[node], m, sizeof(g_nm[node]));
		g_nm_on[node] = 1;
	} else {
		g_nm_on[node] = 0;
	}
	g_nodes_live = 1;
}

void gs_draw_node_hidden(int node, int hidden)
{
	if (node >= 0 && node < MAX_NODES) {
		g_nhide[node] = hidden ? 1 : 0;
	}
}

void gs_draw_nodes_reset(void)
{
	g_fog_on = 0;
	memset(g_nm_on, 0, sizeof(g_nm_on));
	memset(g_nhide, 0, sizeof(g_nhide));
	g_nodes_live = 0;
}

void gs_draw_camera_lock(int on)
{
	g_cam_locked = on ? 1 : 0;
}

void gs_draw_camera_look(float ex, float ey, float ez, float tx, float ty, float tz, float fov_deg)
{
	g_cam_x = ex;
	g_cam_y = ey;
	g_cam_z = ez;
	g_look_x = tx;
	g_look_y = ty;
	g_look_z = tz;
	g_attach_x = g_attach_y = g_attach_z = 0.0f;
	g_eye_h = 0.0f;
	if (fov_deg > 1.0f && fov_deg < 170.0f) {
		g_fov = fov_deg;
	}
}

void gs_draw_set_clear(int r, int g, int b)
{
	g_clear_r = r;
	g_clear_g = g;
	g_clear_b = b;
}

void gs_draw_set_fog(int on, int r, int g, int b, float near_d, float far_d)
{
	g_fog_on = on && far_d > near_d;
	g_fog_r = r;
	g_fog_g = g;
	g_fog_b = b;
	g_fog_near = near_d;
	g_fog_far = far_d;
}

/* split screen is not on the PS1 runtime yet: always one view */
void gs_draw_set_views(int n)
{
	(void)n;
}

void gs_draw_view_look(int i, float ex, float ey, float ez, float tx, float ty, float tz, float fov_deg)
{
	(void)i;
	(void)ex;
	(void)ey;
	(void)ez;
	(void)tx;
	(void)ty;
	(void)tz;
	(void)fov_deg;
}

int gs_draw_view_count(void)
{
	return 0;
}

void gs_draw_use_vu1(int on)
{
	(void)on;
}

void gs_draw_camera(float *x, float *y, float *z)
{
	eye_now(x, y, z);
}

float gs_draw_world_yaw(void)
{
	return g_world_yaw;
}

void gs_draw_fill_mvp(float out[16], int width, int height)
{
	(void)width;
	(void)height;
	memset(out, 0, sizeof(float) * 16);
}

int gs_draw_tex_info(int i, int *vram, int *w, int *h, int *psm)
{
	if (i < 0 || i >= g_tex_n || !g_tex[i].ready) {
		return 0;
	}
	if (vram) {
		*vram = 0;
	}
	if (w) {
		*w = g_tex[i].w;
	}
	if (h) {
		*h = g_tex[i].h;
	}
	if (psm) {
		*psm = g_tex[i].psm;
	}
	return 1;
}
