// MIT. The PS2 guest's draw API (gs_draw.h) on the Nintendo DS: the scene's
// P1MS primitives (the PS1 forms, internal/platforms/ps1/gdirpack.go) through
// the 3D engine on the top screen, P1TX textures in texture VRAM as the DS's
// own palette formats, and the HUD (rects, the 5x7 font and a 3x5 one for
// small text) drawn by the CPU into an 8-bit bitmap background over the 3D.
//
// At load each culling chunk's primitives are put in runs: one texture, all
// quads or all triangles. A frame culls groups of chunks and chunks by their
// spheres in fixed point (the ARM9 has no FPU), sorts what is left near to
// far (the DS draws 2048 polygons a frame at most: when a scene has more,
// the far ones go) and sends each run straight to the geometry engine's
// ports, from the mesh as it is: a P1MS position is a DS v16 as it stands,
// and each node's matrix scales it back to metres.
//
// Depth: a Z-buffer. The PS1 and N64 guests draw the backdrop (NFOG nodes
// past the far plane) and the ground layers (LAYR < 0) without the depth
// buffer, under everything; the DS cannot turn the depth test off, so the
// backdrop goes at the back of the depth range (each group a step nearer
// than the last, so it keeps the painter's order) and layers and decals
// (LAYR > 0) are pushed a fraction of their distance back or forward.

#include "gs_draw.h"

#include <nds.h>

#include <math.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the per-chunk loops: ARM code in ITCM (no wait states, no cache misses) */
#define HOT ITCM_CODE ARM_CODE
#define HOT_INLINE static inline __attribute__((always_inline)) ARM_CODE

#define SCR_W 256
#define SCR_H 192
#define HUD_W 640.0f
#define HUD_H 448.0f
#define MAX_TEX 64
#ifdef GDIR_MAX_NODES
#define MAX_NODES GDIR_MAX_NODES
#else
#define MAX_NODES 512
#endif
#ifndef NEAR_M
#define NEAR_M 0.5f
#endif
#define MAX_POLYS 2000   /* of the 2048 the DS keeps a frame (clipping adds some) */
#define MAX_VERTS 6000   /* of 6144 */
#define TEX_BUDGET (500 * 1024) /* texture VRAM (banks A-D), less what allocation loses */
#define DEPTH_TOP 0.97f  /* the scene's depth range ends here; the backdrop is behind */
#define LAYER_STEP 1     /* a LAYR step: 1/4096 of the distance */

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

/* font: the PS2 guest's 5x7 table */
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
/* The PS2's smallest text is under 5 pixels a character at 256 wide: that
 * size gets this 3x5 font (lower case as small capitals). */
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

static inline int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static inline uint16_t rgb15(int r, int g, int b)
{
	return (uint16_t)(clampi(r, 0, 255) >> 3 | (clampi(g, 0, 255) >> 3) << 5 | (clampi(b, 0, 255) >> 3) << 10);
}

/* ------------------------------------------------------------ textures */
struct Tex {
	int ready;
	int w, h, psm; /* as the pack has it (psm 0 T4, 1 T8, 2 CT16) */
	int tiled;
	int cw, ch;    /* the pack's texture as the DS copy has it (resampled) */
	int W, H;      /* the DS copy: powers of two, cw x ch in the top left */
	int32_t kx, ky; /* pack texel -> DS t16 (12.4), as 16.16 */
	int name;      /* the GL texture */
	uint32_t fmt;  /* its TEXIMAGE_PARAM */
	int pal;       /* its PLTT_BASE, -1 none */
};
static Tex g_tex[MAX_TEX];
static int g_tex_n;

static int pow2_up(int v)
{
	int p = 8;
	while (p < v && p < 1024) {
		p <<= 1;
	}
	return p;
}

/* the nearer power of two (a wrapped texture is resampled to it) */
static int pow2_near(int v)
{
	const int up = pow2_up(v);
	return (up > 8 && up - v > v - up / 2) ? up / 2 : up;
}

static void free_textures(void)
{
	glResetTextures();
	memset(g_tex, 0, sizeof(g_tex));
	g_tex_n = 0;
}

static int tex_bytes(const Tex *t)
{
	const int bits = t->psm == 0 ? 4 : (t->psm == 1 ? 8 : 16);
	return t->W * t->H * bits / 8;
}

/* P1TX: "P1TX", u16 abi, u16 count, then per texture u16 w, h, u8 psm
 * (0 T4, 1 T8, 2 CT16), u8 flags (1 tiled), u16 clut entries, u32 data
 * bytes, data (rows of w texels; T4 low nibble first, as the DS has it),
 * clut (PS1 colours: the DS's RGB15, 0x0000 the only see-through one). */
