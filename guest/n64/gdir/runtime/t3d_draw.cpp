// MIT. The PS2 guest's draw API (gs_draw.h) on the N64 with Tiny3D: the
// scene's P1MS primitives (the PS1 forms, internal/platforms/ps1/gdirpack.go)
// through the RSP with a Z-buffer, P1TX textures cut down to fit TMEM, and
// the HUD (rects, the 5x7 font and a 3x5 one for small text) on top in the
// PS1 guest's 320x240 layout.
//
// At load each culling chunk becomes batches: the corners its primitives use
// (position, colour and texel, one vertex each however many share it) in
// Tiny3D's vertex form, at most 68 at a time, and the triangles as indices
// into them, a batch per texture. A frame culls groups of chunks and chunks
// by their spheres on the CPU, then loads each visible batch's vertices and
// draws its triangles.
//
// Space: P1MS positions are Godot's (Y up, metres) times the mesh's scale;
// each node's matrix is its world correction times 1/scale, the camera a
// float look-at as the other guests have it.

#include "gs_draw.h"

#include <libdragon.h>
#include <t3d/t3d.h>
#include <t3d/t3dmath.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>

#define SCR_W 320
#define SCR_H 240
#define HUD_W 640.0f
#define HUD_H 448.0f
#define SAFE_Y 8 /* as the PS1: the PS2's 448 lines on the middle 224 */
#define MAX_TEX 64
#ifdef GDIR_MAX_NODES
#define MAX_NODES GDIR_MAX_NODES
#else
#define MAX_NODES 512
#endif
#define NEAR_M 0.25f
#define BATCH_V 68     /* corners a batch loads (Tiny3D's cache holds 70) */
#define MAT_RING 1024  /* node and instance matrices a frame */
#define MAT_FRAMES 3   /* frames in flight (the display's buffers) */
#define TMEM_TEXELS_4 4096 /* CI4 texels TMEM holds beside a palette */
#define TMEM_TEXELS_8 2048 /* CI8 */
#define TMEM_TEXELS_16 2048 /* RGBA16 */
#define DECAL_DEPTH 0x18   /* depth offset a LAYR step (decals nearer, ground layers in order) */

static int g_pal;

/* a colour from ints (0..255, clamped) */
static inline color_t col(int r, int g, int b, int a)
{
	r = r < 0 ? 0 : (r > 255 ? 255 : r);
	g = g < 0 ? 0 : (g > 255 ? 255 : g);
	b = b < 0 ? 0 : (b > 255 ? 255 : b);
	a = a < 0 ? 0 : (a > 255 ? 255 : a);
	return RGBA32((uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a);
}

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

/* ------------------------------------------------------------ textures */
struct Tex {
	int ready;
	int w, h, psm; /* as the pack has it (psm 0 T4, 1 T8, 2 CT16) */
	int tiled;
	int sx, sy; /* the N64 copy is w >> sx by h >> sy */
	/* too big for TMEM and not wrapped: kept whole and drawn a tile at a
	 * time (tw x th texels, neighbours sharing a row and column so filtering
	 * meets across the seam); the primitives are cut along the tiles */
	int split, tw, th, ntx, nty;
	surface_t surf;
	uint16_t *tlut; /* CI: RGBA16 palette (16 or 256), uncached */
	int tlut_n;
	void *data;
};
static Tex g_tex[MAX_TEX];
static int g_tex_n;

/* PS1 colour (0bbbbbgggggrrrrr, 0x0000 the only see-through one) to RGBA5551 */
static inline uint16_t n64_color(uint16_t c)
{
	if (c == 0) {
		return 0;
	}
	const unsigned r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
	return (uint16_t)((r << 11) | (g << 6) | (b << 1) | 1);
}

static void free_textures(void)
{
	for (int i = 0; i < MAX_TEX; i++) {
		free(g_tex[i].data);
		if (g_tex[i].tlut) {
			free_uncached(g_tex[i].tlut);
		}
	}
	memset(g_tex, 0, sizeof(g_tex));
	g_tex_n = 0;
}

/* P1TX: "P1TX", u16 abi, u16 count, then per texture u16 w, h, u8 psm
 * (0 T4, 1 T8, 2 CT16), u8 flags (1 tiled), u16 clut entries, u32 data
 * bytes, data (rows of w texels; T4 low nibble first), clut (PS1 colours). */
static void parse_textures(const uint8_t *b, unsigned sz)
{
	free_textures();
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
		if (off > sz || t->w <= 0 || t->h <= 0 || t->w > 1024 || t->h > 1024 || t->psm > 2) {
			continue;
		}
		const int bits = t->psm == 0 ? 4 : (t->psm == 1 ? 8 : 16);
		if (data_n < (unsigned)(t->w * t->h * bits / 8)) {
			continue;
		}
		/* a wrapped texture too big for TMEM is halved (the longer side) until
		 * it fits; any other is split into tiles */
		const int budget = bits == 4 ? TMEM_TEXELS_4 : (bits == 8 ? TMEM_TEXELS_8 : TMEM_TEXELS_16);
		int w = t->w, h = t->h;
		t->sx = t->sy = 0;
		t->split = 0;
		if (w * h > budget && !t->tiled) {
			/* a tile's load starts on an 8-byte TMEM line, so it can be
			 * that many texels wider than asked (16 CI4, 8 CI8, 4 RGBA16):
			 * room is left for them */
			const int slack = 64 / bits;
			int tw = w < 64 ? w : 64;
			int th = budget / (tw + slack);
			th = th > h ? h : th;
			t->split = 1;
			t->tw = tw;
			t->th = th;
			t->ntx = tw < w ? (w - 1 + tw - 2) / (tw - 1) : 1;
			t->nty = th < h ? (h - 1 + th - 2) / (th - 1) : 1;
		}
		while (!t->split && w * h > budget) {
			if (w >= h) {
				w >>= 1;
				t->sx++;
			} else {
				h >>= 1;
				t->sy++;
			}
		}
		/* rows of 8 bytes (TMEM's line) at least */
		int stride = w * bits / 8;
		stride = (stride + 7) & ~7;
		uint8_t *px = (uint8_t *)memalign(16, (size_t)stride * h);
		if (!px) {
			continue;
		}
		memset(px, 0, (size_t)stride * h);
		for (int y = 0; y < h; y++) {
			const int sy = y << t->sy;
			for (int x = 0; x < w; x++) {
				const int sxx = x << t->sx;
				if (bits == 4) {
					const uint8_t s = data[(sy * t->w + sxx) >> 1];
					const unsigned idx = (sxx & 1) ? (s >> 4) : (s & 15);
					/* the N64's CI4 has the first texel in the high nibble */
					px[y * stride + (x >> 1)] |= (uint8_t)((x & 1) ? idx : (idx << 4));
				} else if (bits == 8) {
					px[y * stride + x] = data[sy * t->w + sxx];
				} else {
					const uint8_t *s = data + (sy * t->w + sxx) * 2;
					const uint16_t c = n64_color((uint16_t)(s[0] | (s[1] << 8)));
					px[y * stride + x * 2] = (uint8_t)(c >> 8);
					px[y * stride + x * 2 + 1] = (uint8_t)c;
				}
			}
		}
		data_cache_hit_writeback(px, (unsigned long)stride * h);
		t->data = px;
		const tex_format_t fmt = bits == 4 ? FMT_CI4 : (bits == 8 ? FMT_CI8 : FMT_RGBA16);
		t->surf = surface_make(px, fmt, (uint32_t)w, (uint32_t)h, (uint32_t)stride);
		if (bits < 16) {
			const int cells = bits == 4 ? 16 : 256;
			t->tlut = (uint16_t *)malloc_uncached(cells * 2);
			if (!t->tlut) {
				continue;
			}
			for (int k = 0; k < cells; k++) {
				t->tlut[k] = k < (int)clut_n ? n64_color((uint16_t)(clut[k * 2] | (clut[k * 2 + 1] << 8))) : 0;
			}
			t->tlut_n = cells;
		}
		t->ready = 1;
	}
}

