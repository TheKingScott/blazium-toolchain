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
#define PRIM_BYTES (64 * 1024) /* ~1500 textured triangles a frame */
#define MAX_TEX 64
#ifdef GDIR_MAX_NODES
#define MAX_NODES GDIR_MAX_NODES /* the VM's node table */
#else
#define MAX_NODES 512
#endif
#define NEAR_M 2.0f /* clip plane: the race camera sees the road from ~4 m on; nearer splits it into many pieces */

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
static unsigned g_vert_n, g_prim_n, g_chunk_n;
/* P1MS v3 (internal/platforms/ps1/gdirpack.go): the vertex position is a GTE
 * SVECTOR; a primitive is a triangle (d 0xffff) or a quad a,b,c,d (the GPU
 * draws a,b,c and b,c,d) */
struct P1Vert {
	SVECTOR p;
	uint8_t r, g, b, pad;
};
struct P1Prim {
	uint16_t a, b, c, d;
	uint8_t uv[8];
	uint8_t tex, flags;
	uint16_t pad;
};
#define CHUNK_SZ 28 /* first u32, quads u16, node u16, sphere f32 x4, triangles u16, pad */
static const P1Vert *g_verts;
static const P1Prim *g_prims;
static const uint8_t *g_chunks;
/* each chunk's sphere in mesh units, for culling with the GTE */
struct ChunkI {
	SVECTOR c; /* pad holds nothing */
	int32_t r;
};
static ChunkI *g_cint;
/* runs of up to 16 neighbouring chunks of one node in one sphere: a run off
 * screen is skipped with one test (a track is ~1800 chunks) */
struct ChunkG {
	SVECTOR c;
	int32_t r;
	uint16_t first, n, node;
};
static ChunkG *g_grp;
static unsigned g_grp_n;
static uint8_t g_ndrawn[MAX_NODES]; /* the node has chunks (or is an instance source) */
/* a 3x4 transform in integers: rotation x scale in 4.12 (wider than the
 * GTE's 16 bits so scaled nodes fit), translation in mesh units */
struct FxM {
	int32_t m[9];
	int32_t t[3];
};
static const uint8_t *g_inst;
static unsigned g_inst_n;
static struct FxM *g_ifx;  /* each instance's transform, converted at load */
static int32_t *g_iscale;
/* per instance: its world transform (node x instance) and bounding sphere,
 * redone only when the node's matrix changes */
struct InstCache {
	struct FxM w;
	int32_t cx, cy, cz, r;
	uint32_t ver;
	uint8_t ok;
};
static InstCache *g_icache;
/* each instance source's chunks in one sphere, in its own mesh units */
static int32_t g_isph[MAX_NODES][4];
static uint8_t g_isrc[MAX_NODES];
static uint16_t g_isrc_c0[MAX_NODES], g_isrc_c1[MAX_NODES];
static uint8_t g_nfog[MAX_NODES];
static int g_ready;

/* node matrices, converted once when the VM sends them (not every frame:
 * the PS1 does float maths in software) */
static FxM g_nm[MAX_NODES];
static int32_t g_nscale[MAX_NODES]; /* largest axis scale, 4.12 */
static uint8_t g_nm_on[MAX_NODES];
static uint32_t g_nm_ver[MAX_NODES]; /* bumped on every change: instance caches */
static void fx_from_float(const float *f, FxM *o);
static void vc_clear(void);
static int32_t fx_max_scale(const FxM *f);
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
				free(g_ifx);
				free(g_iscale);
				free(g_icache);
				g_ifx = (FxM *)malloc(sizeof(FxM) * (n ? n : 1));
				g_iscale = (int32_t *)malloc(sizeof(int32_t) * (n ? n : 1));
				g_icache = (InstCache *)malloc(sizeof(InstCache) * (n ? n : 1));
				if (!g_ifx || !g_iscale || !g_icache) {
					g_inst_n = 0;
				}
				for (unsigned i = 0; i < g_inst_n; i++) {
					float d[12];
					for (int k = 0; k < 12; k++) {
						d[k] = rdf(g_inst + i * 52 + 4 + k * 4);
					}
					fx_from_float(d, &g_ifx[i]);
					g_iscale[i] = fx_max_scale(&g_ifx[i]);
					g_icache[i].ok = 0;
				}
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
		const unsigned nd = rd16(g_chunks + ci * CHUNK_SZ + 6);
		if (nd >= MAX_NODES || !g_isrc[nd]) {
			continue;
		}
		if (g_isrc[nd] == 1) {
			g_isrc_c0[nd] = (uint16_t)ci;
			g_isrc[nd] = 2;
		}
		g_isrc_c1[nd] = (uint16_t)(ci + 1);
	}
	/* a sphere round each instance source's chunks (the centre of their
	 * centres, out to the farthest edge) */
	for (unsigned nd = 0; nd < MAX_NODES; nd++) {
		if (g_isrc[nd] != 2) {
			continue;
		}
		int64_t sx = 0, sy = 0, sz = 0;
		const int cn = g_isrc_c1[nd] - g_isrc_c0[nd];
		for (unsigned ci = g_isrc_c0[nd]; ci < g_isrc_c1[nd]; ci++) {
			sx += g_cint[ci].c.vx;
			sy += g_cint[ci].c.vy;
			sz += g_cint[ci].c.vz;
		}
		const int32_t cx = cn ? (int32_t)(sx / cn) : 0, cy = cn ? (int32_t)(sy / cn) : 0, cz = cn ? (int32_t)(sz / cn) : 0;
		int32_t r = 0;
		for (unsigned ci = g_isrc_c0[nd]; ci < g_isrc_c1[nd]; ci++) {
			const int32_t dx = g_cint[ci].c.vx - cx, dy = g_cint[ci].c.vy - cy, dz = g_cint[ci].c.vz - cz;
			const int32_t d = (int32_t)sqrtf((float)dx * dx + (float)dy * dy + (float)dz * dz) + g_cint[ci].r;
			r = d > r ? d : r;
		}
		g_isph[nd][0] = cx;
		g_isph[nd][1] = cy;
		g_isph[nd][2] = cz;
		g_isph[nd][3] = r;
	}
}