static void parse_textures(const uint8_t *b, unsigned sz)
{
	free_textures();
	if (!b || sz < 8 || memcmp(b, "P1TX", 4) != 0) {
		return;
	}
	const unsigned n = rd16(b + 6);
	const uint8_t *data_at[MAX_TEX], *clut_at[MAX_TEX];
	unsigned clut_n[MAX_TEX];
	unsigned off = 8;
	/* sizes first: the DS copies must fit texture VRAM together */
	for (unsigned i = 0; i < n && i < MAX_TEX && off + 12 <= sz; i++) {
		Tex *t = &g_tex[i];
		t->w = rd16(b + off);
		t->h = rd16(b + off + 2);
		t->psm = b[off + 4];
		t->tiled = (b[off + 5] & 1) != 0;
		clut_n[i] = rd16(b + off + 6);
		const unsigned data_n = rd32(b + off + 8);
		off += 12;
		data_at[i] = b + off;
		off += data_n;
		clut_at[i] = b + off;
		off += clut_n[i] * 2;
		g_tex_n = (int)i + 1;
		if (off > sz || t->w <= 0 || t->h <= 0 || t->w > 1024 || t->h > 1024 || t->psm > 2) {
			continue;
		}
		const int bits = t->psm == 0 ? 4 : (t->psm == 1 ? 8 : 16);
		if (data_n < (unsigned)(t->w * t->h * bits / 8)) {
			continue;
		}
		if (t->tiled) {
			t->W = t->cw = pow2_near(t->w);
			t->H = t->ch = pow2_near(t->h);
		} else {
			t->cw = t->w;
			t->ch = t->h;
			t->W = pow2_up(t->w);
			t->H = pow2_up(t->h);
		}
		t->ready = 1;
	}
	for (;;) {
		int total = 0, big = -1;
		for (int i = 0; i < g_tex_n; i++) {
			if (!g_tex[i].ready) {
				continue;
			}
			total += tex_bytes(&g_tex[i]);
			if (big < 0 || tex_bytes(&g_tex[i]) > tex_bytes(&g_tex[big])) {
				big = i;
			}
		}
		if (total <= TEX_BUDGET || big < 0) {
			break;
		}
		/* the biggest at half size on its longer side */
		Tex *t = &g_tex[big];
		if (t->W <= 8 && t->H <= 8) {
			break;
		}
		if (t->W >= t->H) {
			t->cw = (t->cw + 1) / 2;
			t->W = t->tiled ? t->W / 2 : pow2_up(t->cw);
			t->cw = t->tiled ? t->W : t->cw;
		} else {
			t->ch = (t->ch + 1) / 2;
			t->H = t->tiled ? t->H / 2 : pow2_up(t->ch);
			t->ch = t->tiled ? t->H : t->ch;
		}
	}
	for (int i = 0; i < g_tex_n; i++) {
		Tex *t = &g_tex[i];
		if (!t->ready) {
			continue;
		}
		t->ready = 0;
		const int bits = t->psm == 0 ? 4 : (t->psm == 1 ? 8 : 16);
		const int cells = bits == 4 ? 16 : 256;
		/* a palette texture's see-through colour (0x0000) goes to index 0,
		 * the one the DS leaves out; index 0's colour takes its place */
		uint8_t remap[256];
		uint16_t pal[256];
		int clear = 0;
		for (int k = 0; k < 256; k++) {
			remap[k] = (uint8_t)k;
			pal[k] = 0;
		}
		if (bits < 16) {
			const uint8_t *clut = clut_at[i];
			int first_clear = -1;
			for (int k = 0; k < cells && k < (int)clut_n[i]; k++) {
				pal[k] = (uint16_t)(clut[k * 2] | (clut[k * 2 + 1] << 8));
				if (pal[k] == 0 && first_clear < 0) {
					first_clear = k;
				}
			}
			if ((int)clut_n[i] < cells && first_clear < 0) {
				first_clear = (int)clut_n[i]; /* unused entries are black, not clear */
				pal[first_clear] = 0x8000;
			}
			if (first_clear >= 0) {
				clear = 1;
				const uint16_t c0 = pal[0];
				/* every clear index to 0; 0 to where the first clear one was */
				for (int k = 0; k < cells; k++) {
					if (k < (int)clut_n[i] && pal[k] == 0) {
						remap[k] = 0;
					}
				}
				remap[0] = (uint8_t)(c0 == 0 ? 0 : first_clear);
				pal[first_clear] = c0;
				pal[0] = 0;
			}
		}
		const int stride = t->W * bits / 8;
		uint8_t *px = (uint8_t *)malloc((size_t)stride * t->H);
		if (!px) {
			printf("DS: no memory to convert texture %d\n", i);
			continue;
		}
		memset(px, 0, (size_t)stride * t->H);
		const uint8_t *data = data_at[i];
		for (int y = 0; y < t->ch; y++) {
			const int sy = y * t->h / t->ch;
			for (int x = 0; x < t->cw; x++) {
				const int sxx = x * t->w / t->cw;
				if (bits == 4) {
					const uint8_t s = data[(sy * t->w + sxx) >> 1];
					const unsigned idx = remap[(sxx & 1) ? (s >> 4) : (s & 15)];
					px[y * stride + (x >> 1)] |= (uint8_t)((x & 1) ? (idx << 4) : idx);
				} else if (bits == 8) {
					px[y * stride + x] = remap[data[sy * t->w + sxx]];
				} else {
					const uint8_t *s = data + (sy * t->w + sxx) * 2;
					const uint16_t c = (uint16_t)(s[0] | (s[1] << 8));
					const uint16_t d = c ? (uint16_t)(c | 0x8000) : 0;
					px[y * stride + x * 2] = (uint8_t)d;
					px[y * stride + x * 2 + 1] = (uint8_t)(d >> 8);
				}
			}
		}
		int params = TEXGEN_TEXCOORD;
		if (t->tiled) {
			params |= GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T;
		}
		if (clear) {
			params |= GL_TEXTURE_COLOR0_TRANSPARENT;
		}
		const GL_TEXTURE_TYPE_ENUM type = bits == 4 ? GL_RGB16 : (bits == 8 ? GL_RGB256 : GL_RGBA);
		glGenTextures(1, &t->name);
		glBindTexture(0, t->name);
		DC_FlushRange(px, (size_t)stride * t->H);
		int ok = glTexImage2D(0, 0, type, t->W, t->H, 0, params, px);
		if (ok && bits < 16) {
			DC_FlushRange(pal, sizeof(pal));
			ok = glColorTableEXT(0, 0, (uint16_t)cells, 0, 0, pal);
		}
		free(px);
		if (!ok) {
			printf("DS: texture %d (%dx%d) did not fit VRAM\n", i, t->W, t->H);
			continue;
		}
		t->fmt = glGetTexParameter();
		t->pal = -1;
		if (bits < 16) {
			glGetColorTableParameterEXT(0, GL_COLOR_TABLE_FORMAT_EXT, &t->pal);
		}
		/* a pack texel to the DS's (12.4, at the copy's scale) */
		t->kx = (int32_t)(((int64_t)t->cw << 20) / t->w);
		t->ky = (int32_t)(((int64_t)t->ch << 20) / t->h);
		t->ready = 1;
	}
	glBindTexture(0, 0);
}

/* ------------------------------------------------------------ mesh */
static const uint8_t *g_mesh;
static int g_scale = 1024;
static unsigned g_vert_n, g_prim_n, g_chunk_n;
#define VERT_SZ 12  /* i16 x y z pad, u8 r g b pad */
#define PRIM_SZ 20  /* u16 a b c d (d 0xffff: a triangle), u8 uv[8], tex, flags, pad */
#define CHUNK_SZ 28 /* u32 first, u16 quads, u16 node, f32 cx cy cz r, u16 triangles, pad */
static const uint8_t *g_verts;
static const uint8_t *g_prims;
static const uint8_t *g_chunks;
static uint16_t *g_vcol; /* per vertex: [0] untextured, [1] textured (the PS1's x/128) */

/* a run: one texture, quads or triangles, prims g_order[o0 .. o0 + n) */
struct Run {
	uint32_t o0;
	uint16_t n;
	uint8_t tex;  /* 255: untextured */
	uint8_t quad;
};
static Run *g_run;
static unsigned g_run_n;
static uint32_t *g_order;
struct Chunk {
	int32_t c[3], r; /* sphere in 1/16 m (r < 0: empty) */
	uint32_t run0;
	uint16_t nrun;
	uint16_t node;
	uint16_t polys;
};
static Chunk *g_ch;
/* runs of up to 16 neighbouring chunks of one node in one sphere */
struct Group {
	int32_t c[3], r;
	uint16_t first, n, node;
};
static Group *g_grp;
static unsigned g_grp_n;
static uint8_t g_ndrawn[MAX_NODES];

static const uint8_t *g_inst;
static unsigned g_inst_n;
static int32_t (*g_imat)[12]; /* each instance's matrix: 4.12 rotation, 20.12 metres */
static int32_t *g_isc;        /* and its largest axis scale (4.12) */
static uint8_t g_isrc[MAX_NODES];
static uint16_t g_isrc_c0[MAX_NODES], g_isrc_c1[MAX_NODES];
static int32_t g_isph[MAX_NODES][4]; /* 1/16 m */
static uint8_t g_nfog[MAX_NODES];
static int8_t g_nlayer[MAX_NODES];
static int g_ready;

/* node matrices from the VM (3x4 row-major, metres) as the FM form below:
 * rotation 4.12, translation 20.12, and the largest axis scale (4.12) */
static int32_t g_nmf[MAX_NODES][12];
static int32_t g_nsc[MAX_NODES];
static uint8_t g_nm_on[MAX_NODES];
static uint8_t g_nhide[MAX_NODES];
static int g_nodes_live;

/* a float to 20.12 from its bits (no FPU: a soft multiply and convert
 * would be ten times this), truncated, saturating */
static inline int32_t f2fx(float f)
{
	uint32_t u;
	memcpy(&u, &f, 4);
	const int e = (int)((u >> 23) & 255);
	if (e < 100) {
		return 0;
	}
	const int32_t mant = (int32_t)((u & 0x7fffffu) | 0x800000u);
	const int sh = e - 138; /* value = mant * 2^(e - 150) * 4096 */
	int32_t v;
	if (sh >= 8) {
		v = 0x7fffffff;
	} else if (sh >= 0) {
		v = mant << sh;
	} else {
		v = -sh >= 32 ? 0 : mant >> -sh;
	}
	return (u & 0x80000000u) ? -v : v;
}