/* the font: 5x7 glyphs in 8x8 cells (16 a row, 6 rows) and 3x5 ones in 4x6
 * cells (32 a row, 3 rows), as I4 (one TMEM load each) */
static surface_t g_font_big, g_font_small;
static int g_font_cur; /* 1 big, 2 small loaded, 0 none */

static void make_font(void)
{
	static uint8_t big[64 * 48] __attribute__((aligned(16)));   /* 128x48 I4 */
	static uint8_t small[64 * 18] __attribute__((aligned(16))); /* 128x18 I4 */
	memset(big, 0, sizeof(big));
	memset(small, 0, sizeof(small));
	for (int c = 0; c < 95; c++) {
		const int cx = (c % 16) * 8, cy = (c / 16) * 8;
		for (int row = 0; row < 7; row++) {
			for (int col = 0; col < 5; col++) {
				if (kFont5x7[c][row] & (1u << (4 - col))) {
					const int tx = cx + col, ty = cy + row;
					big[ty * 64 + (tx >> 1)] |= (uint8_t)((tx & 1) ? 0x0f : 0xf0);
				}
			}
		}
	}
	for (int c = 0; c < 95; c++) {
		const int cx = (c % 32) * 4, cy = (c / 32) * 6;
		for (int row = 0; row < 5; row++) {
			for (int col = 0; col < 3; col++) {
				if (kFont3x5[c][row] & (1u << (2 - col))) {
					const int tx = cx + col, ty = cy + row;
					small[ty * 64 + (tx >> 1)] |= (uint8_t)((tx & 1) ? 0x0f : 0xf0);
				}
			}
		}
	}
	data_cache_hit_writeback(big, sizeof(big));
	data_cache_hit_writeback(small, sizeof(small));
	g_font_big = surface_make(big, FMT_I4, 128, 48, 64);
	g_font_small = surface_make(small, FMT_I4, 128, 18, 64);
}

/* ------------------------------------------------------------ mesh */
static const uint8_t *g_mesh;
static int g_scale = 1024;
static float g_inv_scale = 1.0f / 1024.0f;
static unsigned g_vert_n, g_prim_n, g_chunk_n;
#define VERT_SZ 12  /* i16 x y z pad, u8 r g b pad */
#define PRIM_SZ 20  /* u16 a b c d (d 0xffff: a triangle), u8 uv[8], tex, flags, pad */
#define CHUNK_SZ 28 /* u32 first, u16 quads, u16 node, f32 cx cy cz r, u16 triangles, pad */
static const uint8_t *g_verts;
static const uint8_t *g_prims;
static const uint8_t *g_chunks;

/* the draw form */
struct Batch {
	uint32_t v0;   /* first vertex pair in g_vbuf */
	uint32_t i0;   /* first index in g_ibuf */
	uint16_t ntri;
	uint8_t nv;
	uint8_t tex;   /* 0xff: untextured */
	uint16_t tile; /* a split texture's tile */
	uint16_t pad;
};
static T3DVertPacked *g_vbuf;
static uint8_t *g_ibuf;
static Batch *g_batch;
static unsigned g_batch_n;
struct Chunk {
	float c[3], r; /* sphere in metres (r < 0: empty) */
	uint32_t b0;
	uint16_t bn;
	uint16_t node;
};
static Chunk *g_ch;
/* runs of up to 16 neighbouring chunks of one node in one sphere */
struct Group {
	float c[3], r;
	uint16_t first, n, node;
};
static Group *g_grp;
static unsigned g_grp_n;
static uint8_t g_ndrawn[MAX_NODES];

static const uint8_t *g_inst;
static unsigned g_inst_n;
static float (*g_imat)[12]; /* each instance's matrix (metres) */
static uint8_t g_isrc[MAX_NODES];
static uint16_t g_isrc_c0[MAX_NODES], g_isrc_c1[MAX_NODES];
static float g_isph[MAX_NODES][4];
static uint8_t g_nfog[MAX_NODES];
static int8_t g_nlayer[MAX_NODES];
static int g_ready;

/* node matrices from the VM (3x4 row-major, metres) */
static float g_nm[MAX_NODES][12];
static float g_nscale[MAX_NODES];
static uint8_t g_nm_on[MAX_NODES];
static uint8_t g_nhide[MAX_NODES];
static int g_nodes_live;

static float max_scale(const float *m)
{
	float best = 0.0f;
	for (int c = 0; c < 3; c++) {
		const float l = sqrtf(m[c] * m[c] + m[4 + c] * m[4 + c] + m[8 + c] * m[8 + c]);
		best = l > best ? l : best;
	}
	return best;
}

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

static inline void m34_point(const float *m, const float *p, float *o)
{
	for (int r = 0; r < 3; r++) {
		o[r] = m[r * 4] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3];
	}
}

static void parse_extras(const uint8_t *b, unsigned sz)
{
	g_inst = 0;
	g_inst_n = 0;
	memset(g_isrc, 0, sizeof(g_isrc));
	memset(g_nfog, 0, sizeof(g_nfog));
	memset(g_nlayer, 0, sizeof(g_nlayer));
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
				free(g_imat);
				g_imat = (float (*)[12])malloc(sizeof(float) * 12 * (n ? n : 1));
				if (!g_imat) {
					g_inst_n = 0;
				}
				for (unsigned i = 0; i < g_inst_n; i++) {
					for (int k = 0; k < 12; k++) {
						g_imat[i][k] = rdf(g_inst + i * 52 + 4 + k * 4);
					}
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
		} else if (memcmp(h, "LAYR", 4) == 0) {
			/* u16 node, s16 render priority: negative a ground layer (drawn
			 * in order, lowest first), positive a decal (nearer a step) */
			if (off + n * 4 > sz) {
				break;
			}
			for (unsigned i = 0; i < n; i++) {
				const unsigned node = rd16(b + off + i * 4);
				int pri = (int16_t)rd16(b + off + i * 4 + 2);
				pri = pri < -8 ? -8 : (pri > 16 ? 16 : pri);
				if (node < MAX_NODES) {
					g_nlayer[node] = (int8_t)pri;
				}
			}
			off += n * 4;
		} else {
			break;
		}
	}
	for (unsigned ci = 0; ci < g_chunk_n; ci++) {
		const unsigned nd = g_ch[ci].node;
		if (nd >= MAX_NODES || !g_isrc[nd]) {
			continue;
		}
		if (g_isrc[nd] == 1) {
			g_isrc_c0[nd] = (uint16_t)ci;
			g_isrc[nd] = 2;
		}
		g_isrc_c1[nd] = (uint16_t)(ci + 1);
	}
	for (unsigned nd = 0; nd < MAX_NODES; nd++) {
		if (g_isrc[nd] != 2) {
			continue;
		}
		float s[3] = { 0, 0, 0 };
		int cn = 0;
		for (unsigned ci = g_isrc_c0[nd]; ci < g_isrc_c1[nd]; ci++) {
			if (g_ch[ci].r < 0.0f) {
				continue;
			}
			for (int k = 0; k < 3; k++) {
				s[k] += g_ch[ci].c[k];
			}
			cn++;
		}
		for (int k = 0; k < 3; k++) {
			s[k] = cn ? s[k] / (float)cn : 0.0f;
		}
		float r = 0.0f;
		for (unsigned ci = g_isrc_c0[nd]; ci < g_isrc_c1[nd]; ci++) {
			if (g_ch[ci].r < 0.0f) {
				continue;
			}
			const float dx = g_ch[ci].c[0] - s[0], dy = g_ch[ci].c[1] - s[1], dz = g_ch[ci].c[2] - s[2];
			const float d = sqrtf(dx * dx + dy * dy + dz * dz) + g_ch[ci].r;
			r = d > r ? d : r;
		}
		g_isph[nd][0] = s[0];
		g_isph[nd][1] = s[1];
		g_isph[nd][2] = s[2];
		g_isph[nd][3] = cn ? r : -1.0f;
	}
}