/* the sphere round chunks first..first+n-1 (empty ones left out) */
static int32_t group_sphere(unsigned first, unsigned n, SVECTOR *c)
{
	int32_t sx = 0, sy = 0, sz = 0, cnt = 0;
	for (unsigned ci = first; ci < first + n; ci++) {
		if (g_cint[ci].r < 0) {
			continue;
		}
		sx += g_cint[ci].c.vx;
		sy += g_cint[ci].c.vy;
		sz += g_cint[ci].c.vz;
		cnt++;
	}
	if (!cnt) {
		c->vx = c->vy = c->vz = c->pad = 0;
		return -1;
	}
	c->vx = (int16_t)(sx / cnt);
	c->vy = (int16_t)(sy / cnt);
	c->vz = (int16_t)(sz / cnt);
	c->pad = 0;
	int32_t r = 0;
	for (unsigned ci = first; ci < first + n; ci++) {
		if (g_cint[ci].r < 0) {
			continue;
		}
		const float dx = (float)(g_cint[ci].c.vx - c->vx), dy = (float)(g_cint[ci].c.vy - c->vy),
					dz = (float)(g_cint[ci].c.vz - c->vz);
		const int32_t d = (int32_t)sqrtf(dx * dx + dy * dy + dz * dz) + g_cint[ci].r + 1;
		r = d > r ? d : r;
	}
	return r;
}

static void build_groups(void)
{
	free(g_grp);
	g_grp_n = 0;
	g_grp = (ChunkG *)malloc(sizeof(ChunkG) * (g_chunk_n ? g_chunk_n : 1));
	if (!g_grp) {
		return;
	}
	const int32_t max_r = 80 * g_scale; /* a run wider than ~160 m is split */
	unsigned ci = 0;
	while (ci < g_chunk_n) {
		const unsigned node = rd16(g_chunks + ci * CHUNK_SZ + 6);
		unsigned n = 1;
		while (n < 16 && ci + n < g_chunk_n && rd16(g_chunks + (ci + n) * CHUNK_SZ + 6) == node) {
			n++;
		}
		ChunkG *g = &g_grp[g_grp_n];
		int32_t r = group_sphere(ci, n, &g->c);
		while (n > 1 && r > max_r) {
			n /= 2;
			r = group_sphere(ci, n, &g->c);
		}
		g->r = r;
		g->first = (uint16_t)ci;
		g->n = (uint16_t)n;
		g->node = (uint16_t)node;
		g_grp_n++;
		ci += n;
	}
}

static int parse_mesh(const uint8_t *b, unsigned sz)
{
	if (!b || sz < 32 || memcmp(b, "P1MS", 4) != 0 || rd16(b + 4) != 3 || ((uintptr_t)b & 3)) {
		return 0;
	}
	g_scale = rd16(b + 6);
	if (g_scale <= 0) {
		g_scale = 1;
	}
	g_vert_n = rd32(b + 8);
	g_prim_n = rd32(b + 12);
	g_chunk_n = rd32(b + 16);
	const unsigned extras = rd32(b + 20);
	unsigned off = 32;
	g_verts = (const P1Vert *)(b + off);
	off += g_vert_n * sizeof(P1Vert);
	g_prims = (const P1Prim *)(b + off);
	off += g_prim_n * sizeof(P1Prim);
	off = (off + 3) & ~3u;
	g_chunks = b + off;
	off += g_chunk_n * CHUNK_SZ;
	if (off + extras > sz) {
		return 0;
	}
	g_mesh = b;
	g_mesh_sz = sz;
	/* a corner past the vertices (a damaged file) is pointed at vertex 0,
	 * so the draw loops need not check */
	{
		P1Prim *pp = (P1Prim *)g_prims;
		for (unsigned i = 0; i < g_prim_n; i++) {
			if (pp[i].a >= g_vert_n) pp[i].a = 0;
			if (pp[i].b >= g_vert_n) pp[i].b = 0;
			if (pp[i].c >= g_vert_n) pp[i].c = 0;
			if (pp[i].d != 0xffff && pp[i].d >= g_vert_n) pp[i].d = 0;
		}
	}
	free(g_cint);
	g_cint = (ChunkI *)malloc(sizeof(ChunkI) * (g_chunk_n ? g_chunk_n : 1));
	if (!g_cint) {
		return 0;
	}
	for (unsigned ci = 0; ci < g_chunk_n; ci++) {
		const uint8_t *ch = g_chunks + ci * CHUNK_SZ;
		const float sc = (float)g_scale;
		g_cint[ci].c.vx = (int16_t)(rdf(ch + 8) * sc);
		g_cint[ci].c.vy = (int16_t)(rdf(ch + 12) * sc);
		g_cint[ci].c.vz = (int16_t)(rdf(ch + 16) * sc);
		g_cint[ci].c.pad = 0;
		const float r = rdf(ch + 20);
		g_cint[ci].r = r < 0.0f ? -1 : (int32_t)(r * sc) + 1;
	}
	build_groups();
	parse_extras(b + off, extras);
	memset(g_ndrawn, 0, sizeof(g_ndrawn));
	for (unsigned ci = 0; ci < g_chunk_n; ci++) {
		const unsigned nd = rd16(g_chunks + ci * CHUNK_SZ + 6);
		if (nd < MAX_NODES) {
			g_ndrawn[nd] = 1;
		}
	}
	return 1;
}

/* the GDIR VM sends a node's matrix only if something is drawn with it */
extern "C" int gs_draw_node_drawn(int node)
{
	return node >= 0 && node < MAX_NODES && g_ndrawn[node];
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

static int32_t clamp16(int32_t v)
{
	return v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
}

static void fx_from_float(const float *f, FxM *o)
{
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++) {
			o->m[r * 3 + c] = (int32_t)(f[r * 4 + c] * 4096.0f);
		}
		o->t[r] = (int32_t)(f[r * 4 + 3] * (float)g_scale);
	}
}

/* o = a * b (b's translation through a), in 64-bit products */
static void fx_mul(const FxM *a, const FxM *b, FxM *o)
{
	FxM r;
	for (int i = 0; i < 3; i++) {
		const int64_t a0 = a->m[i * 3], a1 = a->m[i * 3 + 1], a2 = a->m[i * 3 + 2];
		for (int j = 0; j < 3; j++) {
			r.m[i * 3 + j] = (int32_t)((a0 * b->m[j] + a1 * b->m[3 + j] + a2 * b->m[6 + j]) >> 12);
		}
		r.t[i] = (int32_t)((a0 * b->t[0] + a1 * b->t[1] + a2 * b->t[2]) >> 12) + a->t[i];
	}
	*o = r;
}