/* the largest axis scale of a 4.12 rotation (columns' lengths), 4.12 */
static uint32_t isqrt32(uint32_t x)
{
	uint32_t r = 0, bit = 1u << 30;
	while (bit > x) {
		bit >>= 2;
	}
	while (bit) {
		if (x >= r + bit) {
			x -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return r;
}

static int32_t axis_scale(const int32_t *m)
{
	uint32_t best = 0;
	for (int c = 0; c < 3; c++) {
		/* in 8.8 so the squares fit 32 bits up to a scale of 128 */
		const int32_t a = m[c] >> 4, b = m[4 + c] >> 4, d = m[8 + c] >> 4;
		const uint32_t l2 = (uint32_t)(a * a) + (uint32_t)(b * b) + (uint32_t)(d * d);
		const uint32_t l = isqrt32(l2) << 4;
		best = l > best ? l : best;
	}
	return (int32_t)best + 16;
}

static inline int32_t to16(float m)
{
	return (int32_t)(m * 16.0f + (m < 0.0f ? -0.5f : 0.5f));
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
				free(g_isc);
				g_imat = (int32_t (*)[12])malloc(sizeof(int32_t) * 12 * (n ? n : 1));
				g_isc = (int32_t *)malloc(sizeof(int32_t) * (n ? n : 1));
				if (!g_imat || !g_isc) {
					g_inst_n = 0;
				}
				for (unsigned i = 0; i < g_inst_n; i++) {
					for (int k = 0; k < 12; k++) {
						g_imat[i][k] = (int32_t)(rdf(g_inst + i * 52 + 4 + k * 4) * 4096.0f);
					}
					g_isc[i] = axis_scale(g_imat[i]);
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
			if (g_ch[ci].r < 0) {
				continue;
			}
			for (int k = 0; k < 3; k++) {
				s[k] += (float)g_ch[ci].c[k];
			}
			cn++;
		}
		for (int k = 0; k < 3; k++) {
			s[k] = cn ? s[k] / (float)cn : 0.0f;
		}
		float r = 0.0f;
		for (unsigned ci = g_isrc_c0[nd]; ci < g_isrc_c1[nd]; ci++) {
			if (g_ch[ci].r < 0) {
				continue;
			}
			const float dx = (float)g_ch[ci].c[0] - s[0], dy = (float)g_ch[ci].c[1] - s[1], dz = (float)g_ch[ci].c[2] - s[2];
			const float d = sqrtf(dx * dx + dy * dy + dz * dz) + (float)g_ch[ci].r;
			r = d > r ? d : r;
		}
		for (int k = 0; k < 3; k++) {
			g_isph[nd][k] = (int32_t)s[k];
		}
		g_isph[nd][3] = cn ? (int32_t)r + 1 : -1;
	}
}

static int32_t group_sphere(unsigned first, unsigned n, int32_t *c)
{
	float s[3] = { 0, 0, 0 };
	int cnt = 0;
	for (unsigned ci = first; ci < first + n; ci++) {
		if (g_ch[ci].r < 0) {
			continue;
		}
		for (int k = 0; k < 3; k++) {
			s[k] += (float)g_ch[ci].c[k];
		}
		cnt++;
	}
	if (!cnt) {
		c[0] = c[1] = c[2] = 0;
		return -1;
	}
	for (int k = 0; k < 3; k++) {
		c[k] = (int32_t)(s[k] / (float)cnt);
	}
	float r = 0.0f;
	for (unsigned ci = first; ci < first + n; ci++) {
		if (g_ch[ci].r < 0) {
			continue;
		}
		const float dx = (float)(g_ch[ci].c[0] - c[0]), dy = (float)(g_ch[ci].c[1] - c[1]), dz = (float)(g_ch[ci].c[2] - c[2]);
		const float d = sqrtf(dx * dx + dy * dy + dz * dz) + (float)g_ch[ci].r;
		r = d > r ? d : r;
	}
	return (int32_t)r + 1;
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
		int32_t r = group_sphere(ci, n, g->c);
		while (n > 1 && r > 80 * 16) {
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
	g_grp = (Group *)realloc(g_grp, sizeof(Group) * (g_grp_n ? g_grp_n : 1));
}

static void free_mesh(void)
{
	free(g_run);
	free(g_order);
	free(g_ch);
	free(g_vcol);
	g_run = 0;
	g_order = 0;
	g_ch = 0;
	g_vcol = 0;
	g_run_n = 0;
}

static inline int prim_tex(const uint8_t *pr)
{
	return pr[16] < g_tex_n && g_tex[pr[16]].ready ? pr[16] : 255;
}

static int build_runs(void)
{
	free_mesh();
	g_ch = (Chunk *)malloc(sizeof(Chunk) * (g_chunk_n ? g_chunk_n : 1));
	g_order = (uint32_t *)malloc(sizeof(uint32_t) * (g_prim_n ? g_prim_n : 1));
	g_vcol = (uint16_t *)malloc(sizeof(uint16_t) * 2 * (g_vert_n ? g_vert_n : 1));
	/* a run per texture and kind a chunk at most */
	unsigned rmax = 0;
	if (g_ch && g_order && g_vcol) {
		for (unsigned ci = 0; ci < g_chunk_n; ci++) {
			const uint8_t *ch = g_chunks + ci * CHUNK_SZ;
			const unsigned np = rd16(ch + 4) + rd16(ch + 24);
			const unsigned kinds = 2 * (unsigned)(g_tex_n + 1);
			rmax += np < kinds ? np : kinds;
		}
		g_run = (Run *)malloc(sizeof(Run) * (rmax ? rmax : 1));
	}
	if (!g_ch || !g_order || !g_vcol || !g_run) {
		printf("DS: no memory for the mesh's runs (%u primitives)\n", g_prim_n);
		free_mesh();
		return 0;
	}
	for (unsigned v = 0; v < g_vert_n; v++) {
		const uint8_t *p = g_verts + v * VERT_SZ;
		const int r = p[8], g = p[9], b = p[10];
		g_vcol[v * 2] = rgb15(r, g, b);
		g_vcol[v * 2 + 1] = rgb15(r * 2, g * 2, b * 2);
	}
	unsigned on = 0;
	for (unsigned ci = 0; ci < g_chunk_n; ci++) {
		const uint8_t *ch = g_chunks + ci * CHUNK_SZ;
		Chunk *c = &g_ch[ci];
		c->c[0] = to16(rdf(ch + 8));
		c->c[1] = to16(rdf(ch + 12));
		c->c[2] = to16(rdf(ch + 16));
		const float r = rdf(ch + 20);
		c->r = r < 0.0f ? -1 : to16(r) + 1;
		c->node = rd16(ch + 6);
		c->run0 = g_run_n;
		c->nrun = 0;
		c->polys = 0;
		const unsigned first = rd32(ch);
		unsigned np = rd16(ch + 4) + rd16(ch + 24);
		if (first >= g_prim_n) {
			np = 0;
		} else if (first + np > g_prim_n) {
			np = g_prim_n - first;
		}
		/* by texture (the untextured last), quads then triangles, in mesh
		 * order within a run */
		for (int tx = 0; tx <= g_tex_n; tx++) {
			const int want = tx < g_tex_n ? tx : 255;
			for (int quad = 1; quad >= 0; quad--) {
				const unsigned o0 = on;
				for (unsigned k = 0; k < np; k++) {
					const uint8_t *pr = g_prims + (first + k) * PRIM_SZ;
					if (prim_tex(pr) == want && (rd16(pr + 6) != 0xffff) == (quad != 0)) {
						g_order[on++] = first + k;
					}
				}
				if (on == o0) {
					continue;
				}
				for (unsigned s = o0; s < on; s += 65535) {
					Run *rn = &g_run[g_run_n++];
					rn->o0 = s;
					rn->n = (uint16_t)(on - s > 65535 ? 65535 : on - s);
					rn->tex = (uint8_t)want;
					rn->quad = (uint8_t)quad;
					c->nrun++;
				}
				c->polys = (uint16_t)(c->polys + (on - o0 > 65535 ? 65535 : on - o0));
			}
		}
	}
	g_run = (Run *)realloc(g_run, sizeof(Run) * (g_run_n ? g_run_n : 1));
	printf("DS mesh: %u prims, %u chunks, %u runs\n", g_prim_n, g_chunk_n, g_run_n);
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
	if (off + extras > sz || g_chunk_n > 65535) {
		return 0;
	}
	g_mesh = b;
	if (!build_runs()) {
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

/* A frame's geometry goes into a command list in RAM (packed: a word of four
 * command ids, then their parameters), which one DMA sends to the geometry
 * FIFO as the engine takes it. Writing the ports directly would stall the
 * CPU on the FIFO for most of the frame; this way the scripts run while the
 * engine works through the last frame. */
#define CB_WORDS (48 * 1024) /* 192 KiB */
enum {
	CMD_MTX_MODE = 0x10, CMD_MTX_LOAD44 = 0x16, CMD_MTX_LOAD43 = 0x17, CMD_COLOR = 0x20, CMD_TEXCOORD = 0x22,
	CMD_VTX16 = 0x23, CMD_POLYFMT = 0x29, CMD_TEXFMT = 0x2a, CMD_PLTT = 0x2b, CMD_BEGIN = 0x40, CMD_SWAP = 0x50,
	CMD_VIEWPORT = 0x60,
};
static uint32_t *g_cb;    /* the list */
static uint32_t *g_cw;    /* the next word */
static uint32_t *g_chdr;  /* the command word being filled */
static unsigned g_cslot;  /* commands in it (4: full) */
static uint32_t *g_cend;  /* the end a run must stay before */
static int g_dma_on;      /* the last list is still going out */

HOT_INLINE void cb_op(uint32_t cmd)
{
	if (g_cslot == 4) {
		g_chdr = g_cw++;
		*g_chdr = 0;
		g_cslot = 0;
	}
	*g_chdr |= cmd << (8 * g_cslot++);
}

HOT_INLINE void cb_w(uint32_t v)
{
	*g_cw++ = v;
}

/* wait for the last list to have gone into the FIFO */
static void gx_wait(void)
{
	if (g_dma_on) {
		while (dmaBusy(0)) {
		}
		g_dma_on = 0;
	}
}

/* ------------------------------------------------------------ HUD layer */
/* BG3 of the main engine: a 256x192 8-bit bitmap (VRAM bank E) over the 3D
 * (BG0); index 0 is see-through. The CPU draws it in RAM when the HUD
 * changes and copies it over. */
static uint8_t *g_hud_px;  /* 256 x 192 */
static uint16_t g_hud_pal[256];
static int g_hud_pal_n;
static HudQuad g_hud_drawn[16];
static volatile int g_hud_dirty = 1;
static void vblank(void);
static int g_hud_bg;

/* ------------------------------------------------------------ boot */
void gs_draw_boot(int pal)
{
	(void)pal;
	powerOn(POWER_ALL_2D | POWER_3D_CORE | POWER_MATRIX);
	videoSetMode(MODE_5_3D);
	videoSetModeSub(MODE_0_2D);
	/* textures in A-D, their palettes in F and G, the HUD bitmap in E, the
	 * log on the bottom screen in H */
	vramSetBankA(VRAM_A_TEXTURE);
	vramSetBankB(VRAM_B_TEXTURE);
	vramSetBankC(VRAM_C_TEXTURE);
	vramSetBankD(VRAM_D_TEXTURE);
	vramSetBankE(VRAM_E_MAIN_BG);
	vramSetBankF(VRAM_F_TEX_PALETTE_SLOT0);
	vramSetBankG(VRAM_G_TEX_PALETTE_SLOT1);
	vramSetBankH(VRAM_H_SUB_BG);
	consoleInit(NULL, 0, BgType_Text4bpp, BgSize_T_256x256, 8, 0, false, true);
	glInit();
	glEnable(GL_TEXTURE_2D);
	glDisable(GL_ANTIALIAS);
	glDisable(GL_BLEND);
	glClearPolyID(63);
	glClearDepth(GL_MAX_DEPTH);
	glViewport(0, 0, SCR_W - 1, SCR_H - 1);
	g_hud_bg = bgInit(3, BgType_Bmp8, BgSize_B8_256x256, 0, 0);
	bgSetPriority(g_hud_bg, 0);
	bgSetPriority(0, 1);
	g_cb = (uint32_t *)memalign(32, CB_WORDS * 4);
	g_hud_px = (uint8_t *)memalign(32, SCR_W * SCR_H);
	if (g_hud_px) {
		memset(g_hud_px, 0, SCR_W * SCR_H);
	}
	dmaFillWords(0, bgGetGfxPtr(g_hud_bg), SCR_W * SCR_H);
	irqSet(IRQ_VBLANK, vblank);
	irqEnable(IRQ_VBLANK);
}

/* ------------------------------------------------------------ view */
static void eye_now(float *x, float *y, float *z)
{
	*x = g_cam_x + g_attach_x + g_shake_x;
	*y = g_cam_y + g_attach_y + g_shake_y + g_eye_h;
	*z = g_cam_z + g_attach_z + g_shake_z;
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

/* A fixed-point model-view: rotation (and scale) 4.12, translation metres
 * 20.12; sc the largest axis scale (4.12) for the spheres. */
struct FM {
	int32_t m[12];
	int32_t sc;
};

static void fm_from(const float *m, FM *f)
{
	for (int k = 0; k < 12; k++) {
		f->m[k] = f2fx(m[k]);
	}
	f->sc = axis_scale(f->m);
}

/* o = a x b (b a node's or an instance's matrix in the same form); the
 * caller sets o->sc */
static HOT void fm_mul(const FM *a, const int32_t *b, FM *o)
{
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 4; c++) {
			int64_t v = (int64_t)a->m[r * 4] * b[c] + (int64_t)a->m[r * 4 + 1] * b[4 + c] + (int64_t)a->m[r * 4 + 2] * b[8 + c];
			v >>= 12;
			if (c == 3) {
				v += a->m[r * 4 + 3];
			}
			o->m[r * 4 + c] = (int32_t)v;
		}
	}
}

/* a point (1/16 m) through f, into view space (1/16 m) */
static inline __attribute__((always_inline)) ARM_CODE void fm_point(const FM *f, const int32_t *p, int32_t *o)
{
	for (int r = 0; r < 3; r++) {
		o[r] = ((f->m[r * 4] * p[0] + f->m[r * 4 + 1] * p[1] + f->m[r * 4 + 2] * p[2]) >> 12) + (f->m[r * 4 + 3] >> 8);
	}
}

/* ------------------------------------------------------------ drawing */
struct DrawCtx {
	float view[12];  /* world to view */
	FM vfm;          /* the same, fixed */
	int32_t tan_x, tan_y, sec_x, sec_y; /* 4.12 */
	int32_t near16, far16, far_main16;  /* 1/16 m */
	int far_pass;
	int fog;
	int x, y, w, h;  /* the view's part of the screen */
	float tanx_f, tany_f;
	unsigned polys, verts;
	int full;        /* the polygon budget is spent */
	int cur_tex;     /* -2 nothing bound */
	int cur_mat;     /* the matrix slot loaded (-1 none) */
	int back_step;   /* backdrop groups drawn */
	int32_t bucket_k; /* depth (1/16 m) to bucket, 16.16 */
	int32_t proj[16]; /* the backdrop's projection as loaded, but for [10] */
};

/* a sphere (view space, 1/16 m) against the view: 0 out, 1 in */
HOT_INLINE int sphere_visible(const DrawCtx *c, const int32_t *p, int32_t r, int nofar)
{
	if (r < 0) {
		return 0;
	}
	const int32_t d = -p[2];
	if (d + r < c->near16 || (!nofar && d - r > c->far16)) {
		return 0;
	}
	if (d + r < 0) {
		return 0;
	}
	const int32_t dd = d > 0 ? d : 0;
	const int32_t lx = ((dd * c->tan_x) >> 12) + ((r * c->sec_x) >> 12);
	const int32_t ly = ((dd * c->tan_y) >> 12) + ((r * c->sec_y) >> 12);
	if (p[0] > lx || -p[0] > lx || p[1] > ly || -p[1] > ly) {
		return 0;
	}
	return 1;
}

/* the frame's matrices: slot 0 the view (unmoved nodes), then the moved
 * nodes' and the instances' as they come up */
#define MAT_SLOTS 768
static FM g_mslot[MAT_SLOTS];
static int8_t g_mslot_layer[MAT_SLOTS];
static unsigned g_mslot_n;
static int16_t g_node_slot[MAX_NODES]; /* this view's slot of each moved node, -1 none yet */

static HOT int node_slot(DrawCtx *c, unsigned node)
{
	const int moved = node < MAX_NODES && g_nodes_live && g_nm_on[node];
	if (!moved) {
		return 0;
	}
	if (g_node_slot[node] >= 0) {
		return g_node_slot[node];
	}
	if (g_mslot_n >= MAT_SLOTS) {
		return -1;
	}
	const int s = (int)g_mslot_n++;
	fm_mul(&g_mslot[0], g_nmf[node], &g_mslot[s]);
	g_mslot[s].sc = (g_mslot[0].sc * g_nsc[node]) >> 12;
	g_mslot_layer[s] = 0;
	g_node_slot[node] = (int16_t)s;
	return s;
}

/* the hardware's model-view: the slot's matrix times 1/scale (the mesh's
 * int16 read as v16 4.12), a LAYR step pushed back or forward */
static HOT void load_slot(DrawCtx *c, int slot, int layer)
{
	const FM *f = &g_mslot[slot];
	/* rotation x 4096/scale, in 20.12 */
	const int32_t k = (int32_t)((4096 * 4096) / g_scale);
	/* LAYR: the whole model-view scaled about the eye (1 - pri/4096) */
	const int32_t lay = 4096 - layer * LAYER_STEP;
	int32_t m[12];
	for (int r = 0; r < 3; r++) {
		for (int col = 0; col < 3; col++) {
			int32_t v = (int32_t)(((int64_t)f->m[r * 4 + col] * k) >> 12);
			m[r * 4 + col] = layer ? (int32_t)(((int64_t)v * lay) >> 12) : v;
		}
		m[r * 4 + 3] = layer ? (int32_t)(((int64_t)f->m[r * 4 + 3] * lay) >> 12) : f->m[r * 4 + 3];
	}
	/* the DS takes row vectors: its rows are the columns here */
	cb_op(CMD_MTX_LOAD43);
	for (int col = 0; col < 4; col++) {
		cb_w((uint32_t)m[0 * 4 + col]);
		cb_w((uint32_t)m[1 * 4 + col]);
		cb_w((uint32_t)m[2 * 4 + col]);
	}
	c->cur_mat = slot * 64 + (layer + 16);
}

/* one chunk's runs into the command list */
static HOT void draw_chunk(DrawCtx *c, unsigned ci, int fog)
{
	const Chunk *ch = &g_ch[ci];
	const uint32_t polyfmt = POLY_ALPHA(31) | POLY_CULL_NONE | (fog ? POLY_FOG : 0);
	/* a quad a, b, c, d is the triangles a, b, c and b, c, d: a, b, d, c around */
	static const uint8_t q4[4] = { 0, 1, 3, 2 };
	static const uint8_t q3[4] = { 0, 1, 2, 0 };
	for (unsigned ri = ch->run0; ri < ch->run0 + ch->nrun; ri++) {
		const Run *rn = &g_run[ri];
		const unsigned nc = rn->quad ? 4 : 3;
		if (c->polys + rn->n > MAX_POLYS || c->verts + rn->n * nc > MAX_VERTS ||
				(uint32_t)(g_cend - g_cw) < rn->n * nc * 5 + 16) {
			c->full = 1;
			return;
		}
		c->polys += rn->n;
		c->verts += rn->n * nc;
		const int tex = rn->tex;
		if (tex != c->cur_tex) {
			cb_op(CMD_TEXFMT);
			cb_w(tex == 255 ? 0 : g_tex[tex].fmt);
			if (tex != 255 && g_tex[tex].pal >= 0) {
				cb_op(CMD_PLTT);
				cb_w((uint32_t)g_tex[tex].pal);
			}
			c->cur_tex = tex;
		}
		cb_op(CMD_POLYFMT);
		cb_w(polyfmt);
		cb_op(CMD_BEGIN);
		cb_w(rn->quad ? GL_QUADS : GL_TRIANGLES);
		const uint32_t *ord = g_order + rn->o0;
		const uint8_t *qo = rn->quad ? q4 : q3;
		/* each corner a command word of its own (texcoord, colour, vertex;
		 * the fourth a no-op) and its parameters */
		uint32_t *w = g_cw;
		const uint8_t *const prims = g_prims;
		const uint8_t *const verts = g_verts;
		const uint16_t *const vcol = g_vcol;
		const unsigned vn = g_vert_n;
		if (tex == 255) {
			const uint32_t hdr = CMD_COLOR | CMD_VTX16 << 8;
			for (unsigned k = 0; k < rn->n; k++) {
				const uint8_t *pr = prims + ord[k] * PRIM_SZ;
				for (unsigned j = 0; j < nc; j++) {
					const unsigned cj = qo[j];
					unsigned vi = (unsigned)pr[cj * 2] | ((unsigned)pr[cj * 2 + 1] << 8);
					vi = vi < vn ? vi : 0;
					const uint16_t *vp = (const uint16_t *)(verts + vi * VERT_SZ);
					w[0] = hdr;
					w[1] = vcol[vi * 2];
					w[2] = (uint32_t)vp[0] | ((uint32_t)vp[1] << 16);
					w[3] = vp[2];
					w += 4;
				}
			}
		} else {
			const uint32_t hdr = CMD_TEXCOORD | CMD_COLOR << 8 | CMD_VTX16 << 16;
			const int32_t kx = g_tex[tex].kx, ky = g_tex[tex].ky;
			for (unsigned k = 0; k < rn->n; k++) {
				const uint8_t *pr = prims + ord[k] * PRIM_SZ;
				for (unsigned j = 0; j < nc; j++) {
					const unsigned cj = qo[j];
					unsigned vi = (unsigned)pr[cj * 2] | ((unsigned)pr[cj * 2 + 1] << 8);
					vi = vi < vn ? vi : 0;
					const uint16_t *vp = (const uint16_t *)(verts + vi * VERT_SZ);
					const uint32_t s = (uint32_t)((pr[8 + cj * 2] * kx) >> 16) & 0xffff;
					const uint32_t t = (uint32_t)((pr[9 + cj * 2] * ky) >> 16) & 0xffff;
					w[0] = hdr;
					w[1] = s | (t << 16);
					w[2] = vcol[vi * 2 + 1];
					w[3] = (uint32_t)vp[0] | ((uint32_t)vp[1] << 16);
					w[4] = vp[2];
					w += 5;
				}
			}
		}
		g_cw = w;
		g_cslot = 4; /* the next command starts a word of its own */
	}
}

/* what a frame draws, near to far: a chunk with its matrix slot */
struct Item {
	uint16_t ci;
	int16_t slot;
	int8_t layer;
	uint8_t fog;
	uint16_t next;
};
#define MAX_ITEMS 2048
#define BUCKETS 64
static Item g_items[MAX_ITEMS];
static unsigned g_item_n;
static uint16_t g_bucket[BUCKETS];

HOT_INLINE void add_item(DrawCtx *c, unsigned ci, int slot, int layer, int fog, int32_t d)
{
	if (g_item_n >= MAX_ITEMS) {
		return;
	}
	int b = (int)(((d < 0 ? 0 : (d > 0xffff ? 0xffff : d)) * c->bucket_k) >> 16);
	b = b >= BUCKETS ? BUCKETS - 1 : b;
	Item *it = &g_items[g_item_n];
	it->ci = (uint16_t)ci;
	it->slot = (int16_t)slot;
	it->layer = (int8_t)layer;
	it->fog = (uint8_t)fog;
	it->next = g_bucket[b];
	g_bucket[b] = (uint16_t)(g_item_n + 1);
	g_item_n++;
}

/* the projection (row vectors for the DS): z mapped onto [-1, depth_top]
 * of the depth range; depth_top < 0: the backdrop's, every point at -depth_top */
static void load_projection(float tan_x, float tan_y, float n, float f, float depth_top)
{
	float m[16];
	memset(m, 0, sizeof(m));
	m[0] = 1.0f / tan_x;
	m[5] = 1.0f / tan_y;
	if (depth_top < 0.0f) {
		/* z_clip = k * w */
		m[10] = depth_top;
		m[11] = -1.0f;
	} else {
		const float A = (f + n) / (n - f), B = 2.0f * f * n / (n - f);
		const float h = (depth_top + 1.0f) * 0.5f;
		m[10] = h * A - h + 1.0f;
		m[11] = -1.0f;
		m[14] = h * B;
	}
	cb_op(CMD_MTX_MODE);
	cb_w(GL_PROJECTION);
	cb_op(CMD_MTX_LOAD44);
	for (int i = 0; i < 16; i++) {
		cb_w((uint32_t)(int32_t)(m[i] * 4096.0f));
	}
	cb_op(CMD_MTX_MODE);
	cb_w(GL_MODELVIEW);
}

/* the fog table for linear fog from near_d to far_d (m) under the main
 * projection (near n, far f, depth top dt) */
static void set_fog_table(float n, float f, float dt, float near_d, float far_d)
{
	/* a distance's 15-bit depth: ((z/w) + 1) * 0x4000 */
	const float A = (f + n) / (n - f), B = 2.0f * f * n / (n - f);
	const float h = (dt + 1.0f) * 0.5f;
	const float a = h * A - h + 1.0f, bb = h * B;
	/* z/w at distance d (z_e = -d, w = d): (-a d + bb) / d = -a + bb / d */
	float zn = -a + bb / near_d, zf = -a + bb / far_d;
	int d0 = (int)((zn + 1.0f) * 16384.0f), d1 = (int)((zf + 1.0f) * 16384.0f);
	d0 = clampi(d0, 0, 0x7fff);
	d1 = clampi(d1, d0 + 32, 0x7fff);
	int shift = 0;
	while (shift < 10 && 32 * (0x400 >> (shift + 1)) >= d1 - d0) {
		shift++;
	}
	const int step = 0x400 >> shift;
	glFogShift(shift);
	glFogOffset(d0);
#ifdef NDS_STATS
	printf("fog depth %d..%d shift %d\n", d0, d1, shift);
#endif
	for (int i = 0; i < 32; i++) {
		/* the entry's depth back to a distance */
		const float depth = (float)(d0 + (i + 1) * step) / 16384.0f - 1.0f;
		/* z/w = -a + bb/d, so d = bb / (z/w + a) (both negative) */
		const float den = depth + a;
		float d = den < -1e-6f ? bb / den : far_d * 4.0f;
		float t = (d - near_d) / (far_d - near_d);
		t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
		glFogDensity(i, (int)(t * 127.0f));
	}
}


static HOT void cull_groups(DrawCtx *c)
{
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
		const int slot = node_slot(c, node);
		if (slot < 0) {
			continue;
		}
		const FM *f = &g_mslot[slot];
		int32_t p[3];
		fm_point(f, g->c, p);
		const int32_t gr = (g->r * f->sc) >> 12;
		if (!sphere_visible(c, p, gr, nfog)) {
			continue;
		}
		if (nfog) {
			/* an NFOG group reaching past the main far plane is backdrop:
			 * drawn first, at the back; one within it is scene, unfogged */
			const int beyond = -p[2] + gr > c->far_main16;
			if (beyond != c->far_pass) {
				continue;
			}
		}
		const int layer = node < MAX_NODES ? g_nlayer[node] : 0;
		int drew = 0;
		for (unsigned ci = g->first; ci < (unsigned)g->first + g->n; ci++) {
			const Chunk *ch = &g_ch[ci];
			if (ch->r < 0 || !ch->nrun) {
				continue;
			}
			int32_t q[3];
			fm_point(f, ch->c, q);
			if (!sphere_visible(c, q, (ch->r * f->sc) >> 12, nfog)) {
				continue;
			}
			if (c->far_pass) {
				/* the backdrop in mesh order, each group a step nearer
				 * than the one before (the last drawn on top, as with no
				 * depth buffer) */
				if (!drew) {
					const int k = 4095 - (c->back_step < 100 ? c->back_step : 100);
					cb_op(CMD_MTX_MODE);
					cb_w(GL_PROJECTION);
					cb_op(CMD_MTX_LOAD44);
					for (int i = 0; i < 16; i++) {
						cb_w((uint32_t)(i == 10 ? -k : c->proj[i]));
					}
					cb_op(CMD_MTX_MODE);
					cb_w(GL_MODELVIEW);
					c->back_step++;
					drew = 1;
				}
				if (c->cur_mat != slot * 64 + 16) {
					load_slot(c, slot, 0);
				}
				draw_chunk(c, ci, 0);
			} else {
				add_item(c, ci, slot, layer, c->fog && !nfog, -q[2]);
			}
		}
	}
}

static HOT void cull_instances(DrawCtx *c)
{
	for (unsigned i = 0; i < g_inst_n; i++) {
		const unsigned nd = rd16(g_inst + i * 52);
		if (nd >= MAX_NODES || g_isrc[nd] != 2 || g_nhide[nd] || g_isph[nd][3] < 0) {
			continue;
		}
		const int base = node_slot(c, nd);
		if (base < 0 || g_mslot_n >= MAT_SLOTS) {
			continue;
		}
		FM w;
		fm_mul(&g_mslot[base], g_imat[i], &w);
		w.sc = (g_mslot[base].sc * g_isc[i]) >> 12;
		int32_t p[3];
		fm_point(&w, g_isph[nd], p);
		if (!sphere_visible(c, p, (g_isph[nd][3] * w.sc) >> 12, 0)) {
			continue;
		}
		const int slot = (int)g_mslot_n++;
		g_mslot[slot] = w;
		for (unsigned ci = g_isrc_c0[nd]; ci < g_isrc_c1[nd]; ci++) {
			const Chunk *ch = &g_ch[ci];
			if (ch->r < 0 || !ch->nrun) {
				continue;
			}
			int32_t q[3];
			fm_point(&w, ch->c, q);
			if (sphere_visible(c, q, (ch->r * w.sc) >> 12, 0)) {
				add_item(c, ci, slot, 0, c->fog, -q[2]);
			}
		}
	}
}

#ifdef NDS_STATS
uint32_t nds_dbg_t[5], nds_dbg_items;
#endif

/* the scene from the camera as it stands into the screen rect x, y, w, h
 * (split: one of several views) */
static unsigned draw_view(int x, int y, int w, int h, int split)
{
	(void)split;
	static DrawCtx c;
#ifdef NDS_STATS
	const uint32_t t_in = cpuGetTiming();
#endif
	memset(&c, 0, sizeof(c));
	c.x = x;
	c.y = y;
	c.w = w;
	c.h = h;
	const float half = tanf(g_fov * 0.5f * 3.14159265f / 180.0f);
	c.tany_f = half;
	c.tanx_f = half * (float)w / (float)h;
	c.tan_x = (int32_t)(c.tanx_f * 4096.0f);
	c.tan_y = (int32_t)(c.tany_f * 4096.0f);
	c.sec_x = (int32_t)(sqrtf(1.0f + c.tanx_f * c.tanx_f) * 4096.0f) + 1;
	c.sec_y = (int32_t)(sqrtf(1.0f + c.tany_f * c.tany_f) * 4096.0f) + 1;
	const float far_m = g_fog_on ? g_fog_far : 400.0f;
	const float far_main = far_m + 20.0f;
	c.near16 = (int32_t)(NEAR_M * 16.0f);
	c.far16 = (int32_t)(far_m * 16.0f);
	c.far_main16 = (int32_t)(far_main * 16.0f);
	c.cur_tex = -2;
	c.cur_mat = -1;
	view_matrix(c.view);
	g_mslot_n = 1;
	fm_from(c.view, &g_mslot[0]);
	g_mslot_layer[0] = 0;
	for (unsigned k = 0; k < MAX_NODES; k++) {
		g_node_slot[k] = -1;
	}
	/* the DS's viewport counts y up from the bottom */
	cb_op(CMD_VIEWPORT);
	cb_w((uint32_t)x | (uint32_t)(SCR_H - (y + h)) << 8 | (uint32_t)(x + w - 1) << 16 | (uint32_t)(SCR_H - 1 - y) << 24);
	cb_op(CMD_MTX_MODE);
	cb_w(GL_MODELVIEW);
	/* the backdrop (NFOG past the far plane): at the back of the depth
	 * range, each group a little nearer than the one before */
	c.bucket_k = (int32_t)((BUCKETS << 16) / (c.far_main16 > 0 ? c.far_main16 : 1));
	{
		/* the backdrop's projection: z_clip = k * w, k set per group */
		memset(c.proj, 0, sizeof(c.proj));
		c.proj[0] = (int32_t)(4096.0f / c.tanx_f);
		c.proj[5] = (int32_t)(4096.0f / c.tany_f);
		c.proj[11] = -4096;
	}
	c.far_pass = 1;
#ifdef NDS_STATS
	nds_dbg_t[0] += cpuGetTiming() - t_in;
	const uint32_t t_a = cpuGetTiming();
#endif
	cull_groups(&c);
#ifdef NDS_STATS
	nds_dbg_t[1] += cpuGetTiming() - t_a;
	const uint32_t t_b = cpuGetTiming();
#endif
	/* the scene with the depth buffer, and the fog */
	c.far_pass = 0;
	c.cur_mat = -1;
	c.fog = g_fog_on && g_fog_far > g_fog_near;
	load_projection(c.tanx_f, c.tany_f, NEAR_M, far_main, DEPTH_TOP);
	g_item_n = 0;
	for (int b = 0; b < BUCKETS; b++) {
		g_bucket[b] = 0;
	}
	cull_groups(&c);
#ifdef NDS_STATS
	nds_dbg_t[2] += cpuGetTiming() - t_b;
	const uint32_t t_c = cpuGetTiming();
#endif
	cull_instances(&c);
#ifdef NDS_STATS
	nds_dbg_t[3] += cpuGetTiming() - t_c;
	const uint32_t t_d = cpuGetTiming();
#endif
	for (int b = 0; b < BUCKETS && !c.full; b++) {
		for (unsigned i = g_bucket[b]; i && !c.full; i = g_items[i - 1].next) {
			const Item *it = &g_items[i - 1];
			if (c.cur_mat != it->slot * 64 + (it->layer + 16)) {
				load_slot(&c, it->slot, it->layer);
			}
			draw_chunk(&c, it->ci, it->fog);
		}
	}
#ifdef NDS_STATS
	nds_dbg_t[4] += cpuGetTiming() - t_d;
	nds_dbg_items += g_item_n;
#endif
	return c.polys;
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
	return (int)(y * ((float)SCR_H / HUD_H));
}

static uint8_t hud_color(int r, int g, int b)
{
	const uint16_t c = rgb15(r, g, b) | 0x8000;
	for (int i = 1; i < g_hud_pal_n; i++) {
		if (g_hud_pal[i] == c) {
			return (uint8_t)i;
		}
	}
	if (g_hud_pal_n >= 256) {
		return 255;
	}
	g_hud_pal[g_hud_pal_n] = c;
	return (uint8_t)g_hud_pal_n++;
}

static void hud_rect(int x0, int y0, int x1, int y1, uint8_t ci)
{
	x0 = x0 < 0 ? 0 : x0;
	y0 = y0 < 0 ? 0 : y0;
	x1 = x1 > SCR_W ? SCR_W : x1;
	y1 = y1 > SCR_H ? SCR_H : y1;
	for (int y = y0; y < y1; y++) {
		memset(g_hud_px + y * SCR_W + x0, ci, x1 > x0 ? (size_t)(x1 - x0) : 0);
	}
}

/* one glyph, px screen pixels per font pixel; px 0 is the 3x5 font */
static void hud_glyph(int x, int y, int ch, int px, uint8_t ci)
{
	if (ch < 32 || ch > 126) {
		ch = '?';
	}
	const int c = ch - 32;
	if (px == 0) {
		for (int row = 0; row < 5; row++) {
			for (int col = 0; col < 3; col++) {
				if (kFont3x5[c][row] & (1u << (2 - col))) {
					const int gx = x + col, gy = y + row;
					if (gx >= 0 && gx < SCR_W && gy >= 0 && gy < SCR_H) {
						g_hud_px[gy * SCR_W + gx] = ci;
					}
				}
			}
		}
		return;
	}
	for (int row = 0; row < 7; row++) {
		for (int col = 0; col < 5; col++) {
			if (kFont5x7[c][row] & (1u << (4 - col))) {
				hud_rect(x + col * px, y + row * px, x + (col + 1) * px, y + (row + 1) * px, ci);
			}
		}
	}
}

static void hud_text(const HudQuad *h, int r, int g, int b)
{
	const uint8_t ci = hud_color(r, g, b);
	const int sc = h->scale > 0 ? h->scale : 1;
	/* the PS2's glyph is 6 * scale wide at 640: 2.4 * scale here */
	const int px = sc * 2 / 5;
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
	const int min_pitch = px ? 8 * px : 6;
	int last_y = -1000;
	while (*t) {
		int n = 0;
		while (t[n] && t[n] != '\n') {
			n++;
		}
		int ly = hy((float)y_ps2);
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
				hud_glyph(x + k * adv, ly, (unsigned char)t[k], px, ci);
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
	if (!g_hud_px || g_hud_dirty == 2) {
		return; /* the last one has not gone to VRAM yet */
	}
	if (!g_hud_dirty && memcmp(g_hud_drawn, g_hudq, sizeof(g_hudq)) == 0) {
		return;
	}
	memcpy(g_hud_drawn, g_hudq, sizeof(g_hudq));
	g_hud_dirty = 0;
	memset(g_hud_px, 0, SCR_W * SCR_H);
	g_hud_pal_n = 1;
	for (int i = 0; i < 16; i++) {
		const HudQuad *h = &g_hudq[i];
		if (!h->used || h->hidden) {
			continue;
		}
		if (!h->styled || h->kind != 1) {
			hud_rect(hx((float)h->x), hy((float)h->y), hx((float)(h->x + h->w)), hy((float)(h->y + h->h)), hud_color(h->r, h->g, h->b));
		}
		if (h->text[0]) {
			if (h->styled) {
				hud_text(h, h->tr, h->tg, h->tb);
			} else {
				hud_text(h, 255, 255, 255);
			}
		}
	}
	/* in the next vertical blank, with the frame it goes with */
	DC_FlushRange(g_hud_px, SCR_W * SCR_H);
	g_hud_dirty = 2;
}

/* the vertical blank: a new HUD and the fade go out, and the frame count
 * that paces gs_draw_scene goes up */
static volatile int g_bright;
static volatile uint32_t g_vbl;
static uint32_t g_frame_vbl;

static void vblank(void)
{
	if (g_hud_dirty == 2) {
		/* by the CPU: the geometry list may have DMA channel 0 */
		const uint32_t *src = (const uint32_t *)g_hud_px;
		volatile uint32_t *dst = (volatile uint32_t *)bgGetGfxPtr(g_hud_bg);
		for (int i = 0; i < SCR_W * SCR_H / 4; i++) {
			dst[i] = src[i];
		}
		for (int i = 0; i < g_hud_pal_n; i++) {
			BG_PALETTE[i] = g_hud_pal[i];
		}
		g_hud_dirty = 0;
	}
	setBrightness(1, g_bright);
	g_vbl++;
}

/* ------------------------------------------------------------ frame */
/* Split screen (gs_draw_set_views): up to four views of the one scene, each
 * with its own camera, in its own part of the screen (2: top and bottom
 * halves; 3-4: quarters), as the PS2 guest has it. The HUD goes on top once. */
#define MAX_VIEWS 4
struct ViewCam {
	float ex, ey, ez, tx, ty, tz, fov;
};
static ViewCam g_views[MAX_VIEWS];
static int g_view_n;

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

#ifdef NDS_STATS
unsigned nds_dbg_polys;
#endif
static int g_fog_set = -1;
static float g_fog_set_near, g_fog_set_far;

#ifdef NDS_STATS
uint32_t nds_dbg_draw_t;
#endif

void gs_draw_scene(void)
{
	/* a frame a vertical blank at most (the game's clock is the frame's) */
	while (g_vbl == g_frame_vbl) {
		swiWaitForVBlank();
	}
	g_frame_vbl = g_vbl;
	/* the last list has to be in the FIFO before this one is written */
	gx_wait();
#ifdef NDS_STATS
	const uint32_t t_draw0 = cpuGetTiming();
#endif
	g_cw = g_cb;
	g_cslot = 4;
	g_cend = g_cb + CB_WORDS - 64;
	glClearColor((uint8_t)(clampi(g_clear_r, 0, 255) >> 3), (uint8_t)(clampi(g_clear_g, 0, 255) >> 3), (uint8_t)(clampi(g_clear_b, 0, 255) >> 3), 31);
	/* fog: the table follows the main projection's depth */
#ifdef NDS_NOFOG
	g_fog_on = 0;
#endif
	const int fog = g_fog_on && g_fog_far > g_fog_near;
	if (fog) {
		const float far_main = g_fog_far + 20.0f;
		if (g_fog_set != 1 || g_fog_set_near != g_fog_near || g_fog_set_far != g_fog_far) {
			set_fog_table(NEAR_M, far_main, DEPTH_TOP, g_fog_near, g_fog_far);
#ifdef NDS_STATS
			printf("fog %d..%d m\n", (int)g_fog_near, (int)g_fog_far);
#endif
			g_fog_set_near = g_fog_near;
			g_fog_set_far = g_fog_far;
		}
		glFogColor((uint8_t)(clampi(g_fog_r, 0, 255) >> 3), (uint8_t)(clampi(g_fog_g, 0, 255) >> 3), (uint8_t)(clampi(g_fog_b, 0, 255) >> 3), 31);
		glEnable(GL_FOG);
	} else if (g_fog_set != 0) {
		glDisable(GL_FOG);
	}
	g_fog_set = fog;
	unsigned polys = 0;
	if (g_ready && g_view_n < 2) {
		polys = draw_view(0, 0, SCR_W, SCR_H, 0);
	} else if (g_ready) {
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
			polys += draw_view(vx, vy, vw, vh, 1);
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
	}
#ifdef NDS_STATS
	{
		extern unsigned nds_dbg_polys;
		nds_dbg_polys = polys;
	}
#endif
	(void)polys;
	draw_hud();
	/* the list ends with the swap (Z-buffer, translucent sorted), and goes */
	cb_op(CMD_SWAP);
	cb_w(0);
	const uint32_t n = (uint32_t)(g_cw - g_cb);
	DC_FlushRange(g_cb, n * 4);
	dmaSetParams(0, g_cb, (void *)&GFX_FIFO, DMA_FIFO | n);
	g_dma_on = 1;
#ifdef NDS_STATS
	nds_dbg_draw_t += cpuGetTiming() - t_draw0;
#endif
	/* the fade: the main screen's master brightness, towards black or white */
	int level = 0;
	if (g_fade_a > 0) {
		level = g_fade_a >= 128 ? 16 : (g_fade_a * 16 + 64) / 128;
		if (g_fade_r + g_fade_g + g_fade_b < 384) {
			level = -level;
		}
	}
	g_bright = level;
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
	gx_wait(); /* the last frame's list reads the old mesh */
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
		printf("DS: mesh did not parse\n");
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
	(void)roll; /* rolls the up vector, not the forward one */
	const float cx = cosf(pitch), sx = sinf(pitch);
	const float y2 = sx, z2 = -cx;
	const float cy = cosf(yaw), sy = sinf(yaw);
	*fx = z2 * sy;
	*fy = y2;
	*fz = z2 * cy;
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
	/* the rest zeroed, so an unchanged HUD compares equal */
	memset(q->text + i, 0, sizeof(q->text) - i);
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

/* 2D sprites and tile maps are not on the PS1-form runtimes yet */
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
		for (int k = 0; k < 12; k++) {
			g_nmf[node][k] = f2fx(m[k]);
		}
		g_nsc[node] = axis_scale(g_nmf[node]);
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