static float group_sphere(unsigned first, unsigned n, float *c)
{
	float s[3] = { 0, 0, 0 };
	int cnt = 0;
	for (unsigned ci = first; ci < first + n; ci++) {
		if (g_ch[ci].r < 0.0f) {
			continue;
		}
		for (int k = 0; k < 3; k++) {
			s[k] += g_ch[ci].c[k];
		}
		cnt++;
	}
	if (!cnt) {
		c[0] = c[1] = c[2] = 0.0f;
		return -1.0f;
	}
	for (int k = 0; k < 3; k++) {
		c[k] = s[k] / (float)cnt;
	}
	float r = 0.0f;
	for (unsigned ci = first; ci < first + n; ci++) {
		if (g_ch[ci].r < 0.0f) {
			continue;
		}
		const float dx = g_ch[ci].c[0] - c[0], dy = g_ch[ci].c[1] - c[1], dz = g_ch[ci].c[2] - c[2];
		const float d = sqrtf(dx * dx + dy * dy + dz * dz) + g_ch[ci].r;
		r = d > r ? d : r;
	}
	return r;
}

static void build_groups(void)
{
	free(g_grp);
	g_grp_n = 0;
	g_grp = (Group *)malloc(sizeof(Group) * (g_chunk_n ? g_chunk_n : 1));
	if (!g_grp) {
		return;
	}
	unsigned ci = 0;
	while (ci < g_chunk_n) {
		const unsigned node = g_ch[ci].node;
		unsigned n = 1;
		while (n < 16 && ci + n < g_chunk_n && g_ch[ci + n].node == node) {
			n++;
		}
		Group *g = &g_grp[g_grp_n];
		float r = group_sphere(ci, n, g->c);
		while (n > 1 && r > 80.0f) {
			n /= 2;
			r = group_sphere(ci, n, g->c);
		}
		g->r = r;
		g->first = (uint16_t)ci;
		g->n = (uint16_t)n;
		g->node = (uint16_t)node;
		g_grp_n++;
		ci += n;
	}
}

static void free_batches(void)
{
	free(g_vbuf);
	free(g_ibuf);
	free(g_batch);
	free(g_ch);
	g_vbuf = 0;
	g_ibuf = 0;
	g_batch = 0;
	g_ch = 0;
	g_batch_n = 0;
}

/* a corner's vertex in the batch being built: its slot, or -1 when full */
struct BuildB {
	int nv;
	uint32_t key[BATCH_V];
	int16_t hash[256];
};

static inline uint32_t corner_key(unsigned vi, unsigned u, unsigned v)
{
	return (vi << 16) | (u << 8) | v;
}

static void put_vertex(T3DVertPacked *pair, int slot, unsigned vi, unsigned u, unsigned v, int tex)
{
	const uint8_t *p = g_verts + vi * VERT_SZ;
	int16_t x = (int16_t)rd16(p), y = (int16_t)rd16(p + 2), z = (int16_t)rd16(p + 4);
	unsigned r = p[8], g = p[9], b = p[10];
	int16_t s = 0, t = 0;
	if (tex >= 0) {
		/* the PS1's (and PS2's) textured colour is x/128 */
		r = r * 2 > 255 ? 255 : r * 2;
		g = g * 2 > 255 ? 255 : g * 2;
		b = b * 2 > 255 ? 255 : b * 2;
		/* texel coordinates (10.5), onto the N64's cut-down copy */
		s = (int16_t)(((int)u << 5) >> g_tex[tex].sx);
		t = (int16_t)(((int)v << 5) >> g_tex[tex].sy);
	}
	const uint32_t rgba = (r << 24) | (g << 16) | (b << 8) | 0xff;
	if (slot & 1) {
		pair->posB[0] = x;
		pair->posB[1] = y;
		pair->posB[2] = z;
		pair->normB = 0;
		pair->rgbaB = rgba;
		pair->stB[0] = s;
		pair->stB[1] = t;
	} else {
		pair->posA[0] = x;
		pair->posA[1] = y;
		pair->posA[2] = z;
		pair->normA = 0;
		pair->rgbaA = rgba;
		pair->stA[0] = s;
		pair->stA[1] = t;
	}
}

/* a corner cut out of a primitive along a split texture's tiles */
struct CutV {
	float x, y, z, r, g, b, u, v;
};

static void put_cut(T3DVertPacked *pair, int slot, const CutV *c)
{
	const unsigned r = c->r * 2.0f > 255.0f ? 255u : (unsigned)(c->r * 2.0f + 0.5f);
	const unsigned g = c->g * 2.0f > 255.0f ? 255u : (unsigned)(c->g * 2.0f + 0.5f);
	const unsigned b = c->b * 2.0f > 255.0f ? 255u : (unsigned)(c->b * 2.0f + 0.5f);
	const uint32_t rgba = (r << 24) | (g << 16) | (b << 8) | 0xff;
	const int16_t x = (int16_t)lroundf(c->x), y = (int16_t)lroundf(c->y), z = (int16_t)lroundf(c->z);
	const int16_t s = (int16_t)lroundf(c->u * 32.0f), t = (int16_t)lroundf(c->v * 32.0f);
	if (slot & 1) {
		pair->posB[0] = x;
		pair->posB[1] = y;
		pair->posB[2] = z;
		pair->normB = 0;
		pair->rgbaB = rgba;
		pair->stB[0] = s;
		pair->stB[1] = t;
	} else {
		pair->posA[0] = x;
		pair->posA[1] = y;
		pair->posA[2] = z;
		pair->normA = 0;
		pair->rgbaA = rgba;
		pair->stA[0] = s;
		pair->stA[1] = t;
	}
}

/* Sutherland-Hodgman: the part of in[n] with sign * (u or v) >= sign * k */
static int cut_poly(const CutV *in, int n, CutV *out, int axis_v, float k, float sign)
{
	int m = 0;
	for (int i = 0; i < n; i++) {
		const CutV *a = &in[i], *b = &in[(i + 1) % n];
		const float da = sign * ((axis_v ? a->v : a->u) - k), db = sign * ((axis_v ? b->v : b->u) - k);
		if (da >= 0.0f) {
			out[m++] = *a;
		}
		if ((da >= 0.0f) != (db >= 0.0f)) {
			const float t = da / (da - db);
			CutV c;
			c.x = a->x + (b->x - a->x) * t;
			c.y = a->y + (b->y - a->y) * t;
			c.z = a->z + (b->z - a->z) * t;
			c.r = a->r + (b->r - a->r) * t;
			c.g = a->g + (b->g - a->g) * t;
			c.b = a->b + (b->b - a->b) * t;
			c.u = a->u + (b->u - a->u) * t;
			c.v = a->v + (b->v - a->v) * t;
			if (axis_v) {
				c.v = k;
			} else {
				c.u = k;
			}
			out[m++] = c;
		}
	}
	return m;
}

/* the batch builder's state */
struct BuildS {
	unsigned vp, ip; /* vertex pairs, indices used */
	Batch *bt;
	int key;         /* tex << 16 | tile of bt */
	Chunk *c;
	BuildB bb;
};

/* the current batch, or a new one, for key with room for nv more corners */
static void need_batch(BuildS *st, int key, int nv)
{
	if (st->bt && st->key == key && st->bt->nv + nv <= BATCH_V && st->bt->ntri < 60000) {
		return;
	}
	if (st->bt) {
		st->vp = st->bt->v0 + ((unsigned)st->bt->nv + 1) / 2;
	}
	Batch *bt = &g_batch[g_batch_n++];
	bt->v0 = st->vp;
	bt->i0 = st->ip;
	bt->ntri = 0;
	bt->nv = 0;
	bt->tex = (uint8_t)(key >> 16);
	bt->tile = (uint16_t)(key & 0xffff);
	bt->pad = 0;
	st->c->bn++;
	st->bt = bt;
	st->key = key;
	st->bb.nv = 0;
	memset(st->bb.hash, 0xff, sizeof(st->bb.hash));
}