static void fx_load(const FxM *f)
{
	MATRIX m;
	for (int i = 0; i < 9; i++) {
		m.m[i / 3][i % 3] = (int16_t)clamp16(f->m[i]);
	}
	m.t[0] = f->t[0];
	m.t[1] = f->t[1];
	m.t[2] = f->t[2];
	/* the GTE macros read m in their asm without telling the compiler:
	 * make it store m first (at -O2 it would drop the "unused" stores) */
	__asm__ volatile("" : : "m"(m) : "memory");
	gte_SetRotMatrix(&m);
	gte_SetTransMatrix(&m);
	vc_clear(); /* cached corners were for the old matrix */
}

static int32_t fx_max_scale(const FxM *f)
{
	/* an upper bound of the longest column (|x|+|y|+|z| >= its length):
	 * only culling radii use it, and a few % too big costs nothing */
	int32_t best = 0;
	for (int c = 0; c < 3; c++) {
		const int32_t x = f->m[c] < 0 ? -f->m[c] : f->m[c];
		const int32_t y = f->m[3 + c] < 0 ? -f->m[3 + c] : f->m[3 + c];
		const int32_t z = f->m[6 + c] < 0 ? -f->m[6 + c] : f->m[6 + c];
		const int32_t l = x + y + z;
		best = l > best ? l : best;
	}
	return best;
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
	int fog;  /* untextured triangles fade to the fog colour (GTE depth cue) */
	int nfog; /* this chunk's node is NFOG (the backdrop): no fog, no far cut */
	int32_t far_fog_u;
	int32_t fog_n, fog_k; /* fog start (mesh units) and 4096*65536 / its length */
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
static int sphere_visible(const DrawCtx *c, const SVECTOR *centre, int32_t r0, int32_t sc_fx)
{
	if (r0 < 0) {
		return 0;
	}
	const int32_t r = sc_fx == 4096 ? r0 : (int32_t)(((int64_t)r0 * sc_fx) >> 12);
	VECTOR v;
	gte_ldv0(centre);
	gte_rt();
	gte_stlvnl(&v);
	if (v.vz + r < c->near_u || v.vz - r > c->far_u) {
		return 0;
	}
	/* 32-bit (a 64-bit multiply is a libgcc call): zr < 2^20 here (far_u
	 * plus a radius), tangents < 2^14 */
	const int32_t zr = v.vz + r;
	const int32_t lim_x = (((zr >> 4) * c->tanx_fx) >> 8) + r;
	const int32_t lim_y = (((zr >> 4) * c->tany_fx) >> 8) + r;
	if (v.vx > lim_x || -v.vx > lim_x || v.vy > lim_y || -v.vy > lim_y) {
		return 0;
	}
	/* 2: wholly inside the view, near to far (each chunk in it is too) */
	const int32_t zn = v.vz - r;
	if (zn >= c->near_u && zr <= c->far_u) {
		const int32_t ix = (((zn >> 4) * c->tanx_fx) >> 8) - r, iy = (((zn >> 4) * c->tany_fx) >> 8) - r;
		if (v.vx <= ix && -v.vx <= ix && v.vy <= iy && -v.vy <= iy) {
			return 2;
		}
	}
	return 1;
}

static int chunk_visible(const DrawCtx *c, unsigned ci, int32_t sc_fx)
{
	return sphere_visible(c, &g_cint[ci].c, g_cint[ci].r, sc_fx);
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

/* ---- the vertex cache: each corner a chunk's triangles share is turned
 * through the GTE once (RTPS) and kept in the scratchpad, 1 KiB of
 * one-cycle RAM, until the matrix changes. Direct-mapped by index. */
struct VCache {
	uint32_t tag; /* vertex index | the matrix's generation << 16 */
	DVECTOR sxy;
	int32_t sz;
};
#define VC_N 64 /* 12 bytes each: 768 of the scratchpad's 1024 */
#define g_vc ((VCache *)0x1f800000)
static uint32_t g_vc_gen; /* bumped when the matrix changes: no clearing */

static void vc_clear(void)
{
	g_vc_gen += 0x10000;
	if (!g_vc_gen) {
		/* wrapped: old tags could match again */
		for (int i = 0; i < VC_N; i++) {
			g_vc[i].tag = 0xffffffffu;
		}
		g_vc_gen = 0x10000;
	}
}

static inline const VCache *vc_get(unsigned i, const P1Vert *v)
{
	VCache *e = &g_vc[i & (VC_N - 1)];
	const uint32_t tag = i | g_vc_gen;
	if (e->tag != tag) {
		gte_ldv0(&v->p);
		gte_rtps();
		gte_stsxy(&e->sxy);
		gte_stsz(&e->sz);
		e->tag = tag;
	}
	return e;
}

/* ---- the slow path: triangles through the near plane, or too big for the
 * GPU. Done in view space with the GTE matrix still loaded: clip at the near
 * plane, split until each piece fits the GPU's limits, project by hand. */
struct CV {
	int32_t x, y, z;
	int32_t r, g, b, u, v; /* colour and texel, 8.8 */
};

/* a clipped polygon (inside the guard band, in front of the near plane):
 * each corner projected once, one facing test, then a fan of primitives */
static int emit_fan(DrawCtx *c, const CV *v, const int *sx, const int *sy, int pn, unsigned tex);

static int emit_poly(DrawCtx *c, const CV *v, int pn, unsigned tex)
{
	int sx[9], sy[9];
	const int32_t h = (int32_t)c->h;
	for (int k = 0; k < pn; k++) {
		const int32_t z = v[k].z > 0 ? v[k].z : 1;
		sx[k] = SCR_W / 2 + v[k].x * h / z;
		sy[k] = SCR_H / 2 + v[k].y * h / z;
	}
	return emit_fan(c, v, sx, sy, pn, tex);
}

/* a convex polygon already projected (corners' z, colour and texel in v) */
static int emit_fan(DrawCtx *c, const CV *v, const int *sx, const int *sy, int pn, unsigned tex)
{
	int32_t zs = 0;
	int x0 = 1 << 30, x1 = -(1 << 30), y0 = 1 << 30, y1 = -(1 << 30);
	for (int k = 0; k < pn; k++) {
		zs += v[k].z;
		x0 = sx[k] < x0 ? sx[k] : x0;
		x1 = sx[k] > x1 ? sx[k] : x1;
		y0 = sy[k] < y0 ? sy[k] : y0;
		y1 = sy[k] > y1 ? sy[k] : y1;
	}
	if (x1 < 0 || x0 >= SCR_W || y1 < 0 || y0 >= SCR_H) {
		return 0;
	}
	/* the polygon's facing (shoelace; the GTE's NCLIP sign) */
	int32_t area = 0;
	for (int k = 0; k < pn; k++) {
		const int j = k + 1 < pn ? k + 1 : 0;
		area += sx[k] * sy[j] - sx[j] * sy[k];
	}
	if (area <= 0) {
		return 0;
	}
	int idx = (zs / (pn * 4)) >> c->ot_shift;
	if (idx >= OT_LEN) {
		return 0;
	}
	if (idx < 1) {
		idx = 1;
	}
	const int textured = tex < (unsigned)g_tex_n && g_tex[tex].ready;
	int n = 0;
	for (int i = 1; i + 1 < pn; i++) {
		const int a = 0, b = i, d = i + 1;
		if (textured) {
			const Tex *tx = &g_tex[tex];
			const unsigned need = sizeof(POLY_GT3) + (tx->tiled ? 2 * sizeof(DR_TWIN) : 0);
			if (g_pp + need > g_pend) {
				break;
			}
			POLY_GT3 *p = (POLY_GT3 *)g_pp;
			g_pp += sizeof(POLY_GT3);
			setPolyGT3(p);
			setXY3(p, sx[a], sy[a], sx[b], sy[b], sx[d], sy[d]);
			setRGB0(p, v[a].r >> 8, v[a].g >> 8, v[a].b >> 8);
			setRGB1(p, v[b].r >> 8, v[b].g >> 8, v[b].b >> 8);
			setRGB2(p, v[d].r >> 8, v[d].g >> 8, v[d].b >> 8);
			setUV3(p, (v[a].u >> 8) + tx->u0, (v[a].v >> 8) + tx->v0, (v[b].u >> 8) + tx->u0, (v[b].v >> 8) + tx->v0,
					(v[d].u >> 8) + tx->u0, (v[d].v >> 8) + tx->v0);
			p->tpage = tx->tpage;
			p->clut = tx->clut;
			if (tx->tiled) {
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
			setXY3(p, sx[a], sy[a], sx[b], sy[b], sx[d], sy[d]);
			setRGB0(p, v[a].r >> 8, v[a].g >> 8, v[a].b >> 8);
			setRGB1(p, v[b].r >> 8, v[b].g >> 8, v[b].b >> 8);
			setRGB2(p, v[d].r >> 8, v[d].g >> 8, v[d].b >> 8);
			ot_append(idx, p, p);
		}
		n++;
	}
	return n;
}

/* uv: the three corners' texel UVs (u, v each) */
static int draw_tri_slow(DrawCtx *c, const P1Vert *va, const P1Vert *vb, const P1Vert *vc, const uint8_t *uv, unsigned tex)
{
	const P1Vert *src[3] = { va, vb, vc };
	CV v[3];
	for (int k = 0; k < 3; k++) {
		VECTOR o;
		gte_ldv0(&src[k]->p);
		gte_rt();
		gte_stlvnl(&o);
		v[k].x = o.vx;
		v[k].y = o.vy;
		v[k].z = o.vz;
		v[k].r = src[k]->r << 8;
		v[k].g = src[k]->g << 8;
		v[k].b = src[k]->b << 8;
		v[k].u = uv[k * 2] << 8;
		v[k].v = uv[k * 2 + 1] << 8;
	}
	for (int k = 0; k < 3; k++) {
		if (v[k].x > 0xfffff || v[k].x < -0xfffff || v[k].y > 0xfffff || v[k].y < -0xfffff || v[k].z > 0xfffff) {
			return 0; /* far outside anything drawn */
		}
	}
	/* clip against the near plane and a guard band round the screen
	 * (screen x -340..660, y -130..370: inside the GPU's 1023x511), so
	 * every piece projects to a primitive the GPU draws */
	CV pa[9], pb[9];
	int pn = 3;
	pa[0] = v[0];
	pa[1] = v[1];
	pa[2] = v[2];
	const int32_t h = (int32_t)c->h;
	/* plane k: d = a*z - s*coord >= 0 with a = band edge in pixels, s = h */
	const int32_t band[4] = { SCR_W / 2 + 340, SCR_W / 2 + 340, SCR_H / 2 + 130, SCR_H / 2 + 130 };
	CV *in = pa, *out = pb;
	static const int order[5] = { 4, 0, 1, 2, 3 }; /* near first: then z > 0 */
	for (int oi = 0; oi < 5; oi++) {
		const int k = order[oi];
		/* each corner's distance to the plane, once */
		int32_t d[9];
		int inside = 0;
		for (int i = 0; i < pn; i++) {
			const CV &a = in[i];
			if (k == 4) {
				d[i] = a.z - c->near_u;
			} else {
				/* 32-bit: corners are inside +-2^20 (checked above), band
				 * <= 500 and h < 512 */
				const int32_t ca = (k >> 1) ? a.y : a.x;
				d[i] = (k & 1) ? band[k] * a.z + h * ca : band[k] * a.z - h * ca;
			}
			inside += d[i] >= 0;
		}
		if (inside == pn) {
			continue; /* this plane cuts nothing: no copy */
		}
		if (!inside) {
			return 0;
		}
		int on = 0;
		for (int i = 0; i < pn; i++) {
			const int j = i + 1 < pn ? i + 1 : 0;
			const CV &a = in[i], &b = in[j];
			const int32_t da = d[i], db = d[j];
			if (da >= 0) {
				out[on++] = a;
			}
			if ((da >= 0) != (db >= 0) && on < 9) {
				/* the point at d = 0 on a..b */
				int32_t na = da, nb = db;
				while (na > 0x3ffff || na < -0x3ffff || nb > 0x3ffff || nb < -0x3ffff) {
					na >>= 2;
					nb >>= 2;
				}
				const int32_t den = na - nb;
				const int32_t t = den ? (na << 12) / den : 0;
				CV m;
				m.x = a.x + (int32_t)(((int64_t)(b.x - a.x) * t) >> 12);
				m.y = a.y + (int32_t)(((int64_t)(b.y - a.y) * t) >> 12);
				m.z = a.z + (int32_t)(((int64_t)(b.z - a.z) * t) >> 12);
				m.r = a.r + (((b.r - a.r) * t) >> 12);
				m.g = a.g + (((b.g - a.g) * t) >> 12);
				m.b = a.b + (((b.b - a.b) * t) >> 12);
				m.u = a.u + (((b.u - a.u) * t) >> 12);
				m.v = a.v + (((b.v - a.v) * t) >> 12);
				if (k == 4 && m.z < c->near_u) {
					m.z = c->near_u;
				}
				out[on++] = m;
			}
		}
		pn = on;
		if (pn < 3) {
			return 0;
		}
		CV *t = in;
		in = out;
		out = t;
	}
	return emit_poly(c, in, pn, tex);
}

/* A triangle through the near plane, the quick way: clipped there in
 * object space (32-bit sums: positions are int16), the pieces projected by
 * the GTE (RTPS: no divides in software). A corner the GTE can't place on
 * screen (its SX/SY saturate) or a piece too big for the GPU goes to the
 * exact routine (draw_tri_slow). */
struct NV {
	int32_t x, y, z;    /* object space, mesh units */
	int32_t vx, vy, vz; /* view space */
	int32_t r, g, b, u, v;
};

static int draw_tri_near(DrawCtx *c, const P1Vert *va, const P1Vert *vb, const P1Vert *vc, const uint8_t *uv, unsigned tex)
{
	const P1Vert *src[3] = { va, vb, vc };
	NV in[3];
	int behind = 0;
	for (int k = 0; k < 3; k++) {
		VECTOR o;
		gte_ldv0(&src[k]->p);
		gte_rt();
		gte_stlvnl(&o);
		if (o.vx > 0xfffff || o.vx < -0xfffff || o.vy > 0xfffff || o.vy < -0xfffff || o.vz > 0xfffff || o.vz < -0xfffff) {
			return draw_tri_slow(c, va, vb, vc, uv, tex);
		}
		in[k].x = src[k]->p.vx;
		in[k].y = src[k]->p.vy;
		in[k].z = src[k]->p.vz;
		in[k].vx = o.vx;
		in[k].vy = o.vy;
		in[k].vz = o.vz;
		in[k].r = src[k]->r;
		in[k].g = src[k]->g;
		in[k].b = src[k]->b;
		in[k].u = uv[k * 2];
		in[k].v = uv[k * 2 + 1];
		behind += o.vz < c->near_u;
	}
	if (behind == 3) {
		return 0;
	}
	/* the near plane: a triangle becomes a triangle or a quad */
	NV out[4];
	int pn = 0;
	const int32_t nz = c->near_u;
	for (int i = 0; i < 3; i++) {
		const NV &a = in[i], &b = in[i + 1 < 3 ? i + 1 : 0];
		const int32_t da = a.vz - nz, db = b.vz - nz;
		if (da >= 0) {
			out[pn++] = a;
		}
		if ((da >= 0) != (db >= 0)) {
			int32_t na = da, nb = db;
			while (na > 0x7ffff || na < -0x7ffff || nb > 0x7ffff || nb < -0x7ffff) {
				na >>= 1;
				nb >>= 1;
			}
			const int32_t t = (na << 12) / (na - nb); /* 0..4096 along a..b */
			NV &m = out[pn++];
			m.x = a.x + (((b.x - a.x) * t) >> 12);
			m.y = a.y + (((b.y - a.y) * t) >> 12);
			m.z = a.z + (((b.z - a.z) * t) >> 12);
			m.vx = a.vx + (((b.vx - a.vx) >> 4) * t >> 8);
			m.vy = a.vy + (((b.vy - a.vy) >> 4) * t >> 8);
			m.vz = nz;
			m.r = a.r + (((b.r - a.r) * t) >> 12);
			m.g = a.g + (((b.g - a.g) * t) >> 12);
			m.b = a.b + (((b.b - a.b) * t) >> 12);
			m.u = a.u + (((b.u - a.u) * t) >> 12);
			m.v = a.v + (((b.v - a.v) * t) >> 12);
		}
	}
	if (pn < 3) {
		return 0;
	}
	/* wholly outside a side of the view (beside or above the camera):
	 * nothing to draw. 12-bit tangents, a corner or two of slack. */
	{
		int out_l = 0, out_r = 0, out_t = 0, out_b = 0;
		for (int k = 0; k < pn; k++) {
			/* 32-bit: |vz| < 2^20, tangents < 2^14 */
			const int32_t lx = (((out[k].vz >> 4) * c->tanx_fx) >> 8) + 2;
			const int32_t ly = (((out[k].vz >> 4) * c->tany_fx) >> 8) + 2;
			out_r += out[k].vx > lx;
			out_l += out[k].vx < -lx;
			out_b += out[k].vy > ly;
			out_t += out[k].vy < -ly;
		}
		if (out_r == pn || out_l == pn || out_b == pn || out_t == pn) {
			return 0;
		}
	}
	int sx[4], sy[4];
	CV cv[4];
	for (int k = 0; k < pn; k++) {
		SVECTOR p;
		p.vx = (int16_t)out[k].x;
		p.vy = (int16_t)out[k].y;
		p.vz = (int16_t)out[k].z;
		p.pad = 0;
		DVECTOR s;
		int32_t flag;
		gte_ldv0(&p);
		gte_rtps();
		gte_stsxy(&s);
		gte_stflg(&flag);
		if (flag & 0x6000) {
			/* SX2/SY2 saturated: off the GTE's screen range */
			return draw_tri_slow(c, va, vb, vc, uv, tex);
		}
		sx[k] = s.vx;
		sy[k] = s.vy;
		cv[k].z = out[k].vz;
		cv[k].r = out[k].r << 8;
		cv[k].g = out[k].g << 8;
		cv[k].b = out[k].b << 8;
		cv[k].u = out[k].u << 8;
		cv[k].v = out[k].v << 8;
	}
	int x0 = sx[0], x1 = sx[0], y0 = sy[0], y1 = sy[0];
	for (int k = 1; k < pn; k++) {
		x0 = sx[k] < x0 ? sx[k] : x0;
		x1 = sx[k] > x1 ? sx[k] : x1;
		y0 = sy[k] < y0 ? sy[k] : y0;
		y1 = sy[k] > y1 ? sy[k] : y1;
	}
	if (x1 - x0 > 1000 || y1 - y0 > 500) {
		return draw_tri_slow(c, va, vb, vc, uv, tex);
	}
	return emit_fan(c, cv, sx, sy, pn, tex);
}

/* a primitive through the slow path: a triangle, or a quad as its two */
static int prim_slow(DrawCtx *c, const P1Prim *pr, const P1Vert *va, const P1Vert *vb, const P1Vert *vc, const P1Vert *vd)
{
	int n = draw_tri_near(c, va, vb, vc, pr->uv, pr->tex);
	if (vd) {
		/* the second triangle c,b,d (the source triangle's winding) */
		const uint8_t uv2[6] = { pr->uv[4], pr->uv[5], pr->uv[2], pr->uv[3], pr->uv[6], pr->uv[7] };
		n += draw_tri_near(c, vc, vb, vd, uv2, pr->tex);
	}
	return n;
}

/* a run of quads (QUAD 1) or of triangles: one loop for each, no
 * per-primitive test of which */
template <int QUAD>
static void draw_prims(DrawCtx *c, unsigned first, unsigned n)
{
	const int32_t near_z = c->near_u;
	const int ot_shift = c->ot_shift;
	const int fog = c->fog && !c->nfog;
	const P1Vert *const verts = g_verts;
	unsigned end = first + n;
	if (end > g_prim_n) {
		end = g_prim_n;
	}
	c->tested += n;
	unsigned drawn = 0;
	for (const P1Prim *pr = g_prims + first, *pe = g_prims + end; pr < pe; pr++) {
		/* indices were checked when the mesh loaded (parse_mesh) */
		const unsigned ia = pr->a, ib = pr->b, ic = pr->c, id = QUAD ? pr->d : 0;
		const int quad = QUAD;
		const P1Vert *va = verts + ia;
		const P1Vert *vb = verts + ib;
		const P1Vert *vc = verts + ic;
		const P1Vert *vd = quad ? verts + id : 0;
		const VCache *ca = vc_get(ia, va);
		const VCache *cb = vc_get(ib, vb);
		const VCache *cc = vc_get(ic, vc);
		const VCache *cd = quad ? vc_get(id, vd) : cc;
		const int32_t z0 = ca->sz, z1 = cb->sz, z2 = cc->sz, z3 = cd->sz;
		if (z0 < near_z || z1 < near_z || z2 < near_z || z3 < near_z) {
			/* through the near plane (the road under the kart): cut it
			 * there instead of losing it */
			if (z0 >= near_z || z1 >= near_z || z2 >= near_z || z3 >= near_z) {
				drawn += prim_slow(c, pr, va, vb, vc, vd);
			}
			continue;
		}
		const DVECTOR s0 = ca->sxy, s1 = cb->sxy, s2 = cc->sxy, s3 = cd->sxy;
		/* front faces only (the GTE's NCLIP sign; a quad is flat enough
		 * that its first triangle speaks for it) */
		const int32_t nclip = (s1.vx - s0.vx) * (s2.vy - s0.vy) - (s2.vx - s0.vx) * (s1.vy - s0.vy);
		if (nclip <= 0) {
			continue; /* back face or edge on */
		}
		{
			/* the GPU drops a primitive wider than 1023 or taller than
			 * 511 pixels: big ones near the camera are split */
			int x0 = s0.vx, x1 = x0, y0 = s0.vy, y1 = y0;
			x0 = s1.vx < x0 ? s1.vx : x0;
			x1 = s1.vx > x1 ? s1.vx : x1;
			y0 = s1.vy < y0 ? s1.vy : y0;
			y1 = s1.vy > y1 ? s1.vy : y1;
			x0 = s2.vx < x0 ? s2.vx : x0;
			x1 = s2.vx > x1 ? s2.vx : x1;
			y0 = s2.vy < y0 ? s2.vy : y0;
			y1 = s2.vy > y1 ? s2.vy : y1;
			x0 = s3.vx < x0 ? s3.vx : x0;
			x1 = s3.vx > x1 ? s3.vx : x1;
			y0 = s3.vy < y0 ? s3.vy : y0;
			y1 = s3.vy > y1 ? s3.vy : y1;
			if (x1 < 0 || x0 >= SCR_W || y1 < 0 || y0 >= SCR_H) {
				continue;
			}
			if (x1 - x0 > 1000 || y1 - y0 > 500) {
				drawn += draw_tri_slow(c, va, vb, vc, pr->uv, pr->tex);
				if (vd) {
					const uint8_t uv2[6] = { pr->uv[4], pr->uv[5], pr->uv[2], pr->uv[3], pr->uv[6], pr->uv[7] };
					drawn += draw_tri_slow(c, vc, vb, vd, uv2, pr->tex);
				}
				continue;
			}
		}
		/* average depth / 4 */
		const int32_t zsum = quad ? z0 + z1 + z2 + z3 : z0 + z1 + z2;
		int idx = (quad ? zsum >> 4 : (zsum * 1365) >> 14) >> ot_shift;
		if (idx >= OT_LEN) {
			if (!c->nfog) {
				continue;
			}
			idx = OT_LEN - 1; /* the backdrop: behind everything */
		}
		if (idx < 1) {
			idx = 1;
		}
		const unsigned tex = pr->tex;
		if (tex < (unsigned)g_tex_n && g_tex[tex].ready) {
			const Tex *tx = &g_tex[tex];
			const unsigned need = (quad ? sizeof(POLY_GT4) : sizeof(POLY_GT3)) + (tx->tiled ? 2 * sizeof(DR_TWIN) : 0);
			if (g_pp + need > g_pend) {
				break;
			}
			const uint8_t *uv = pr->uv;
			const int u0 = tx->u0, v0 = tx->v0;
			void *p;
			if (quad) {
				POLY_GT4 *q = (POLY_GT4 *)g_pp;
				g_pp += sizeof(POLY_GT4);
				setPolyGT4(q);
				setXY4(q, s0.vx, s0.vy, s1.vx, s1.vy, s2.vx, s2.vy, s3.vx, s3.vy);
				setRGB0(q, va->r, va->g, va->b);
				setRGB1(q, vb->r, vb->g, vb->b);
				setRGB2(q, vc->r, vc->g, vc->b);
				setRGB3(q, vd->r, vd->g, vd->b);
				setUV4(q, uv[0] + u0, uv[1] + v0, uv[2] + u0, uv[3] + v0, uv[4] + u0, uv[5] + v0, uv[6] + u0, uv[7] + v0);
				q->tpage = tx->tpage;
				q->clut = tx->clut;
				p = q;
			} else {
				POLY_GT3 *t = (POLY_GT3 *)g_pp;
				g_pp += sizeof(POLY_GT3);
				setPolyGT3(t);
				setXY3(t, s0.vx, s0.vy, s1.vx, s1.vy, s2.vx, s2.vy);
				setRGB0(t, va->r, va->g, va->b);
				setRGB1(t, vb->r, vb->g, vb->b);
				setRGB2(t, vc->r, vc->g, vc->b);
				setUV3(t, uv[0] + u0, uv[1] + v0, uv[2] + u0, uv[3] + v0, uv[4] + u0, uv[5] + v0);
				t->tpage = tx->tpage;
				t->clut = tx->clut;
				p = t;
			}
			if (tx->tiled) {
				/* window on, the primitive, window off */
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
		} else if (quad) {
			if (g_pp + sizeof(POLY_G4) > g_pend) {
				break;
			}
			POLY_G4 *q = (POLY_G4 *)g_pp;
			g_pp += sizeof(POLY_G4);
			if (fog) {
				/* towards the fog colour by the quad's average depth */
				int32_t f = (int32_t)((((int64_t)(zsum >> 2) - c->fog_n) * c->fog_k) >> 16);
				f = f < 0 ? 0 : (f > 4096 ? 4096 : f);
				__asm__ volatile("mtc2 %0, $8" : : "r"(f));
				gte_ldrgb3(&va->r, &vb->r, &vc->r);
				gte_dpct();
				gte_strgb3_g4(q);
				gte_ldrgb(&vd->r);
				gte_dpcs();
				gte_strgb(&q->r3);
			} else {
				setRGB0(q, va->r, va->g, va->b);
				setRGB1(q, vb->r, vb->g, vb->b);
				setRGB2(q, vc->r, vc->g, vc->b);
				setRGB3(q, vd->r, vd->g, vd->b);
			}
			setPolyG4(q);
			setXY4(q, s0.vx, s0.vy, s1.vx, s1.vy, s2.vx, s2.vy, s3.vx, s3.vy);
			ot_append(idx, q, q);
		} else {
			if (g_pp + sizeof(POLY_G3) > g_pend) {
				break;
			}
			POLY_G3 *p = (POLY_G3 *)g_pp;
			g_pp += sizeof(POLY_G3);
			if (fog) {
				/* the colours towards the fog colour by the triangle's
				 * average depth: IR0 0 at the fog's near, 4096 at its far */
				int32_t f = (int32_t)((((int64_t)(zsum / 3) - c->fog_n) * c->fog_k) >> 16);
				f = f < 0 ? 0 : (f > 4096 ? 4096 : f);
				__asm__ volatile("mtc2 %0, $8" : : "r"(f));
				gte_ldrgb3(&va->r, &vb->r, &vc->r);
				gte_dpct();
				gte_strgb3_g3(p);
			} else {
				setRGB0(p, va->r, va->g, va->b);
				setRGB1(p, vb->r, vb->g, vb->b);
				setRGB2(p, vc->r, vc->g, vc->b);
			}
			setPolyG3(p);
			setXY3(p, s0.vx, s0.vy, s1.vx, s1.vy, s2.vx, s2.vy);
			ot_append(idx, p, p);
		}
		drawn++;
	}
	c->tris += drawn;
}

static inline void draw_chunk(DrawCtx *c, unsigned ci)
{
	const uint8_t *ch = g_chunks + ci * CHUNK_SZ;
	const unsigned first = rd32(ch), nq = rd16(ch + 4), nt = rd16(ch + 24);
	if (nq) {
		draw_prims<1>(c, first, nq);
	}
	if (nt) {
		draw_prims<0>(c, first + nq, nt);
	}
}

/* one group: its node's matrix in the GTE (loaded: the node whose matrix
 * is there, -1 the view alone), then the group and its chunks culled */
static void draw_group(DrawCtx *c, const FxM *vfx, unsigned gi, int *loaded)
{
	const ChunkG *g = &g_grp[gi];
	const unsigned node = g->node;
	const int moved = node < MAX_NODES && g_nodes_live && g_nm_on[node];
	const int want = moved ? (int)node : -1;
	if (want != *loaded) {
		*loaded = want;
		if (moved) {
			FxM mv;
			fx_mul(vfx, &g_nm[node], &mv);
			fx_load(&mv);
		} else {
			fx_load(vfx);
		}
	}
	c->nfog = node < MAX_NODES && g_nfog[node];
	c->far_u = c->nfog ? 0x7fffffff : c->far_fog_u;
	const int vis = sphere_visible(c, &g->c, g->r, 4096);
	if (!vis) {
		return;
	}
	for (unsigned ci = g->first; ci < (unsigned)g->first + g->n; ci++) {
		/* a group wholly in view: so is each of its chunks */
		if (vis == 2 ? g_cint[ci].r >= 0 : chunk_visible(c, ci, 4096)) {
			draw_chunk(c, ci);
		}
	}
}

static inline int group_skipped(unsigned gi)
{
	const unsigned node = g_grp[gi].node;
	return node < MAX_NODES && (g_isrc[node] || (g_nodes_live && g_nhide[node]));
}

static void draw_scene_mesh(DrawCtx *c, const float *view)
{
	/* the scratchpad was the scripts' stack until now: empty the cache */
	for (int i = 0; i < VC_N; i++) {
		g_vc[i].tag = 0xffffffffu;
	}
	FxM vfx;
	fx_from_float(view, &vfx);
	int loaded = -2;
	for (unsigned gi = 0; gi < g_grp_n; gi++) {
		if (!group_skipped(gi)) {
			draw_group(c, &vfx, gi, &loaded);
		}
	}
	/* MultiMesh instances: each one draws its source node's chunks. The
	 * instance's world matrix and sphere are kept until its node moves; the
	 * sphere is tested before any per-frame matrix work. */
	c->nfog = 0;
	c->far_u = c->far_fog_u;
	for (unsigned i = 0; i < g_inst_n; i++) {
		const uint8_t *row = g_inst + i * 52;
		const unsigned nd = rd16(row);
		if (nd >= MAX_NODES || g_isrc[nd] != 2 || g_nhide[nd]) {
			continue;
		}
		InstCache *ic = &g_icache[i];
		if (!ic->ok || ic->ver != g_nm_ver[nd]) {
			int32_t sc = g_iscale[i];
			if (g_nm_on[nd]) {
				fx_mul(&g_nm[nd], &g_ifx[i], &ic->w);
				sc = (int32_t)(((int64_t)sc * g_nscale[nd]) >> 12);
			} else {
				ic->w = g_ifx[i];
			}
			const int32_t *sp = g_isph[nd];
			const FxM *w = &ic->w;
			ic->cx = (int32_t)(((int64_t)w->m[0] * sp[0] + (int64_t)w->m[1] * sp[1] + (int64_t)w->m[2] * sp[2]) >> 12) + w->t[0];
			ic->cy = (int32_t)(((int64_t)w->m[3] * sp[0] + (int64_t)w->m[4] * sp[1] + (int64_t)w->m[5] * sp[2]) >> 12) + w->t[1];
			ic->cz = (int32_t)(((int64_t)w->m[6] * sp[0] + (int64_t)w->m[7] * sp[1] + (int64_t)w->m[8] * sp[2]) >> 12) + w->t[2];
			ic->r = (int32_t)(((int64_t)sp[3] * sc) >> 12) + 1;
			ic->ver = g_nm_ver[nd];
			ic->ok = 1;
		}
		/* the sphere in view space, against the frustum */
		const int32_t vx = (int32_t)(((int64_t)vfx.m[0] * ic->cx + (int64_t)vfx.m[1] * ic->cy + (int64_t)vfx.m[2] * ic->cz) >> 12) + vfx.t[0];
		const int32_t vy = (int32_t)(((int64_t)vfx.m[3] * ic->cx + (int64_t)vfx.m[4] * ic->cy + (int64_t)vfx.m[5] * ic->cz) >> 12) + vfx.t[1];
		const int32_t vz = (int32_t)(((int64_t)vfx.m[6] * ic->cx + (int64_t)vfx.m[7] * ic->cy + (int64_t)vfx.m[8] * ic->cz) >> 12) + vfx.t[2];
		const int32_t r = ic->r;
		if (vz + r < c->near_u || vz - r > c->far_u) {
			continue;
		}
		const int32_t zr = vz + r;
		const int32_t lim_x = (int32_t)(((int64_t)zr * c->tanx_fx) >> 12) + r;
		const int32_t lim_y = (int32_t)(((int64_t)zr * c->tany_fx) >> 12) + r;
		if (vx > lim_x || -vx > lim_x || vy > lim_y || -vy > lim_y) {
			continue;
		}
		FxM mv;
		fx_mul(&vfx, &ic->w, &mv);
		fx_load(&mv);
		const int32_t chunk_sc = ic->r > 0 ? (int32_t)(((int64_t)g_iscale[i] * (g_nm_on[nd] ? g_nscale[nd] : 4096)) >> 12) : 4096;
		for (unsigned ci = g_isrc_c0[nd]; ci < g_isrc_c1[nd]; ci++) {
			if (chunk_visible(c, ci, chunk_sc)) {
				draw_chunk(c, ci);
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
		/* the GTE's perspective divide saturates nearer than H/2 (H/SZ
		 * tops out at 2): nothing nearer is projected by it */
		if (c.near_u < (int32_t)c.h / 2 + 1) {
			c.near_u = (int32_t)c.h / 2 + 1;
		}
		c.fog = 0;
		if (g_fog_on && g_fog_far > g_fog_near) {
			/* linear fog from the triangle's depth (draw_prims), the GTE's
			 * depth cue (DPCT) blending towards the far colour */
			const float n = (g_fog_near > NEAR_M ? g_fog_near : NEAR_M) * (float)g_scale;
			const float f = g_fog_far * (float)g_scale;
			c.fog_n = (int32_t)n;
			c.fog_k = (int32_t)(4096.0f * 65536.0f / (f - n));
			gte_SetFarColor(g_fog_r, g_fog_g, g_fog_b);
			c.fog = 1;
		}
		c.far_u = (int32_t)(c.far_m * (float)g_scale);
		c.far_fog_u = c.far_u;
		c.tanx_fx = (int32_t)(c.tan_x * 4096.0f);
		c.tany_fx = (int32_t)(c.tan_y * 4096.0f);
		float view[12];
		view_matrix(view);
		draw_scene_mesh(&c, view);
		tris = c.tris;
#ifdef P1_STATS
		static unsigned frames;
		if ((frames++ % 120) == 0) {
			float ex, ey, ez;
			eye_now(&ex, &ey, &ez);
			printf("P1 frame: %u tris, prims %u bytes, eye %d %d %d look %d %d %d (cm), fov %d, scale %d, chunks %u\n", tris,
					(unsigned)(g_pp - f->pri), (int)(ex * 100), (int)(ey * 100), (int)(ez * 100), (int)(g_look_x * 100),
					(int)(g_look_y * 100), (int)(g_look_z * 100), (int)g_fov, g_scale, g_chunk_n);
		}
#endif
	}
	(void)tris;
	const uint16_t h1 = (uint16_t)TIMER_VALUE(1);
	draw_hud();
	const uint16_t h2 = (uint16_t)TIMER_VALUE(1);
	/* the last frame's drawing done and shown: start this one's */
	DrawSync(0);
	const uint16_t h3 = (uint16_t)TIMER_VALUE(1);
#ifdef P1_STATS
	/* a printf to the TTY takes a field or more: only in stats builds */
	static unsigned s_hs, s_hh, s_hg, s_hn;
	s_hs += (uint16_t)(h1 - h0);
	s_hh += (uint16_t)(h2 - h1);
	s_hg += (uint16_t)(h3 - h2);
	if (++s_hn == 120) {
		printf("P1 hblanks/frame: scene %u hud %u gpu wait %u\n", s_hs / 120, s_hh / 120, s_hg / 120);
		s_hs = s_hh = s_hg = s_hn = 0;
	}
#else
	(void)h0;
	(void)h1;
	(void)h2;
	(void)h3;
#endif
	/* Even pacing: each frame is shown the same number of fields after the
	 * last. A frame that would take more raises the count at once; it comes
	 * down one when 90 frames in a row would have made the lower one. (A
	 * race whose frames take 2 or 3 fields by turns runs at a steady 20 FPS
	 * instead of a jerky 24.) */
	static int s_flip_at, s_pace = 1, s_calm;
	const int need = VSync(-1) - s_flip_at + 1; /* shown at the next vblank */
	if (need > s_pace) {
		s_pace = need > 3 ? 3 : need;
		s_calm = 0;
	} else if (need < s_pace) {
		if (++s_calm >= 90) {
			s_pace--;
			s_calm = 0;
		}
	} else {
		s_calm = 0;
	}
	do {
		VSync(0);
	} while (VSync(-1) - s_flip_at < s_pace);
	s_flip_at = VSync(-1);
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
		fx_from_float(m, &g_nm[node]);
		g_nscale[node] = fx_max_scale(&g_nm[node]);
		g_nm_on[node] = 1;
	} else {
		g_nm_on[node] = 0;
	}
	g_nm_ver[node]++;
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
	for (int i = 0; i < MAX_NODES; i++) {
		g_nm_ver[i]++;
	}
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