/* one primitive of a texture that fits TMEM (or none), its corners shared */
static void emit_prim(BuildS *st, const uint8_t *pr, int ptex)
{
	const unsigned d = rd16(pr + 6);
	const int quad = d != 0xffff;
	const int nc = quad ? 4 : 3;
	unsigned vi[4];
	for (int k = 0; k < nc; k++) {
		vi[k] = rd16(pr + k * 2);
		if (vi[k] >= g_vert_n) {
			vi[k] = 0;
		}
	}
	need_batch(st, ptex << 16, nc);
	Batch *bt = st->bt;
	BuildB *bb = &st->bb;
	int slot[4];
	for (int k = 0; k < nc; k++) {
		const unsigned u = ptex != 255 ? pr[8 + k * 2] : 0, v = ptex != 255 ? pr[9 + k * 2] : 0;
		const uint32_t key = corner_key(vi[k], u, v);
		unsigned h = (key * 2654435761u) >> 24;
		int sl = -1;
		while (bb->hash[h] >= 0) {
			if (bb->key[bb->hash[h]] == key) {
				sl = bb->hash[h];
				break;
			}
			h = (h + 1) & 255;
		}
		if (sl < 0) {
			sl = bb->nv++;
			bb->key[sl] = key;
			bb->hash[h] = (int16_t)sl;
			put_vertex(&g_vbuf[bt->v0 + (sl >> 1)], sl, vi[k], u, v, ptex != 255 ? ptex : -1);
		}
		slot[k] = sl;
	}
	bt->nv = (uint8_t)bb->nv;
	/* a, b, c; a quad's second half d, c, b (the PS1 GPU's b, c, d) */
	g_ibuf[st->ip++] = (uint8_t)slot[0];
	g_ibuf[st->ip++] = (uint8_t)slot[1];
	g_ibuf[st->ip++] = (uint8_t)slot[2];
	bt->ntri++;
	if (quad) {
		g_ibuf[st->ip++] = (uint8_t)slot[3];
		g_ibuf[st->ip++] = (uint8_t)slot[2];
		g_ibuf[st->ip++] = (uint8_t)slot[1];
		bt->ntri++;
	}
}

/* the tiles a primitive of split texture t touches: [tx0, tx1] x [ty0, ty1] */
static void prim_tiles(const Tex *t, const uint8_t *pr, int *tx0, int *tx1, int *ty0, int *ty1)
{
	const int nc = rd16(pr + 6) != 0xffff ? 4 : 3;
	int u0 = 255, u1 = 0, v0 = 255, v1 = 0;
	for (int k = 0; k < nc; k++) {
		const int u = pr[8 + k * 2], v = pr[9 + k * 2];
		u0 = u < u0 ? u : u0;
		u1 = u > u1 ? u : u1;
		v0 = v < v0 ? v : v0;
		v1 = v > v1 ? v : v1;
	}
	const int sx = t->tw - 1 > 0 ? t->tw - 1 : 1, sy = t->th - 1 > 0 ? t->th - 1 : 1;
	*tx0 = u0 / sx;
	*tx1 = u1 / sx;
	*ty0 = v0 / sy;
	*ty1 = v1 / sy;
	/* a corner right on a seam belongs to the tile before it too */
	*tx1 = *tx1 >= t->ntx ? t->ntx - 1 : *tx1;
	*ty1 = *ty1 >= t->nty ? t->nty - 1 : *ty1;
	if (*tx0 > 0 && u0 == *tx0 * sx) {
		(*tx0)--;
	}
	if (*ty0 > 0 && v0 == *ty0 * sy) {
		(*ty0)--;
	}
}

/* the part of a primitive of split texture tex in one tile */
static void emit_cut(BuildS *st, const uint8_t *pr, int tex, int tx, int ty)
{
	const Tex *t = &g_tex[tex];
	const int quad = rd16(pr + 6) != 0xffff;
	const int nc = quad ? 4 : 3;
	/* a quad a, b, c, d is the triangles a, b, c and b, c, d: as a polygon a, b, d, c */
	static const int ord4[4] = { 0, 1, 3, 2 };
	CutV p[12], q[12];
	for (int k = 0; k < nc; k++) {
		const int ck = quad ? ord4[k] : k;
		unsigned vi = rd16(pr + ck * 2);
		vi = vi >= g_vert_n ? 0 : vi;
		const uint8_t *vp = g_verts + vi * VERT_SZ;
		p[k].x = (float)(int16_t)rd16(vp);
		p[k].y = (float)(int16_t)rd16(vp + 2);
		p[k].z = (float)(int16_t)rd16(vp + 4);
		p[k].r = vp[8];
		p[k].g = vp[9];
		p[k].b = vp[10];
		p[k].u = pr[8 + ck * 2];
		p[k].v = pr[9 + ck * 2];
	}
	const float sx = (float)(t->tw - 1), sy = (float)(t->th - 1);
	int n = nc;
	n = cut_poly(p, n, q, 0, tx * sx, 1.0f);
	n = cut_poly(q, n, p, 0, (tx + 1) * sx, -1.0f);
	n = cut_poly(p, n, q, 1, ty * sy, 1.0f);
	n = cut_poly(q, n, p, 1, (ty + 1) * sy, -1.0f);
	if (n < 3) {
		return;
	}
	need_batch(st, (tex << 16) | (ty * t->ntx + tx), n);
	Batch *bt = st->bt;
	const int s0 = bt->nv;
	for (int k = 0; k < n; k++) {
		put_cut(&g_vbuf[bt->v0 + ((s0 + k) >> 1)], s0 + k, &p[k]);
	}
	bt->nv = (uint8_t)(s0 + n);
	st->bb.nv = s0 + n;
	for (int k = 1; k + 1 < n; k++) {
		g_ibuf[st->ip++] = (uint8_t)s0;
		g_ibuf[st->ip++] = (uint8_t)(s0 + k);
		g_ibuf[st->ip++] = (uint8_t)(s0 + k + 1);
		bt->ntri++;
	}
}

static inline int prim_tex(const uint8_t *pr)
{
	return pr[16] < g_tex_n && g_tex[pr[16]].ready ? pr[16] : 255;
}

static int build_batches(void)
{
	free_batches();
	g_ch = (Chunk *)malloc(sizeof(Chunk) * (g_chunk_n ? g_chunk_n : 1));
	/* worst cases: four corners and two triangles a primitive, a batch each;
	 * a primitive cut along tiles, eight corners and six triangles a tile */
	unsigned vmax = g_prim_n * 4 + 2 * g_chunk_n + 2, imax = g_prim_n * 6 + 4, bmax = g_prim_n + g_chunk_n + 1;
	for (unsigned i = 0; i < g_prim_n; i++) {
		const uint8_t *pr = g_prims + i * PRIM_SZ;
		const int tx = prim_tex(pr);
		if (tx != 255 && g_tex[tx].split) {
			int x0, x1, y0, y1;
			prim_tiles(&g_tex[tx], pr, &x0, &x1, &y0, &y1);
			const unsigned nt = (unsigned)((x1 - x0 + 1) * (y1 - y0 + 1));
			vmax += nt * 9;
			imax += nt * 18;
			bmax += nt;
		}
	}
	g_vbuf = (T3DVertPacked *)memalign(16, sizeof(T3DVertPacked) * (vmax / 2 + 1));
	g_ibuf = (uint8_t *)malloc(imax);
	g_batch = (Batch *)malloc(sizeof(Batch) * bmax);
	static BuildS st;
	static uint16_t order[1024];
	if (!g_ch || !g_vbuf || !g_ibuf || !g_batch) {
		printf("N64: no memory for the mesh's batches (%u primitives)\n", g_prim_n);
		free_batches();
		return 0;
	}
	st.vp = st.ip = 0;
	for (unsigned ci = 0; ci < g_chunk_n; ci++) {
		const uint8_t *ch = g_chunks + ci * CHUNK_SZ;
		Chunk *c = &g_ch[ci];
		c->c[0] = rdf(ch + 8);
		c->c[1] = rdf(ch + 12);
		c->c[2] = rdf(ch + 16);
		c->r = rdf(ch + 20);
		c->node = rd16(ch + 6);
		c->b0 = g_batch_n;
		c->bn = 0;
		st.c = c;
		st.bt = 0;
		const unsigned first = rd32(ch);
		unsigned np = rd16(ch + 4) + rd16(ch + 24);
		if (first >= g_prim_n) {
			np = 0;
		} else if (first + np > g_prim_n) {
			np = g_prim_n - first;
		}
		if (np > 1024) {
			np = 1024;
		}
		/* the chunk's primitives by texture (in mesh order within one), the
		 * untextured ones last; a split texture's a tile at a time */
		for (int tx = 0; tx <= g_tex_n; tx++) {
			const int want = tx < g_tex_n ? tx : 255;
			unsigned on = 0;
			for (unsigned k = 0; k < np; k++) {
				if (prim_tex(g_prims + (first + k) * PRIM_SZ) == want) {
					order[on++] = (uint16_t)k;
				}
			}
			if (!on) {
				continue;
			}
			if (want != 255 && g_tex[want].split) {
				const Tex *t = &g_tex[want];
				for (int ty = 0; ty < t->nty; ty++) {
					for (int tt = 0; tt < t->ntx; tt++) {
						for (unsigned oi = 0; oi < on; oi++) {
							const uint8_t *pr = g_prims + (first + order[oi]) * PRIM_SZ;
							int x0, x1, y0, y1;
							prim_tiles(t, pr, &x0, &x1, &y0, &y1);
							if (tt >= x0 && tt <= x1 && ty >= y0 && ty <= y1) {
								emit_cut(&st, pr, want, tt, ty);
							}
						}
					}
				}
			} else {
				for (unsigned oi = 0; oi < on; oi++) {
					emit_prim(&st, g_prims + (first + order[oi]) * PRIM_SZ, want);
				}
			}
		}
		if (st.bt) {
			st.vp = st.bt->v0 + ((unsigned)st.bt->nv + 1) / 2;
		}
	}
	const unsigned vp = st.vp, ip = st.ip;
	/* give back what the worst case did not use */
	{
		/* (realloc would not keep the alignment the RSP's DMA needs) */
		T3DVertPacked *fit = (T3DVertPacked *)memalign(16, sizeof(T3DVertPacked) * (vp ? vp : 1));
		if (fit) {
			memcpy(fit, g_vbuf, sizeof(T3DVertPacked) * vp);
			free(g_vbuf);
			g_vbuf = fit;
		}
	}
	g_ibuf = (uint8_t *)realloc(g_ibuf, ip ? ip : 1);
	g_batch = (Batch *)realloc(g_batch, sizeof(Batch) * (g_batch_n ? g_batch_n : 1));
	data_cache_hit_writeback(g_vbuf, sizeof(T3DVertPacked) * vp);
	printf("N64 mesh: %u primitives, %u chunks -> %u batches, %u vertices (%u KiB)\n", g_prim_n, g_chunk_n, g_batch_n, vp * 2,
			(unsigned)(sizeof(T3DVertPacked) * vp / 1024));
	return 1;
}

static int parse_mesh(const uint8_t *b, unsigned sz)
{
	if (!b || sz < 32 || memcmp(b, "P1MS", 4) != 0 || rd16(b + 4) != 3) {
		return 0;
	}
	g_scale = rd16(b + 6);
	if (g_scale <= 0) {
		g_scale = 1;
	}
	g_inv_scale = 1.0f / (float)g_scale;
	g_vert_n = rd32(b + 8);
	g_prim_n = rd32(b + 12);
	g_chunk_n = rd32(b + 16);
	const unsigned extras = rd32(b + 20);
	unsigned off = 32;
	g_verts = b + off;
	off += g_vert_n * VERT_SZ;
	g_prims = b + off;
	off += g_prim_n * PRIM_SZ;
	off = (off + 3) & ~3u;
	g_chunks = b + off;
	off += g_chunk_n * CHUNK_SZ;
	if (off + extras > sz) {
		return 0;
	}
	g_mesh = b;
	if (!build_batches()) {
		return 0;
	}
	build_groups();
	parse_extras(b + off, extras);
	memset(g_ndrawn, 0, sizeof(g_ndrawn));
	for (unsigned ci = 0; ci < g_chunk_n; ci++) {
		if (g_ch[ci].node < MAX_NODES) {
			g_ndrawn[g_ch[ci].node] = 1;
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
static T3DViewport g_vp, g_vp_far;
static T3DMat4FP *g_mat_ring[MAT_FRAMES];
static unsigned g_mat_used;
static int g_frame;

void gs_draw_boot(int pal)
{
	g_pal = pal;
	display_init(RESOLUTION_320x240, DEPTH_16_BPP, 3, GAMMA_NONE, FILTERS_RESAMPLE_ANTIALIAS);
	rdpq_init();
	t3d_init((T3DInitParams){});
	g_vp = t3d_viewport_create();
	g_vp_far = t3d_viewport_create();
	for (int i = 0; i < MAT_FRAMES; i++) {
		g_mat_ring[i] = (T3DMat4FP *)malloc_uncached(sizeof(T3DMat4FP) * MAT_RING);
	}
	make_font();
}

/* ------------------------------------------------------------ view */
static void eye_now(float *x, float *y, float *z)
{
	*x = g_cam_x + g_attach_x + g_shake_x;
	*y = g_cam_y + g_attach_y + g_shake_y + g_eye_h;
	*z = g_cam_z + g_attach_z + g_shake_z;
}

/* world (Y up) to view space (X right, Y up, -Z ahead) as 3x4 rows */
static void view_matrix(float *v)
{
	float ex, ey, ez;
	eye_now(&ex, &ey, &ez);
	float zx = ex - g_look_x, zy = ey - g_look_y, zz = ez - g_look_z;
	float zl = sqrtf(zx * zx + zy * zy + zz * zz);
	if (zl < 1e-5f) {
		zl = 1.0f;
		zz = 1.0f;
	}
	zx /= zl;
	zy /= zl;
	zz /= zl;
	float xx = zz, xz = -zx; /* up x z, up = Y */
	float xl = sqrtf(xx * xx + xz * xz);
	if (xl < 1e-5f) {
		xx = 1.0f;
		xz = 0.0f;
		xl = 1.0f;
	}
	xx /= xl;
	xz /= xl;
	const float yx = zy * xz, yy = zz * xx - zx * xz, yz = -zy * xx;
	v[0] = xx;
	v[1] = 0.0f;
	v[2] = xz;
	v[3] = -(xx * ex + xz * ez);
	v[4] = yx;
	v[5] = yy;
	v[6] = yz;
	v[7] = -(yx * ex + yy * ey + yz * ez);
	v[8] = zx;
	v[9] = zy;
	v[10] = zz;
	v[11] = -(zx * ex + zy * ey + zz * ez);
	if (g_world_yaw != 0.0f) {
		const float c = cosf(g_world_yaw), s = sinf(g_world_yaw);
		const float w[12] = { c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0 };
		float o[12];
		m34_mul(v, w, o);
		memcpy(v, o, sizeof(o));
	}
}

/* a 3x4 row-major float matrix as Tiny3D's (column-major 4x4) */
static void to_t3d(const float *m, T3DMat4 *o)
{
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 3; r++) {
			o->m[c][r] = m[r * 4 + c];
		}
		o->m[c][3] = c == 3 ? 1.0f : 0.0f;
	}
}

/* a node's (or instance's) matrix for the RSP: m x 1/scale, from this
 * frame's ring (NULL when the ring is full) */
static const T3DMat4FP *ring_mat(const float *m)
{
	if (g_mat_used >= MAT_RING) {
		return 0;
	}
	float s[12];
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++) {
			s[r * 4 + c] = m[r * 4 + c] * g_inv_scale;
		}
		s[r * 4 + 3] = m[r * 4 + 3];
	}
	T3DMat4 tm;
	to_t3d(s, &tm);
	T3DMat4FP *fp = &g_mat_ring[g_frame % MAT_FRAMES][g_mat_used++];
	t3d_mat4_to_fixed(fp, &tm);
	return fp;
}

/* ------------------------------------------------------------ drawing */
struct DrawCtx {
	float view[12];  /* world to view */
	float tan_x, tan_y, sec_x, sec_y;
	float far_m;
	int far_pass;    /* drawing the backdrop: NFOG groups past the main far plane */
	int layer_pass;  /* drawing the LAYR ground layers of this priority (-8..-1), or 0 */
	int nodepth;     /* this pass draws without the depth buffer */
	int nfog;        /* the group being drawn is NFOG (no fog, no far cut) */
	float far_main;  /* the main pass's far plane (m) */
	int cur_tex;     /* -2 nothing set; 255 untextured */
	int cur_depth;   /* the depth offset set */
	int cur_fog;
	int fog;
	unsigned tris, batches;
};

/* a sphere (world, metres) against the view: 0 out, 1 in */
static int sphere_visible(const DrawCtx *c, const float *ctr, float r)
{
	if (r < 0.0f) {
		return 0;
	}
	float p[3];
	m34_point(c->view, ctr, p);
	const float d = -p[2];
	if (d + r < NEAR_M || (!c->nfog && d - r > c->far_m)) {
		return 0;
	}
	const float lx = d * c->tan_x + r * c->sec_x, ly = d * c->tan_y + r * c->sec_y;
	if (p[0] > lx || -p[0] > lx || p[1] > ly || -p[1] > ly) {
		return 0;
	}
	return 1;
}

static void set_tex(DrawCtx *c, int tex, int tile)
{
	const int key = (tex << 16) | tile;
	if (key == c->cur_tex) {
		return;
	}
	if (tex == 255) {
		t3d_state_set_drawflags((enum T3DDrawFlags)(T3D_FLAG_SHADED | (c->nodepth ? 0 : T3D_FLAG_DEPTH)));
		rdpq_mode_begin();
		rdpq_mode_combiner(RDPQ_COMBINER1((0, 0, 0, SHADE), (0, 0, 0, 1)));
		rdpq_mode_tlut(TLUT_NONE);
		rdpq_mode_alphacompare(0);
		rdpq_mode_end();
	} else {
		const Tex *t = &g_tex[tex];
		t3d_state_set_drawflags((enum T3DDrawFlags)(T3D_FLAG_SHADED | T3D_FLAG_TEXTURED | (c->nodepth ? 0 : T3D_FLAG_DEPTH)));
		rdpq_mode_begin();
		rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, 0, SHADE, 0), (0, 0, 0, TEX0)));
		rdpq_mode_tlut(t->tlut ? TLUT_RGBA16 : TLUT_NONE);
		rdpq_mode_alphacompare(1);
		rdpq_mode_end();
		if (t->tlut) {
			rdpq_tex_upload_tlut(t->tlut, 0, t->tlut_n);
		}
		rdpq_texparms_t parms;
		memset(&parms, 0, sizeof(parms));
		if (t->tiled) {
			parms.s.repeats = REPEAT_INFINITE;
			parms.t.repeats = REPEAT_INFINITE;
		}
		if (t->split) {
			/* one tile of it (its texel coordinates stay the whole texture's) */
			const int tx = tile % t->ntx, ty = tile / t->ntx;
			const int s0 = tx * (t->tw - 1), t0 = ty * (t->th - 1);
			const int s1 = s0 + t->tw > t->w ? t->w : s0 + t->tw, t1 = t0 + t->th > t->h ? t->h : t0 + t->th;
			rdpq_tex_upload_sub(TILE0, &t->surf, &parms, s0, t0, s1, t1);
		} else {
			rdpq_tex_upload(TILE0, &t->surf, &parms);
		}
	}
	c->cur_tex = key;
}

static void set_fog(DrawCtx *c)
{
	const int fog = c->fog && !c->far_pass && !c->nfog;
	if (fog != c->cur_fog) {
		t3d_tri_sync();
		rdpq_mode_fog(fog ? RDPQ_FOG_STANDARD : 0);
		c->cur_fog = fog;
	}
}

static void draw_chunk(DrawCtx *c, unsigned ci)
{
	const Chunk *ch = &g_ch[ci];
	for (unsigned bi = ch->b0; bi < ch->b0 + ch->bn; bi++) {
		const Batch *b = &g_batch[bi];
		if (!b->ntri) {
			continue;
		}
		set_tex(c, b->tex, b->tile);
		t3d_vert_load(&g_vbuf[b->v0], 0, b->nv);
		const uint8_t *ix = g_ibuf + b->i0;
		for (unsigned t = 0; t < b->ntri; t++, ix += 3) {
			t3d_tri_draw(ix[0], ix[1], ix[2]);
		}
		t3d_tri_sync();
		c->tris += b->ntri;
		c->batches++;
	}
}

static void set_depth(DrawCtx *c, int node)
{
	const int pri = node < MAX_NODES ? g_nlayer[node] : 0;
	/* a decal nearer a step (ground layers draw in passes of their own) */
	const int ofs = pri > 0 ? -pri * DECAL_DEPTH : 0;
	if (ofs != c->cur_depth) {
		t3d_state_set_depth_offset((int16_t)ofs);
		c->cur_depth = ofs;
	}
}

static const float k_ident[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };

static void draw_groups(DrawCtx *c)
{
	const T3DMat4FP *base = ring_mat(k_ident);
	int loaded = -2; /* the node whose matrix is set (-1 the base) */
	for (unsigned gi = 0; gi < g_grp_n; gi++) {
		const Group *g = &g_grp[gi];
		const unsigned node = g->node;
		if (node < MAX_NODES && (g_isrc[node] || (g_nodes_live && g_nhide[node]))) {
			continue;
		}
		const int nfog = node < MAX_NODES && g_nfog[node];
		if (c->far_pass && !nfog) {
			continue;
		}
		/* ground layers (LAYR < 0) in their own passes, under everything */
		const int pri = node < MAX_NODES ? g_nlayer[node] : 0;
		if (c->layer_pass ? pri != c->layer_pass : (!c->far_pass && pri < 0)) {
			continue;
		}
		c->nfog = nfog;
		const int moved = node < MAX_NODES && g_nodes_live && g_nm_on[node];
		const float *m = moved ? g_nm[node] : k_ident;
		float wc[3];
		m34_point(m, g->c, wc);
		const float sc = moved ? g_nscale[node] : 1.0f;
		if (!sphere_visible(c, wc, g->r * sc)) {
			continue;
		}
		if (nfog) {
			/* an NFOG group reaching past the main far plane is backdrop:
			 * drawn first, under everything; one within it draws with the
			 * depth buffer, unfogged */
			float p[3];
			m34_point(c->view, wc, p);
			const int beyond = -p[2] + g->r * sc > c->far_main;
			if (beyond != c->far_pass) {
				continue;
			}
		}
		set_fog(c);
		const int want = moved ? (int)node : -1;
		if (want != loaded) {
			const T3DMat4FP *fp = moved ? ring_mat(m) : base;
			if (!fp) {
				continue;
			}
			t3d_matrix_set(fp, true);
			loaded = want;
		}
		set_depth(c, node);
		for (unsigned ci = g->first; ci < (unsigned)g->first + g->n; ci++) {
			if (g_ch[ci].r < 0.0f) {
				continue;
			}
			m34_point(m, g_ch[ci].c, wc);
			if (sphere_visible(c, wc, g_ch[ci].r * sc)) {
				draw_chunk(c, ci);
			}
		}
	}
}

static void draw_instances(DrawCtx *c)
{
	set_depth(c, MAX_NODES);
	c->nfog = 0;
	set_fog(c);
	for (unsigned i = 0; i < g_inst_n; i++) {
		const unsigned nd = rd16(g_inst + i * 52);
		if (nd >= MAX_NODES || g_isrc[nd] != 2 || g_nhide[nd] || g_isph[nd][3] < 0.0f) {
			continue;
		}
		float w[12];
		if (g_nm_on[nd]) {
			m34_mul(g_nm[nd], g_imat[i], w);
		} else {
			memcpy(w, g_imat[i], sizeof(w));
		}
		float wc[3];
		m34_point(w, g_isph[nd], wc);
		const float sc = max_scale(w);
		if (!sphere_visible(c, wc, g_isph[nd][3] * sc)) {
			continue;
		}
		const T3DMat4FP *fp = ring_mat(w);
		if (!fp) {
			break;
		}
		t3d_matrix_set(fp, true);
		for (unsigned ci = g_isrc_c0[nd]; ci < g_isrc_c1[nd]; ci++) {
			if (g_ch[ci].r < 0.0f) {
				continue;
			}
			m34_point(w, g_ch[ci].c, wc);
			if (sphere_visible(c, wc, g_ch[ci].r * sc)) {
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
	if (y <= 0.0f) {
		return 0;
	}
	if (y >= HUD_H) {
		return SCR_H;
	}
	return SAFE_Y + (int)(y * ((float)(SCR_H - 2 * SAFE_Y) / HUD_H));
}

static int g_hud_mode; /* 1 rects, 2 text */

static void hud_rect(int x0, int y0, int x1, int y1, int r, int g, int b)
{
	x0 = x0 < 0 ? 0 : x0;
	y0 = y0 < 0 ? 0 : y0;
	x1 = x1 > SCR_W ? SCR_W : x1;
	y1 = y1 > SCR_H ? SCR_H : y1;
	if (x1 <= x0 || y1 <= y0) {
		return;
	}
	if (g_hud_mode != 1) {
		rdpq_set_mode_standard();
		rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
		g_hud_mode = 1;
	}
	rdpq_set_prim_color(col(r, g, b, 255));
	rdpq_fill_rectangle(x0, y0, x1, y1);
}

/* one glyph, px screen pixels per font pixel; px 0 is the 3x5 font */
static void hud_glyph(int x, int y, int ch, int px)
{
	if (ch < 32 || ch > 126) {
		ch = '?';
	}
	const int c = ch - 32;
	int u = (c % 16) * 8, v = (c / 16) * 8, gw = 5, gh = 7, font = 1;
	if (px == 0) {
		u = (c % 32) * 4;
		v = (c / 32) * 6;
		gw = 3;
		gh = 5;
		px = 1;
		font = 2;
	}
	if (font != g_font_cur) {
		rdpq_tex_upload(TILE0, font == 1 ? &g_font_big : &g_font_small, NULL);
		g_font_cur = font;
	}
	const int w = gw * px, h = gh * px;
	rdpq_texture_rectangle_scaled(TILE0, x, y, x + w, y + h, u, v, u + gw, v + gh);
}

static void hud_text(const HudQuad *h, int r, int g, int b)
{
	if (g_hud_mode != 2) {
		rdpq_set_mode_standard();
		rdpq_mode_combiner(RDPQ_COMBINER1((0, 0, 0, PRIM), (TEX0, 0, PRIM, 0)));
		rdpq_mode_alphacompare(1);
		rdpq_mode_filter(FILTER_POINT);
		g_hud_mode = 2;
		g_font_cur = 0;
	}
	rdpq_set_prim_color(col(r, g, b, 255));
	const int sc = h->scale > 0 ? h->scale : 1;
	const int px = sc / 2;
	const int adv = px ? 6 * px : 4;
	const char *t = h->text;
	int lines = 1;
	for (const char *p = t; *p; p++) {
		if (*p == '\n') {
			lines++;
		}
	}
	const int bx = hx((float)h->x);
	const int bw = hx((float)h->w);
	const int line_ps2 = 9 * sc;
	int y_ps2 = h->y + 2;
	if (h->styled && h->kind != 1) {
		y_ps2 = h->y + (h->h - (lines * line_ps2 - 2 * sc)) / 2;
	}
	const int lift = px ? 0 : 1;
	const int min_pitch = px ? 8 * px : 6;
	int last_y = -1000;
	while (*t) {
		int n = 0;
		while (t[n] && t[n] != '\n') {
			n++;
		}
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
			const int gx = x + k * adv;
			if (t[k] != ' ' && gx >= 0 && gx + 6 * (px ? px : 1) <= SCR_W && ly >= 0 && ly + 8 * (px ? px : 1) <= SCR_H) {
				hud_glyph(gx, ly, (unsigned char)t[k], px);
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
	g_hud_mode = 0;
	for (int i = 0; i < 16; i++) {
		const HudQuad *h = &g_hudq[i];
		if (!h->used || h->hidden) {
			continue;
		}
		if (!h->styled || h->kind != 1) {
			hud_rect(hx((float)h->x), hy((float)h->y), hx((float)(h->x + h->w)), hy((float)(h->y + h->h)), h->r, h->g, h->b);
		}
		if (h->text[0]) {
			if (h->styled) {
				hud_text(h, h->tr, h->tg, h->tb);
			} else {
				hud_text(h, 255, 255, 255);
			}
		}
	}
	if (g_fade_a > 0) {
		const int a = g_fade_a >= 128 ? 255 : g_fade_a * 2;
		rdpq_set_mode_standard();
		rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
		rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
		rdpq_set_prim_color(col(g_fade_r, g_fade_g, g_fade_b, a));
		rdpq_fill_rectangle(0, 0, SCR_W, SCR_H);
	}
}

/* ------------------------------------------------------------ frame */
/* Split screen (gs_draw_set_views): up to four views of the one scene, each
 * with its own camera, in its own part of the frame (2: top and bottom
 * halves; 3-4: quarters), as the PS2 guest has it. The HUD goes on top once. */
#define MAX_VIEWS 4
struct ViewCam {
	float ex, ey, ez, tx, ty, tz, fov;
};
static ViewCam g_views[MAX_VIEWS];
static int g_view_n;
static T3DViewport g_vpv[MAX_VIEWS], g_vpv_far[MAX_VIEWS];
static int g_vpv_made;

static void view_rect(int i, int n, int *x, int *y, int *w, int *h)
{
	if (n == 2) {
		*x = 0;
		*y = i * (SCR_H / 2);
		*w = SCR_W;
		*h = SCR_H / 2;
		return;
	}
	*x = (i & 1) * (SCR_W / 2);
	*y = (i >> 1) * (SCR_H / 2);
	*w = SCR_W / 2;
	*h = SCR_H / 2;
}

/* the scene from the camera as it stands, into vp (and vp_far for the
 * backdrop) covering w x h pixels; split: one of several views (no safe
 * area: the FOV spans the view) */
static unsigned draw_view(T3DViewport *vp, T3DViewport *vp_far, int w, int h, int split)
{
	DrawCtx c;
	memset(&c, 0, sizeof(c));
	const float half = tanf(g_fov * 0.5f * 3.14159265f / 180.0f);
	/* the PS1's framing for one view: the FOV spans the middle 224 lines */
	c.tan_y = split ? half : half * ((float)SCR_H * 0.5f) / ((float)SCR_H * 0.5f - (float)SAFE_Y);
	c.tan_x = c.tan_y * (float)w / (float)h;
	c.sec_x = sqrtf(1.0f + c.tan_x * c.tan_x);
	c.sec_y = sqrtf(1.0f + c.tan_y * c.tan_y);
	const float fov_v = 2.0f * atanf(c.tan_y);
	c.far_m = g_fog_on ? g_fog_far : 400.0f;
	c.far_main = c.far_m + 20.0f;
	view_matrix(c.view);
	T3DMat4 vm;
	to_t3d(c.view, &vm);
	const uint8_t white[4] = { 255, 255, 255, 255 };
	/* the backdrop (NFOG): first, without the depth buffer, far plane far away */
	t3d_viewport_set_projection(vp_far, fov_v, 1.0f, 4000.0f);
	t3d_viewport_set_view_matrix(vp_far, &vm);
	t3d_viewport_attach(vp_far);
	t3d_light_set_ambient(white);
	t3d_light_set_count(0);
	t3d_fog_set_enabled(false);
	t3d_matrix_push_pos(1);
	c.cur_tex = -2;
	c.cur_fog = 0;
	c.far_pass = 1;
	c.nodepth = 1;
	rdpq_mode_fog(0);
	rdpq_mode_zbuf(false, false);
	draw_groups(&c);
	t3d_matrix_pop(1);
	/* the rest with the depth buffer, and the fog */
	t3d_tri_sync();
	rdpq_mode_zbuf(true, true);
	t3d_viewport_set_projection(vp, fov_v, NEAR_M, c.far_main);
	t3d_viewport_set_view_matrix(vp, &vm);
	t3d_viewport_attach(vp);
	c.fog = g_fog_on && g_fog_far > g_fog_near;
	if (c.fog) {
		rdpq_set_fog_color(col(g_fog_r, g_fog_g, g_fog_b, 255));
		t3d_fog_set_range(g_fog_near, g_fog_far);
		t3d_fog_set_enabled(true);
	}
	t3d_matrix_push_pos(1);
	c.far_pass = 0;
	/* the ground layers, lowest priority first, without the depth buffer:
	 * as on the PS1, a floor and the paths on it never cover what stands on
	 * them (a kart rides a few cm into its floor) */
	rdpq_mode_zbuf(false, false);
	c.nodepth = 1;
	for (int pri = -8; pri <= -1; pri++) {
		c.cur_tex = -2;
		c.layer_pass = pri;
		draw_groups(&c);
	}
	c.layer_pass = 0;
	c.nodepth = 0;
	t3d_tri_sync();
	rdpq_mode_zbuf(true, true);
	c.cur_tex = -2;
	draw_groups(&c);
	draw_instances(&c);
	t3d_matrix_pop(1);
	if (c.cur_depth) {
		t3d_state_set_depth_offset(0);
	}
	if (c.fog) {
		t3d_fog_set_enabled(false);
	}
	t3d_tri_sync();
	return c.tris;
}

void gs_draw_scene(void)
{
	surface_t *fb = display_get();
	rdpq_attach(fb, display_get_zbuf());
	t3d_frame_start();
	t3d_screen_clear_color(col(g_clear_r, g_clear_g, g_clear_b, 255));
	t3d_screen_clear_depth();
	g_frame++;
	g_mat_used = 0;
	unsigned tris = 0;
	if (g_ready && g_view_n < 2) {
		tris = draw_view(&g_vp, &g_vp_far, SCR_W, SCR_H, 0);
	} else if (g_ready) {
		if (!g_vpv_made) {
			for (int v = 0; v < MAX_VIEWS; v++) {
				g_vpv[v] = t3d_viewport_create();
				g_vpv_far[v] = t3d_viewport_create();
			}
			g_vpv_made = 1;
		}
		/* each view with its own camera; the scripted one comes back after */
		const float kx = g_cam_x, ky = g_cam_y, kz = g_cam_z;
		const float lx = g_look_x, ly = g_look_y, lz = g_look_z, kf = g_fov;
		const float ax = g_attach_x, ay = g_attach_y, az = g_attach_z, eh = g_eye_h;
		g_attach_x = g_attach_y = g_attach_z = 0.0f;
		g_eye_h = 0.0f;
		for (int v = 0; v < g_view_n; v++) {
			int vx, vy, vw, vh;
			view_rect(v, g_view_n, &vx, &vy, &vw, &vh);
			const ViewCam *vc = &g_views[v];
			g_cam_x = vc->ex;
			g_cam_y = vc->ey;
			g_cam_z = vc->ez;
			g_look_x = vc->tx;
			g_look_y = vc->ty;
			g_look_z = vc->tz;
			g_fov = vc->fov;
			t3d_viewport_set_area(&g_vpv[v], vx, vy, vw, vh);
			t3d_viewport_set_area(&g_vpv_far[v], vx, vy, vw, vh);
			tris += draw_view(&g_vpv[v], &g_vpv_far[v], vw, vh, 1);
		}
		g_cam_x = kx;
		g_cam_y = ky;
		g_cam_z = kz;
		g_look_x = lx;
		g_look_y = ly;
		g_look_z = lz;
		g_fov = kf;
		g_attach_x = ax;
		g_attach_y = ay;
		g_attach_z = az;
		g_eye_h = eh;
		/* the HUD over the whole frame */
		rdpq_set_scissor(0, 0, SCR_W, SCR_H);
	}
	(void)tris;
	t3d_tri_sync();
	draw_hud();
#ifdef N64_STATS
	{
		/* dev: frames a second (the emulated clock) and triangles, top left */
		static uint64_t s_last;
		static float s_ms = 33.0f;
		static unsigned s_tris;
		const uint64_t now = get_ticks();
		if (s_last) {
			const float ms = (float)(now - s_last) / (float)(TICKS_PER_SECOND / 1000);
			s_ms = s_ms * 0.9f + ms * 0.1f;
		}
		s_last = now;
		s_tris = tris;
		HudQuad q;
		memset(&q, 0, sizeof(q));
		q.used = 1;
		q.x = 8;
		q.y = 0;
		q.w = 300;
		q.h = 20;
		q.styled = 1;
		q.kind = 1;
		q.scale = 2;
		snprintf(q.text, sizeof(q.text), "%d.%d FPS %u TRI", (int)(1000.0f / s_ms), (int)(10000.0f / s_ms) % 10, s_tris);
		hud_text(&q, 255, 255, 0);
		float ex, ey, ez;
		eye_now(&ex, &ey, &ez);
		q.y = 16;
		snprintf(q.text, sizeof(q.text), "E %d %d %d L %d %d %d", (int)ex, (int)ey, (int)ez, (int)g_look_x, (int)g_look_y, (int)g_look_z);
		hud_text(&q, 255, 255, 0);
	}
#endif
	rdpq_detach_show();
}

/* ------------------------------------------------------------ API */
void gs_draw_set_gtex_reload(unsigned char *(*fn)(const unsigned char *mesh, unsigned *size))
{
	(void)fn; /* the N64 keeps its own copy of the textures */
}

int gs_draw_init(const unsigned char *mesh, unsigned mesh_sz, const unsigned char *gtex, unsigned gtex_sz,
		const unsigned char *node, unsigned node_sz)
{
	g_ready = 0;
	g_fade_a = g_fade_r = g_fade_g = g_fade_b = 0;
	memset(g_hudq, 0, sizeof(g_hudq));
	memset(g_nofs, 0, sizeof(g_nofs));
	memset(g_nrot, 0, sizeof(g_nrot));
	/* the RDP may still read the old textures and vertices */
	rspq_wait();
	parse_camera(node, node_sz);
	if (gtex) {
		parse_textures(gtex, gtex_sz);
	} else {
		static int warned;
		if (!warned++) {
			printf("N64: a scene without its GTEX (drawn untextured)\n");
		}
		free_textures();
	}
	if (!parse_mesh(mesh, mesh_sz)) {
		printf("N64: mesh did not parse\n");
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
		g_nscale[node] = max_scale(m);
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

void gs_draw_set_views(int n)
{
	g_view_n = n >= 2 && n <= MAX_VIEWS ? n : 0;
}

void gs_draw_view_look(int i, float ex, float ey, float ez, float tx, float ty, float tz, float fov_deg)
{
	if (i < 0 || i >= MAX_VIEWS) {
		return;
	}
	ViewCam *v = &g_views[i];
	v->ex = ex;
	v->ey = ey;
	v->ez = ez;
	v->tx = tx;
	v->ty = ty;
	v->tz = tz;
	v->fov = fov_deg > 1.0f && fov_deg < 170.0f ? fov_deg : 55.0f;
}

int gs_draw_view_count(void)
{
	return g_view_n;
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
