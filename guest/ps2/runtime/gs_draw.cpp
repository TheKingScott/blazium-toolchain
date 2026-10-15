// MIT. Parse ABI 1 MESH/GTEX, upload GS textures, CPU-project Y-up verts, GIF tris + HUD.

#include "gs_draw.h"

#include <dma.h>
#include <gif_tags.h>
#include <graph.h>
#include <gs_gp.h>
#include <gs_psm.h>
#include <kernel.h>
#include <malloc.h>
#include <math.h>
#include <packet.h>
#include <string.h>


#ifndef BLAZIUM_PS2_COOK_ABI
#define BLAZIUM_PS2_COOK_ABI 1
#endif

#define GS_MAX_TEX 64
#define GS_BATCH_TRIS 48
#define GS_PACKET_QWORDS 2048
enum {
	COOK_PSM_T4 = 0,
	COOK_PSM_T8 = 1,
	COOK_PSM_CT16 = 2
};

struct CookVert {
	float x, y, z;
	unsigned char r, g, b, a;
	float u, v;
	unsigned short node;
	unsigned short tex;
};

struct UploadedTex {
	int ready;
	int hw_psm;
	int width;
	int height;
	int vram;
	int clut_vram;
	int clut_count;
	void *aligned;
	void *clut_aligned;
};

struct Mat4 {
	float m[16];
};

static int g_ready;
static const unsigned char *g_mesh;
static unsigned g_mesh_sz;
static unsigned g_vert_count;
static unsigned g_index_count;
static const unsigned char *g_verts;
static const unsigned char *g_indices;
#define GS_MAX_LAYERS 16
static const unsigned char *g_ly_mesh[GS_MAX_LAYERS];
static unsigned g_ly_sz[GS_MAX_LAYERS];
static int g_ly_n;
static UploadedTex g_tex[GS_MAX_TEX];
static int g_tex_count;
static int g_have_aabb;
static float g_amin[3];
static float g_amax[3];
static float g_cam_x = 0.0f;
static float g_cam_y = 3.0f;
static float g_cam_z = 8.0f;
static float g_look_x = 0.0f;
static float g_look_y = 1.5f;
static float g_look_z = 0.0f;
static float g_fov = 55.0f;
static int g_ortho;
static float g_eye_h;
static float g_world_yaw = 0.0f;
static float g_pitch = 0.0f;
static float g_yaw = 0.0f;
static float g_roll = 0.0f;
static float g_attach_x = 0.0f;
static float g_attach_y = 0.0f;
static float g_attach_z = 0.0f;
static float g_shake_x = 0.0f;
static float g_shake_y = 0.0f;
static float g_shake_z = 0.0f;

struct HudQuad {
	int used;
	int x, y, w, h;
	int r, g, b;
	char text[33];
};
struct SprtQuad {
	int used;
	int x, y, w, h;
	int tex;
	int flip;
};
struct TileQuad {
	int used;
	int x, y;
	int atlas;
};
static HudQuad g_hudq[16];
static SprtQuad g_sprt[32];
static TileQuad g_tile[256];
static int g_sprt_n;
static int g_tile_n;
static float g_nofs[32][3];
static float g_nrot[32][3];
static int g_fade_a;
static int g_fade_r;
static int g_fade_g;
static int g_fade_b;

static unsigned ru16(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned ru32(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static float rf32(const unsigned char *p)
{
	unsigned u = ru32(p);
	float f;
	memcpy(&f, &u, 4);
	return f;
}

static int cook_psm_to_gs(int cook)
{
	if (cook == COOK_PSM_T4) {
		return GS_PSM_4;
	}
	if (cook == COOK_PSM_T8) {
		return GS_PSM_8;
	}
	return GS_PSM_16;
}

static void frame_aabb(void)
{
	if (!g_have_aabb) {
		return;
	}
	const float cx = 0.5f * (g_amin[0] + g_amax[0]);
	const float cy = 0.5f * (g_amin[1] + g_amax[1]);
	const float cz = 0.5f * (g_amin[2] + g_amax[2]);
	float span = g_amax[0] - g_amin[0];
	if (g_amax[1] - g_amin[1] > span) {
		span = g_amax[1] - g_amin[1];
	}
	if (g_amax[2] - g_amin[2] > span) {
		span = g_amax[2] - g_amin[2];
	}
	if (span < 2.0f) {
		span = 2.0f;
	}
	g_look_x = cx;
	g_look_y = cy;
	g_look_z = cz;
	g_cam_x = cx;
	if (span < 3.5f) {
		/* Tiny preview mesh: 3/4 view. Close enough that a 0.6-wide pillar still
		 * hits the stills 8x8 grid; untextured face tints keep it a box. */
		g_cam_x = cx + span * 0.4f + 2.0f;
		g_cam_y = cy + span * 0.35f + 2.2f;
		g_cam_z = cz + span * 0.9f + 4.5f;
	} else {
		g_cam_y = cy + span * 0.35f + 2.0f;
		g_cam_z = cz + span * 0.9f + 4.0f;
	}
}

static int cam_cannot_see_aabb(float x, float y, float z)
{
	if (!g_have_aabb) {
		return 0;
	}
	const float dx0 = x - 0.0f;
	const float dy0 = y - 3.0f;
	const float dz0 = z - 8.0f;
	if (dx0 * dx0 + dy0 * dy0 + dz0 * dz0 < 0.25f) {
		return 1;
	}
	const float cx = 0.5f * (g_amin[0] + g_amax[0]);
	const float cy = 0.5f * (g_amin[1] + g_amax[1]);
	const float cz = 0.5f * (g_amin[2] + g_amax[2]);
	float span = g_amax[0] - g_amin[0];
	if (g_amax[1] - g_amin[1] > span) {
		span = g_amax[1] - g_amin[1];
	}
	if (g_amax[2] - g_amin[2] > span) {
		span = g_amax[2] - g_amin[2];
	}
	const float dx = x - cx;
	const float dy = y - cy;
	const float dz = z - cz;
	const float dist2 = dx * dx + dy * dy + dz * dz;
	const float need = span * 0.75f + 2.0f;
	if (dist2 < need * need) {
		return 1;
	}
	const float far = span * 6.0f + 24.0f;
	return dist2 > far * far;
}

static void read_vert(const unsigned char *p, CookVert *v)
{
	v->x = rf32(p);
	v->y = rf32(p + 4);
	v->z = rf32(p + 8);
	v->r = p[12];
	v->g = p[13];
	v->b = p[14];
	v->a = p[15];
	v->u = rf32(p + 16);
	v->v = rf32(p + 20);
	v->node = (unsigned short)ru16(p + 24);
	v->tex = (unsigned short)ru16(p + 26);
}

static int parse_mesh(const unsigned char *blob, unsigned sz)
{
	if (!blob || sz < 16) {
		return 0;
	}
	if (blob[0] != 'M' || blob[1] != 'E' || blob[2] != 'S' || blob[3] != 'H') {
		return 0;
	}
	if (ru16(blob + 4) != BLAZIUM_PS2_COOK_ABI) {
		return 0;
	}
	g_vert_count = ru32(blob + 8);
	g_index_count = ru32(blob + 12);
	const unsigned vert_bytes = g_vert_count * 28;
	if (16 + vert_bytes + g_index_count * 4 > sz) {
		return 0;
	}
	g_verts = blob + 16;
	g_indices = blob + 16 + vert_bytes;
	g_mesh = blob;
	g_mesh_sz = sz;
	g_have_aabb = 0;
	if (g_vert_count > 0) {
		g_amin[0] = g_amin[1] = g_amin[2] = 1.0e9f;
		g_amax[0] = g_amax[1] = g_amax[2] = -1.0e9f;
		for (unsigned i = 0; i < g_vert_count; i++) {
			CookVert v;
			read_vert(g_verts + i * 28, &v);
			if (v.x < g_amin[0]) {
				g_amin[0] = v.x;
			}
			if (v.y < g_amin[1]) {
				g_amin[1] = v.y;
			}
			if (v.z < g_amin[2]) {
				g_amin[2] = v.z;
			}
			if (v.x > g_amax[0]) {
				g_amax[0] = v.x;
			}
			if (v.y > g_amax[1]) {
				g_amax[1] = v.y;
			}
			if (v.z > g_amax[2]) {
				g_amax[2] = v.z;
			}
		}
		g_have_aabb = 1;
		frame_aabb();
	}
	return 1;
}

static void parse_camera(const unsigned char *blob, unsigned sz)
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
	if (!blob || sz < 8) {
		return;
	}
	if (blob[0] != 'N' || blob[1] != 'O' || blob[2] != 'D' || blob[3] != 'E') {
		return;
	}
	if (ru16(blob + 4) != BLAZIUM_PS2_COOK_ABI) {
		return;
	}
	const unsigned count = ru16(blob + 6);
	const unsigned rec = (8 + count * 138u <= sz) ? 138u : 74u;
	if (8 + count * rec > sz) {
		return;
	}
	for (unsigned i = 0; i < count; i++) {
		const unsigned char *n = blob + 8 + i * rec;
		if (n[4] == 3) {
			g_cam_x = rf32(n + 6);
			g_cam_y = rf32(n + 10);
			g_cam_z = rf32(n + 14);
			if (rec >= 30) {
				g_pitch = rf32(n + 18);
				g_yaw = rf32(n + 22);
				g_roll = rf32(n + 26);
			}
			{
				float fx = 0.0f, fy = 0.0f, fz = -1.0f;
				const float cx = cosf(g_pitch), sx = sinf(g_pitch);
				const float cy = cosf(g_yaw), sy = sinf(g_yaw);
				fy = sx;
				fz = -cx;
				const float fxx = sy * fz;
				const float fzz = cy * fz;
				g_look_x = g_cam_x + fxx * 8.0f;
				g_look_y = g_cam_y + fy * 8.0f;
				g_look_z = g_cam_z + fzz * 8.0f;
			}
			return;
		}
	}
}

static void *dup_align(const void *src, unsigned n)
{
	void *p = memalign(64, n < 64 ? 64 : n);
	if (!p) {
		return 0;
	}
	memset(p, 0, n < 64 ? 64 : n);
	if (src && n) {
		memcpy(p, src, n);
	}
	return p;
}

/* BITBLT reads a full GS rectangle. Short GTEX payloads must be padded or DMA
 * walks off the allocation (sidescroller 16x16 CT16 with 128-byte data). */
static unsigned gs_xfer_bytes(int psm, int w, int h)
{
	if (w < 1 || h < 1) {
		return 0;
	}
	if (psm == GS_PSM_4) {
		return ((unsigned)w * (unsigned)h) / 2u;
	}
	if (psm == GS_PSM_8) {
		return (unsigned)w * (unsigned)h;
	}
	return (unsigned)w * (unsigned)h * 2u;
}

static void *dup_align_xfer(const void *src, unsigned src_n, int psm, int w, int h)
{
	unsigned need = gs_xfer_bytes(psm, w, h);
	void *p;
	if (src_n > need) {
		need = src_n;
	}
	p = dup_align(0, need);
	if (!p) {
		return 0;
	}
	if (src && src_n) {
		memcpy(p, src, src_n < need ? src_n : need);
	}
	return p;
}

/* Cooked CLUT is linear RGB555. GS CSM1 8-bit palettes are 16x16 with this swizzle. */
static int csm1_index(int i)
{
	return (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);
}

static unsigned short clut555(const unsigned char *clut, unsigned clut_n, unsigned idx)
{
	if (!clut || idx >= clut_n) {
		return 0;
	}
	return (unsigned short)clut[idx * 2] | ((unsigned short)clut[idx * 2 + 1] << 8);
}

/* Expand T8/T4 + linear CLUT to PSMCT16 so the GS never has to load a palette. */
static int expand_ct16(UploadedTex *t, const unsigned char *data, unsigned data_n, const unsigned char *clut, unsigned clut_n)
{
	const unsigned n = (unsigned)t->width * (unsigned)t->height;
	unsigned i;
	unsigned short *dst;
	t->aligned = dup_align(0, n * 2);
	if (!t->aligned) {
		return 0;
	}
	dst = (unsigned short *)t->aligned;
	if (t->hw_psm == GS_PSM_4) {
		for (i = 0; i < n; i++) {
			const unsigned src = i / 2;
			unsigned idx = 0;
			if (src < data_n) {
				idx = (data[src] >> ((i & 1u) * 4u)) & 0xfu;
			}
			dst[i] = clut555(clut, clut_n, idx);
		}
	} else {
		for (i = 0; i < n; i++) {
			const unsigned idx = (i < data_n) ? (unsigned)data[i] : 0;
			dst[i] = clut555(clut, clut_n, idx);
		}
	}
	t->vram = graph_vram_allocate(t->width, t->height, GS_PSM_16, GRAPH_ALIGN_BLOCK);
	if (t->vram < 0) {
		free(t->aligned);
		t->aligned = 0;
		return 0;
	}
	t->hw_psm = GS_PSM_16;
	t->clut_count = 0;
	t->clut_vram = -1;
	t->clut_aligned = 0;
	return 1;
}

static int pack_clut_csm1(UploadedTex *t, const unsigned char *clut, unsigned clut_n)
{
	unsigned i;
	const int n256 = clut_n >= 256;
	const unsigned cells = n256 ? 256u : 16u;
	unsigned short *dst;
	t->clut_aligned = dup_align(0, cells * 2);
	if (!t->clut_aligned) {
		return 0;
	}
	dst = (unsigned short *)t->clut_aligned;
	for (i = 0; i < cells; i++) {
		const int j = n256 ? csm1_index((int)i) : (int)i;
		dst[j] = clut555(clut, clut_n, i);
	}
	if (n256) {
		/* dest_width 64 (GS TBW units); allocate the stride so BITBLT cannot wrap. */
		t->clut_vram = graph_vram_allocate(64, 16, GS_PSM_16, GRAPH_ALIGN_BLOCK);
	} else {
		t->clut_vram = graph_vram_allocate(8, 2, GS_PSM_16, GRAPH_ALIGN_BLOCK);
	}
	return t->clut_vram >= 0;
}

static int upload_bits(UploadedTex *t)
{
	packet_t *packet = packet_init(256, PACKET_NORMAL);
	if (!packet) {
		return 0;
	}
	qword_t *q = packet->data;
	q = draw_texture_transfer(q, t->aligned, t->width, t->height, t->hw_psm, t->vram, t->width);
	if (t->clut_count && t->clut_aligned && t->clut_vram >= 0) {
		if (t->clut_count >= 256) {
			q = draw_texture_transfer(q, t->clut_aligned, 16, 16, GS_PSM_16, t->clut_vram, 64);
		} else {
			q = draw_texture_transfer(q, t->clut_aligned, 8, 2, GS_PSM_16, t->clut_vram, 64);
		}
	}
	q = draw_texture_flush(q);
	FlushCache(0);
	dma_channel_send_chain(DMA_CHANNEL_GIF, packet->data, q - packet->data, 0, 0);
	dma_wait_fast();
	packet_free(packet);
	t->ready = 1;
	return 1;
}

static int upload_one(UploadedTex *t, const unsigned char *data, unsigned data_n, const unsigned char *clut, unsigned clut_n)
{
	t->clut_vram = -1;
	t->clut_aligned = 0;
	if (clut_n && clut && (t->hw_psm == GS_PSM_8 || t->hw_psm == GS_PSM_4)) {
		if (expand_ct16(t, data, data_n, clut, clut_n)) {
			return upload_bits(t);
		}
		t->aligned = dup_align_xfer(data, data_n, t->hw_psm, t->width, t->height);
		if (!t->aligned) {
			return 0;
		}
		t->vram = graph_vram_allocate(t->width, t->height, t->hw_psm, GRAPH_ALIGN_BLOCK);
		if (t->vram < 0) {
			return 0;
		}
		if (!pack_clut_csm1(t, clut, clut_n)) {
			t->clut_count = 0;
		}
		return upload_bits(t);
	}
	t->aligned = dup_align_xfer(data, data_n, t->hw_psm, t->width, t->height);
	if (!t->aligned) {
		return 0;
	}
	t->vram = graph_vram_allocate(t->width, t->height, t->hw_psm, GRAPH_ALIGN_BLOCK);
	if (t->vram < 0) {
		return 0;
	}
	t->clut_count = 0;
	return upload_bits(t);
}

static void parse_upload_gtex(const unsigned char *blob, unsigned sz)
{
	g_tex_count = 0;
	memset(g_tex, 0, sizeof(g_tex));
	if (!blob || sz < 8) {
		return;
	}
	if (blob[0] != 'G' || blob[1] != 'T' || blob[2] != 'E' || blob[3] != 'X') {
		return;
	}
	if (ru16(blob + 4) != BLAZIUM_PS2_COOK_ABI) {
		return;
	}
	const unsigned count = ru16(blob + 6);
	unsigned off = 8;
	for (unsigned i = 0; i < count && g_tex_count < GS_MAX_TEX; i++) {
		if (off + 12 > sz) {
			break;
		}
		UploadedTex *t = &g_tex[g_tex_count];
		t->width = (int)ru16(blob + off);
		t->height = (int)ru16(blob + off + 2);
		t->hw_psm = cook_psm_to_gs((int)blob[off + 4]);
		t->clut_count = (int)ru16(blob + off + 6);
		const unsigned data_n = ru32(blob + off + 8);
		off += 12;
		if (off + data_n + (unsigned)t->clut_count * 2 > sz) {
			break;
		}
		const unsigned char *data = blob + off;
		off += data_n;
		const unsigned char *clut = blob + off;
		off += (unsigned)t->clut_count * 2;
		if (t->width <= 0 || t->height <= 0) {
			continue;
		}
		if (upload_one(t, data, data_n, clut, (unsigned)t->clut_count)) {
			g_tex_count++;
		}
	}
}

static qword_t *bind_tex(qword_t *q, int slot)
{
	if (slot < 0 || slot >= g_tex_count || !g_tex[slot].ready) {
		slot = 0;
	}
	if (slot >= g_tex_count || !g_tex[slot].ready) {
		return q;
	}
	UploadedTex *t = &g_tex[slot];
	texbuffer_t texbuf;
	clutbuffer_t clut;
	lod_t lod;
	memset(&texbuf, 0, sizeof(texbuf));
	memset(&clut, 0, sizeof(clut));
	memset(&lod, 0, sizeof(lod));
	texbuf.address = (unsigned)t->vram;
	texbuf.width = (unsigned)t->width;
	texbuf.psm = (unsigned)t->hw_psm;
	texbuf.info.width = draw_log2((unsigned)t->width);
	texbuf.info.height = draw_log2((unsigned)t->height);
	texbuf.info.components = TEXTURE_COMPONENTS_RGB;
	texbuf.info.function = TEXTURE_FUNCTION_DECAL;
	lod.calculation = LOD_USE_K;
	lod.mag_filter = LOD_MAG_NEAREST;
	lod.min_filter = LOD_MIN_NEAREST;
	if (t->clut_count && t->clut_vram >= 0) {
		clut.storage_mode = CLUT_STORAGE_MODE1;
		clut.start = 0;
		clut.psm = GS_PSM_16;
		clut.load_method = CLUT_LOAD;
		clut.address = (unsigned)t->clut_vram;
	} else {
		clut.load_method = CLUT_NO_LOAD;
		clut.address = 0;
		clut.psm = 0;
	}
	q = draw_texture_sampling(q, 0, &lod);
	q = draw_texturebuffer(q, 0, &texbuf, &clut);
	PACK_GIFTAG(q, GIF_SET_TAG(1, 0, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
	q++;
	PACK_GIFTAG(q, 1, GS_REG_TEXFLUSH);
	q++;
	return q;
}

static void mat_ident(Mat4 *o)
{
	memset(o->m, 0, sizeof(o->m));
	o->m[0] = o->m[5] = o->m[10] = o->m[15] = 1.0f;
}

static void mat_mul(const Mat4 *a, const Mat4 *b, Mat4 *o)
{
	Mat4 t;
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			t.m[c * 4 + r] =
					a->m[0 * 4 + r] * b->m[c * 4 + 0] +
					a->m[1 * 4 + r] * b->m[c * 4 + 1] +
					a->m[2 * 4 + r] * b->m[c * 4 + 2] +
					a->m[3 * 4 + r] * b->m[c * 4 + 3];
		}
	}
	*o = t;
}

static void look_at(Mat4 *o, float ex, float ey, float ez, float tx, float ty, float tz)
{
	float zx = ex - tx, zy = ey - ty, zz = ez - tz;
	float zl = sqrtf(zx * zx + zy * zy + zz * zz);
	if (zl < 1e-5f) {
		zl = 1.0f;
	}
	zx /= zl;
	zy /= zl;
	zz /= zl;
	float xx = 1.0f * zz - 0.0f * zy;
	float xy = 0.0f * zx - 0.0f * zz;
	float xz = 0.0f * zy - 1.0f * zx;
	float xl = sqrtf(xx * xx + xy * xy + xz * xz);
	if (xl < 1e-5f) {
		xx = 1.0f;
		xy = 0.0f;
		xz = 0.0f;
		xl = 1.0f;
	}
	xx /= xl;
	xy /= xl;
	xz /= xl;
	const float yx = zy * xz - zz * xy;
	const float yy = zz * xx - zx * xz;
	const float yz = zx * xy - zy * xx;
	mat_ident(o);
	o->m[0] = xx;
	o->m[4] = xy;
	o->m[8] = xz;
	o->m[12] = -(xx * ex + xy * ey + xz * ez);
	o->m[1] = yx;
	o->m[5] = yy;
	o->m[9] = yz;
	o->m[13] = -(yx * ex + yy * ey + yz * ez);
	o->m[2] = zx;
	o->m[6] = zy;
	o->m[10] = zz;
	o->m[14] = -(zx * ex + zy * ey + zz * ez);
}

static void perspective(Mat4 *o, float fov_deg, float aspect, float zn, float zf)
{
	const float f = 1.0f / tanf(fov_deg * 0.5f * 3.14159265f / 180.0f);
	mat_ident(o);
	o->m[0] = f / aspect;
	o->m[5] = f;
	o->m[10] = (zf + zn) / (zn - zf);
	o->m[11] = -1.0f;
	o->m[14] = (2.0f * zf * zn) / (zn - zf);
	o->m[15] = 0.0f;
}

static void ortho_proj(Mat4 *o, float aspect, float zn, float zf)
{
	const float h = 8.0f;
	const float w = h * aspect;
	mat_ident(o);
	o->m[0] = 1.0f / w;
	o->m[5] = 1.0f / h;
	o->m[10] = 2.0f / (zn - zf);
	o->m[14] = (zf + zn) / (zn - zf);
}

void gs_draw_set_ortho(int on)
{
	g_ortho = on ? 1 : 0;
}

void gs_draw_set_eye_height(float h)
{
	g_eye_h = h;
}

static void xform(const Mat4 *m, float x, float y, float z, float *ox, float *oy, float *oz, float *ow)
{
	*ox = m->m[0] * x + m->m[4] * y + m->m[8] * z + m->m[12];
	*oy = m->m[1] * x + m->m[5] * y + m->m[9] * z + m->m[13];
	*oz = m->m[2] * x + m->m[6] * y + m->m[10] * z + m->m[14];
	*ow = m->m[3] * x + m->m[7] * y + m->m[11] * z + m->m[15];
}

static int project_vert(const Mat4 *mvp, const CookVert *v, vertex_f_t *clip, color_f_t *col, texel_f_t *st)
{
	float x, y, z, w;
	{
		const int ni = (v->node < 32) ? (int)v->node : 0;
		float px = v->x + g_nofs[ni][0];
		const float py = v->y + g_nofs[ni][1];
		float pz = v->z + g_nofs[ni][2];
		const float ry = g_nrot[ni][1];
		if (ry != 0.0f) {
			const float c = cosf(ry);
			const float s = sinf(ry);
			const float rx = px * c - pz * s;
			pz = px * s + pz * c;
			px = rx;
		}
		xform(mvp, px, py, pz, &x, &y, &z, &w);
	}
	if (w <= 0.08f) {
		return 0;
	}
	clip->x = x / w;
	clip->y = y / w;
	/* GS ZTST is only GEQUAL/GREATER. OpenGL NDC near is -1; invert so near writes high Z. */
	{
		float nz = -z / w;
		if (nz > 0.999f) {
			nz = 0.999f;
		}
		if (nz < -1.0f) {
			nz = -1.0f;
		}
		clip->z = nz;
	}
	clip->w = w;
	col->r = (float)v->r / 255.0f;
	col->g = (float)v->g / 255.0f;
	col->b = (float)v->b / 255.0f;
	col->a = (float)v->a / 255.0f;
	st->s = v->u;
	st->t = v->v;
	st->r = 0.0f;
	st->q = 1.0f;
	return 1;
}

/* Tiny AABB + 16x16 GTEX turns a 0.8 box into a full-screen checker smear.
 * Face tints keep a readable cube without ST. */
static void face_tint(const CookVert *a, const CookVert *b, const CookVert *c, color_f_t *cols)
{
	float ux = b->x - a->x, uy = b->y - a->y, uz = b->z - a->z;
	float vx = c->x - a->x, vy = c->y - a->y, vz = c->z - a->z;
	float nx = uy * vz - uz * vy;
	float ny = uz * vx - ux * vz;
	float nz = ux * vy - uy * vx;
	float ax = nx < 0.0f ? -nx : nx;
	float ay = ny < 0.0f ? -ny : ny;
	float az = nz < 0.0f ? -nz : nz;
	float r = 0.85f, g = 0.85f, bcol = 0.85f;
	if (ax >= ay && ax >= az) {
		r = 0.85f;
		g = 0.35f;
		bcol = 0.75f;
	} else if (ay >= az) {
		r = 0.35f;
		g = 0.75f;
		bcol = 0.40f;
	} else {
		r = 0.85f;
		g = 0.75f;
		bcol = 0.25f;
	}
	int k;
	for (k = 0; k < 3; k++) {
		cols[k].r = r;
		cols[k].g = g;
		cols[k].b = bcol;
		cols[k].a = 1.0f;
	}
}

static int clip_on_screen(const vertex_f_t *c)
{
	return c->x > -1.2f && c->x < 1.2f && c->y > -1.2f && c->y < 1.2f;
}

static int aabb_is_tiny(void)
{
	float span;
	if (!g_have_aabb) {
		return 0;
	}
	span = g_amax[0] - g_amin[0];
	if (g_amax[1] - g_amin[1] > span) {
		span = g_amax[1] - g_amin[1];
	}
	if (g_amax[2] - g_amin[2] > span) {
		span = g_amax[2] - g_amin[2];
	}
	return span < 3.5f;
}

/* Reverse-Z from clip.w (view depth). 1/w uses the full 32-bit ZBUF so faces
 * on the same model no longer collapse to one of 65536 packed 16-bit steps. */
static void apply_gs_z(xyz_t *xyz, const vertex_f_t *clip, int n, int bias)
{
	int i;
	for (i = 0; i < n; i++) {
		float w = clip[i].w;
		float t;
		unsigned int z32;
		if (w < 0.25f) {
			w = 0.25f;
		}
		if (w > 400.0f) {
			w = 400.0f;
		}
		t = 0.25f / w;
		if (t > 1.0f) {
			t = 1.0f;
		}
		z32 = (unsigned int)(t * 4294967295.0f);
		if (bias > 0 && z32 > (unsigned)bias) {
			z32 -= (unsigned)bias;
		}
		if (z32 < 1u) {
			z32 = 1u;
		}
		xyz[i].z = z32;
	}
}

/* draw_convert_xyz(xyz, 2048, 2048, ...) maps NDC -1..1 onto GS pixels
 * 0..4096, but only the framebuffer-sized window centred on 2048 is visible.
 * Without this, a 640x448 frame shows the middle 1/6.4 x 1/9.1 of the view.
 * Scale NDC to half the framebuffer so -1..1 spans exactly the screen. */
static void fit_ndc_to_frame(vertex_f_t *c, int n, const framebuffer_t *f)
{
	const float sx = (float)((f && f->width > 0) ? f->width : 640) * 0.5f / 2048.0f;
	const float sy = (float)((f && f->height > 0) ? f->height : 448) * 0.5f / 2048.0f;
	for (int i = 0; i < n; i++) {
		c[i].x *= sx;
		c[i].y *= sy;
	}
}

static void send_packet(packet_t *packet, qword_t *q);

void gs_draw_fb_origin(int width, int height, float *ox, float *oy)
{
	const int w = width > 0 ? width : 640;
	const int h = height > 0 ? height : 448;
	if (ox) {
		*ox = 2048.0f - (float)(w / 2);
	}
	if (oy) {
		*oy = 2048.0f - (float)(h / 2);
	}
}

/* 5x7 rows packed in 7 bytes for ASCII 32..90. Bit 0 is the left column. */
static const unsigned char kFont5x7[59][7] = {
	{0, 0, 0, 0, 0, 0, 0}, /* space */
	{4, 4, 4, 4, 4, 0, 4},
	{10, 10, 0, 0, 0, 0, 0},
	{10, 31, 10, 31, 10, 0, 0},
	{4, 14, 20, 14, 5, 14, 4},
	{17, 18, 4, 8, 19, 0, 0},
	{8, 20, 8, 21, 18, 13, 0},
	{4, 4, 0, 0, 0, 0, 0},
	{4, 8, 8, 8, 8, 8, 4},
	{4, 2, 2, 2, 2, 2, 4},
	{0, 10, 4, 31, 4, 10, 0},
	{0, 4, 4, 31, 4, 4, 0},
	{0, 0, 0, 0, 4, 4, 8},
	{0, 0, 0, 31, 0, 0, 0},
	{0, 0, 0, 0, 0, 4, 4},
	{1, 2, 4, 8, 16, 0, 0},
	{14, 17, 19, 21, 25, 17, 14}, /* 0 */
	{4, 12, 4, 4, 4, 4, 14},
	{14, 17, 1, 6, 8, 16, 31},
	{14, 17, 1, 6, 1, 17, 14},
	{2, 6, 10, 18, 31, 2, 2},
	{31, 16, 30, 1, 1, 17, 14},
	{6, 8, 16, 30, 17, 17, 14},
	{31, 1, 2, 4, 8, 8, 8},
	{14, 17, 17, 14, 17, 17, 14},
	{14, 17, 17, 15, 1, 2, 12},
	{0, 4, 4, 0, 4, 4, 0},
	{0, 4, 4, 0, 4, 4, 8},
	{2, 4, 8, 16, 8, 4, 2},
	{0, 0, 31, 0, 31, 0, 0},
	{8, 4, 2, 1, 2, 4, 8},
	{14, 17, 1, 6, 4, 0, 4},
	{14, 17, 19, 21, 23, 16, 14},
	{14, 17, 17, 31, 17, 17, 17}, /* A */
	{30, 17, 17, 30, 17, 17, 30},
	{14, 17, 16, 16, 16, 17, 14},
	{30, 17, 17, 17, 17, 17, 30},
	{31, 16, 16, 30, 16, 16, 31},
	{31, 16, 16, 30, 16, 16, 16},
	{14, 17, 16, 23, 17, 17, 15},
	{17, 17, 17, 31, 17, 17, 17},
	{14, 4, 4, 4, 4, 4, 14},
	{1, 1, 1, 1, 17, 17, 14},
	{17, 18, 20, 24, 20, 18, 17},
	{16, 16, 16, 16, 16, 16, 31},
	{17, 27, 21, 21, 17, 17, 17},
	{17, 25, 21, 19, 17, 17, 17},
	{14, 17, 17, 17, 17, 17, 14},
	{30, 17, 17, 30, 16, 16, 16},
	{14, 17, 17, 17, 21, 18, 13},
	{30, 17, 17, 30, 20, 18, 17},
	{14, 17, 16, 14, 1, 17, 14},
	{31, 4, 4, 4, 4, 4, 4},
	{17, 17, 17, 17, 17, 17, 14},
	{17, 17, 17, 17, 17, 10, 4},
	{17, 17, 17, 21, 21, 21, 10},
	{17, 17, 10, 4, 10, 17, 17},
	{17, 17, 10, 4, 4, 4, 4},
	{31, 1, 2, 4, 8, 16, 31},
};

static qword_t *emit_filled(qword_t *q, float x0, float y0, float x1, float y1, int r, int g, int b)
{
	rect_t bar;
	memset(&bar, 0, sizeof(bar));
	bar.v0.x = x0;
	bar.v0.y = y0;
	bar.v0.z = 1;
	bar.v1.x = x1;
	bar.v1.y = y1;
	bar.color.r = (unsigned char)(r >> 1);
	bar.color.g = (unsigned char)(g >> 1);
	bar.color.b = (unsigned char)(b >> 1);
	bar.color.a = 0x80;
	bar.color.q = 1.0f;
	return draw_rect_filled(q, 0, &bar);
}

static qword_t *emit_glyph(qword_t *q, float ox, float oy, int ch, int r, int g, int b)
{
	if (ch >= 'a' && ch <= 'z') {
		ch -= 32;
	}
	if (ch < 32 || ch > 90) {
		ch = '?';
	}
	const unsigned char *rows = kFont5x7[ch - 32];
	for (int row = 0; row < 7; row++) {
		unsigned char bits = rows[row];
		int run = 0;
		int start = -1;
		for (int col = 0; col < 6; col++) {
			const int on = (col < 5) && (bits & (1 << (4 - col)));
			if (on) {
				if (start < 0) {
					start = col;
				}
				run++;
			} else if (start >= 0) {
				q = emit_filled(q, ox + (float)start, oy + (float)row, ox + (float)(start + run), oy + (float)row + 1.0f, r, g, b);
				start = -1;
				run = 0;
			}
		}
		if (start >= 0) {
			q = emit_filled(q, ox + (float)start, oy + (float)row, ox + (float)(start + run), oy + (float)row + 1.0f, r, g, b);
		}
	}
	return q;
}

static qword_t *emit_hud(qword_t *q, int width, int height)
{
	/* draw_rect_filled / textured add START_OFFSET ~2048. Pass center-relative
	 * pixels so GS XYZ lands on the 2048-centered framebuffer. */
	const float ox = -0.5f * (float)(width > 0 ? width : 640);
	const float oy = -0.5f * (float)(height > 0 ? height : 448);
	for (int i = 0; i < 16; i++) {
		if (!g_hudq[i].used) {
			continue;
		}
		q = emit_filled(q, ox + (float)g_hudq[i].x, oy + (float)g_hudq[i].y,
				ox + (float)(g_hudq[i].x + g_hudq[i].w), oy + (float)(g_hudq[i].y + g_hudq[i].h),
				g_hudq[i].r, g_hudq[i].g, g_hudq[i].b);
		if (g_hudq[i].text[0]) {
			int n = 0;
			for (int c = 0; g_hudq[i].text[c] && n < 16; c++, n++) {
				q = emit_glyph(q, ox + (float)(g_hudq[i].x + 2 + n * 6), oy + (float)(g_hudq[i].y + 2),
						(int)(unsigned char)g_hudq[i].text[c], 255, 255, 255);
			}
		}
	}
	for (int i = 0; i < g_sprt_n && i < 32; i++) {
		if (!g_sprt[i].used) {
			continue;
		}
		const int tex = g_sprt[i].tex;
		float x0 = ox + (float)g_sprt[i].x;
		float y0 = oy + (float)g_sprt[i].y;
		float x1 = x0 + (float)(g_sprt[i].w > 0 ? g_sprt[i].w : 16);
		float y1 = y0 + (float)(g_sprt[i].h > 0 ? g_sprt[i].h : 16);
		if (g_sprt[i].flip & 1) {
			float t = x0;
			x0 = x1;
			x1 = t;
		}
		if (tex >= 0 && tex < g_tex_count && g_tex[tex].ready) {
			q = bind_tex(q, tex);
			texrect_t spr;
			memset(&spr, 0, sizeof(spr));
			spr.v0.x = x0;
			spr.v0.y = y0;
			spr.v0.z = 1;
			spr.v1.x = x1;
			spr.v1.y = y1;
			spr.t0.u = 0.0f;
			spr.t0.v = 0.0f;
			spr.t1.u = (float)g_tex[tex].width;
			spr.t1.v = (float)g_tex[tex].height;
			spr.color.r = 0x80;
			spr.color.g = 0x80;
			spr.color.b = 0x80;
			spr.color.a = 0x80;
			spr.color.q = 1.0f;
			q = draw_rect_textured(q, 0, &spr);
		} else {
			q = emit_filled(q, x0, y0, x1, y1, 200, 80, 180);
		}
	}
	for (int i = 0; i < g_tile_n && i < 256; i++) {
		if (!g_tile[i].used) {
			continue;
		}
		const float x0 = ox + (float)(g_tile[i].x * 16);
		const float y0 = oy + (float)(g_tile[i].y * 16);
		const int atlas = g_tile[i].atlas;
		if (atlas >= 0 && atlas < g_tex_count && g_tex[atlas].ready) {
			q = bind_tex(q, atlas);
			texrect_t tq;
			memset(&tq, 0, sizeof(tq));
			tq.v0.x = x0;
			tq.v0.y = y0;
			tq.v0.z = 1;
			tq.v1.x = x0 + 16.0f;
			tq.v1.y = y0 + 16.0f;
			tq.t0.u = 0.0f;
			tq.t0.v = 0.0f;
			tq.t1.u = (float)g_tex[atlas].width;
			tq.t1.v = (float)g_tex[atlas].height;
			tq.color.r = 0x80;
			tq.color.g = 0x80;
			tq.color.b = 0x80;
			tq.color.a = 0x80;
			tq.color.q = 1.0f;
			q = draw_rect_textured(q, 0, &tq);
		} else {
			q = emit_filled(q, x0, y0, x0 + 16.0f, y0 + 16.0f, 40, 120, 80);
		}
	}
	if (g_fade_a > 0) {
		draw_enable_blending();
		rect_t fade;
		memset(&fade, 0, sizeof(fade));
		fade.v0.x = ox;
		fade.v0.y = oy;
		fade.v0.z = 1;
		fade.v1.x = ox + (float)width;
		fade.v1.y = oy + (float)height;
		fade.color.r = (unsigned char)(g_fade_r >> 1);
		fade.color.g = (unsigned char)(g_fade_g >> 1);
		fade.color.b = (unsigned char)(g_fade_b >> 1);
		fade.color.a = (unsigned char)g_fade_a;
		fade.color.q = 1.0f;
		q = draw_rect_filled(q, 0, &fade);
		draw_disable_blending();
	}
	return q;
}

int gs_draw_init(const unsigned char *mesh, unsigned mesh_sz,
		const unsigned char *gtex, unsigned gtex_sz,
		const unsigned char *node, unsigned node_sz)
{
	g_ready = 0;
	g_tex_count = 0;
	g_fade_a = g_fade_r = g_fade_g = g_fade_b = 0;
	memset(g_hudq, 0, sizeof(g_hudq));
	memset(g_sprt, 0, sizeof(g_sprt));
	memset(g_tile, 0, sizeof(g_tile));
	g_sprt_n = 0;
	g_tile_n = 0;
	memset(g_nofs, 0, sizeof(g_nofs));
	memset(g_nrot, 0, sizeof(g_nrot));
	parse_camera(node, node_sz);
	parse_upload_gtex(gtex, gtex_sz);
	if (!parse_mesh(mesh, mesh_sz)) {
		return 0;
	}
	g_ready = 1;
	return 1;
}

int gs_draw_layer_add(const unsigned char *mesh, unsigned mesh_sz)
{
	int i;
	if (!mesh || mesh_sz < 16) {
		return 0;
	}
	for (i = 0; i < g_ly_n; i++) {
		if (g_ly_mesh[i] == mesh) {
			return 1;
		}
	}
	if (g_ly_n >= GS_MAX_LAYERS) {
		return 0;
	}
	g_ly_mesh[g_ly_n] = mesh;
	g_ly_sz[g_ly_n] = mesh_sz;
	g_ly_n++;
	return 1;
}

void gs_draw_layer_remove(const unsigned char *mesh)
{
	int i;
	int w = 0;
	for (i = 0; i < g_ly_n; i++) {
		if (g_ly_mesh[i] == mesh) {
			continue;
		}
		g_ly_mesh[w] = g_ly_mesh[i];
		g_ly_sz[w] = g_ly_sz[i];
		w++;
	}
	g_ly_n = w;
}

int gs_draw_ready(void)
{
	return g_ready;
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
	float x = 0.0f;
	float y = 0.0f;
	float z = -1.0f;
	const float cz = cosf(roll);
	const float sz = sinf(roll);
	const float x1 = x * cz - y * sz;
	const float y1 = x * sz + y * cz;
	const float z1 = z;
	const float cx = cosf(pitch);
	const float sx = sinf(pitch);
	const float x2 = x1;
	const float y2 = y1 * cx - z1 * sx;
	const float z2 = y1 * sx + z1 * cx;
	const float cy = cosf(yaw);
	const float sy = sinf(yaw);
	*fx = x2 * cy + z2 * sy;
	*fy = y2;
	*fz = -x2 * sy + z2 * cy;
}

static void apply_look_from_euler(void)
{
	float fx = 0.0f;
	float fy = 0.0f;
	float fz = -1.0f;
	euler_yxz_negz(g_pitch, g_yaw, g_roll, &fx, &fy, &fz);
	const float dist = 8.0f;
	g_look_x = g_cam_x + fx * dist;
	g_look_y = g_cam_y + fy * dist;
	g_look_z = g_cam_z + fz * dist;
}

void gs_draw_set_camera(float x, float y, float z, float pitch, float yaw, float roll, float fov_deg)
{
	if (g_have_aabb) {
		frame_aabb();
		if (fov_deg > 1.0f && fov_deg < 170.0f) {
			g_fov = fov_deg;
		}
		return;
	}
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
	if (dist < 1.0f) {
		dist = 1.0f;
	}
	if (dist > 80.0f) {
		dist = 80.0f;
	}
	float fx = 0.0f;
	float fy = 0.0f;
	float fz = -1.0f;
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

static void eye_now(float *x, float *y, float *z)
{
	*x = g_cam_x + g_attach_x + g_shake_x;
	*y = g_cam_y + g_attach_y + g_shake_y + g_eye_h;
	*z = g_cam_z + g_attach_z + g_shake_z;
}

void gs_draw_hud_quad(int slot, int x, int y, int w, int h, int r, int g, int b)
{
	if (slot < 0 || slot >= 16) {
		return;
	}
	g_hudq[slot].used = 1;
	g_hudq[slot].x = x;
	g_hudq[slot].y = y;
	g_hudq[slot].w = w > 0 ? w : 8;
	g_hudq[slot].h = h > 0 ? h : 8;
	g_hudq[slot].r = r;
	g_hudq[slot].g = g;
	g_hudq[slot].b = b;
}

void gs_draw_clear_hud(void)
{
	memset(g_hudq, 0, sizeof(g_hudq));
}

void gs_draw_hud_text(int slot, int x, int y, int r, int g, int b, const char *str)
{
	if (slot < 0 || slot >= 16) {
		return;
	}
	g_hudq[slot].used = 1;
	g_hudq[slot].x = x;
	g_hudq[slot].y = y;
	if (g_hudq[slot].w < 8) {
		g_hudq[slot].w = 96;
	}
	if (g_hudq[slot].h < 8) {
		g_hudq[slot].h = 16;
	}
	g_hudq[slot].r = r;
	g_hudq[slot].g = g;
	g_hudq[slot].b = b;
	unsigned i = 0;
	if (str) {
		while (str[i] && i < 32) {
			g_hudq[slot].text[i] = str[i];
			i++;
		}
	}
	g_hudq[slot].text[i] = 0;
}

void gs_draw_sprt(int slot, int x, int y, int w, int h, int tex, int flip)
{
	if (slot < 0 || slot >= 32) {
		return;
	}
	g_sprt[slot].used = 1;
	g_sprt[slot].x = x;
	g_sprt[slot].y = y;
	g_sprt[slot].w = w > 0 ? w : 16;
	g_sprt[slot].h = h > 0 ? h : 16;
	g_sprt[slot].tex = tex;
	g_sprt[slot].flip = flip;
	if (g_sprt_n <= slot) {
		g_sprt_n = slot + 1;
	}
}

void gs_draw_clear_sprt(void)
{
	memset(g_sprt, 0, sizeof(g_sprt));
	g_sprt_n = 0;
}

void gs_draw_tile(int slot, int x, int y, int atlas)
{
	if (slot < 0 || slot >= 256) {
		return;
	}
	g_tile[slot].used = 1;
	g_tile[slot].x = x;
	g_tile[slot].y = y;
	g_tile[slot].atlas = atlas;
	if (g_tile_n <= slot) {
		g_tile_n = slot + 1;
	}
}

void gs_draw_clear_tiles(void)
{
	memset(g_tile, 0, sizeof(g_tile));
	g_tile_n = 0;
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

int gs_draw_primary_mesh_node(void)
{
	if (!g_verts || g_vert_count == 0) {
		return 0;
	}
	CookVert v;
	read_vert(g_verts, &v);
	return (v.node < 32) ? (int)v.node : 0;
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
	float px = (x ? *x : 0.0f) + g_nofs[node][0];
	float py = (y ? *y : 0.0f) + g_nofs[node][1];
	float pz = (z ? *z : 0.0f) + g_nofs[node][2];
	const float ry = g_nrot[node][1];
	if (ry != 0.0f) {
		const float c = cosf(ry);
		const float s = sinf(ry);
		const float rx = px * c - pz * s;
		pz = px * s + pz * c;
		px = rx;
	}
	if (x) {
		*x = px;
	}
	if (y) {
		*y = py;
	}
	if (z) {
		*z = pz;
	}
}

void gs_draw_overlay(framebuffer_t *frame, zbuffer_t *z)
{
	if (!frame) {
		return;
	}
	(void)z;
	packet_t *packet = packet_init(256, PACKET_NORMAL);
	if (!packet) {
		return;
	}
	qword_t *q = packet->data;
	q = draw_framebuffer(q, 0, frame);
	q = emit_hud(q, frame->width, frame->height);
	q = draw_finish(q);
	send_packet(packet, q);
	packet_free(packet);
}

void gs_draw_set_fade(int a, int r, int g, int b)
{
	if (a < 0) {
		a = 0;
	}
	if (a > 128) {
		a = 128;
	}
	g_fade_a = a;
	g_fade_r = r < 0 ? 0 : (r > 255 ? 255 : r);
	g_fade_g = g < 0 ? 0 : (g > 255 ? 255 : g);
	g_fade_b = b < 0 ? 0 : (b > 255 ? 255 : b);
}

void gs_draw_camera(float *x, float *y, float *z)
{
	if (x) {
		*x = g_cam_x;
	}
	if (y) {
		*y = g_cam_y;
	}
	if (z) {
		*z = g_cam_z;
	}
}

float gs_draw_world_yaw(void)
{
	return g_world_yaw;
}

int gs_draw_tex_info(int i, int *vram, int *w, int *h, int *psm)
{
	if (i < 0 || i >= g_tex_count || !g_tex[i].ready) {
		return 0;
	}
	if (vram) {
		*vram = g_tex[i].vram;
	}
	if (w) {
		*w = g_tex[i].width;
	}
	if (h) {
		*h = g_tex[i].height;
	}
	if (psm) {
		*psm = g_tex[i].hw_psm;
	}
	return 1;
}

static void yaw_y(Mat4 *o, float yaw)
{
	const float c = cosf(yaw);
	const float s = sinf(yaw);
	mat_ident(o);
	o->m[0] = c;
	o->m[8] = s;
	o->m[2] = -s;
	o->m[10] = c;
}

void gs_draw_fill_mvp(float out[16], int width, int height)
{
	if (!out) {
		return;
	}
	Mat4 world, view, proj, tmp, mvp;
	yaw_y(&world, g_world_yaw);
	float ex = 0.0f, ey = 0.0f, ez = 0.0f;
	eye_now(&ex, &ey, &ez);
	if (aabb_is_tiny() || cam_cannot_see_aabb(ex, ey, ez)) {
		frame_aabb();
		eye_now(&ex, &ey, &ez);
	}
	look_at(&view, ex, ey, ez, g_look_x, g_look_y, g_look_z);
	if (g_ortho && !g_have_aabb) {
		ortho_proj(&proj, (float)width / (float)(height ? height : 1), 0.25f, 400.0f);
	} else {
		perspective(&proj, g_fov, (float)width / (float)(height ? height : 1), 0.25f, 400.0f);
	}
	mat_mul(&view, &world, &tmp);
	mat_mul(&proj, &tmp, &mvp);
	memcpy(out, mvp.m, sizeof(mvp.m));
}

void gs_draw_orbit(float yaw, float dolly)
{
	const float tx = g_look_x;
	const float ty = g_look_y;
	const float tz = g_look_z;
	float dx = g_cam_x - tx;
	float dz = g_cam_z - tz;
	float dist = sqrtf(dx * dx + dz * dz);
	if (dist < 0.25f) {
		dist = 0.25f;
	}
	float az = atan2f(dx, dz);
	az += yaw;
	dist += dolly;
	if (dist < 1.0f) {
		dist = 1.0f;
	}
	if (dist > 80.0f) {
		dist = 80.0f;
	}
	g_cam_x = tx + sinf(az) * dist;
	g_cam_z = tz + cosf(az) * dist;
	(void)ty;
}

static void send_packet(packet_t *packet, qword_t *q)
{
	if (!packet || !q) {
		return;
	}
	FlushCache(0);
	dma_wait_fast();
	dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data, q - packet->data, 0, 0);
	dma_wait_fast();
}

static qword_t *end_prim(qword_t *q, int *in_prim)
{
	if (!*in_prim) {
		return q;
	}
	u64 *dw = (u64 *)q;
	if (((u32)dw) & 8) {
		*dw++ = 0;
	}
	q = draw_prim_end((qword_t *)dw, 3, DRAW_STQ_REGLIST);
	*in_prim = 0;
	return q;
}

void gs_draw_scene(framebuffer_t *frame, zbuffer_t *z)
{
	if (!frame) {
		return;
	}
	packet_t *packet = packet_init(GS_PACKET_QWORDS, PACKET_NORMAL);
	if (!packet) {
		return;
	}
	qword_t *q = packet->data;
	qword_t *limit = packet->data + (GS_PACKET_QWORDS - 80);

	float ox = 0.0f;
	float oy = 0.0f;
	gs_draw_fb_origin(frame->width, frame->height, &ox, &oy);
	q = draw_framebuffer(q, 0, frame);
	q = draw_zbuffer(q, 0, z);
	q = draw_disable_tests(q, 0, z);
	q = draw_clear(q, 0, ox, oy, (float)frame->width, (float)frame->height, 32, 64, 160);
	if (!aabb_is_tiny()) {
		q = draw_enable_tests(q, 0, z);
	}
	if (!g_ready) {
		q = emit_hud(q, frame->width, frame->height);
		q = draw_finish(q);
		send_packet(packet, q);
		packet_free(packet);
		return;
	}

	Mat4 world, view, proj, tmp, mvp;
	yaw_y(&world, g_world_yaw);
	float ex = 0.0f, ey = 0.0f, ez = 0.0f;
	eye_now(&ex, &ey, &ez);
	if (aabb_is_tiny() || cam_cannot_see_aabb(ex, ey, ez)) {
		frame_aabb();
		eye_now(&ex, &ey, &ez);
	}
	look_at(&view, ex, ey, ez, g_look_x, g_look_y, g_look_z);
	if (g_ortho && !g_have_aabb) {
		ortho_proj(&proj, (float)frame->width / (float)(frame->height ? frame->height : 1), 0.25f, 400.0f);
	} else {
		perspective(&proj, g_fov, (float)frame->width / (float)frame->height, 0.25f, 400.0f);
	}
	mat_mul(&view, &world, &tmp);
	mat_mul(&proj, &tmp, &mvp);

	prim_t prim;
	color_t color;
	memset(&prim, 0, sizeof(prim));
	memset(&color, 0, sizeof(color));
	prim.type = PRIM_TRIANGLE;
	prim.shading = PRIM_SHADE_GOURAUD;
	prim.mapping = (g_tex_count > 0 && !aabb_is_tiny()) ? DRAW_ENABLE : DRAW_DISABLE;
	prim.mapping_type = PRIM_MAP_ST;
	prim.colorfix = PRIM_UNFIXED;
	color.r = 0x80;
	color.g = 0x80;
	color.b = 0x80;
	color.a = 0x80;
	color.q = 1.0f;

	vertex_f_t clip[3];
	color_f_t cols[3];
	texel_f_t sts[3];
	xyz_t xyz[3];
	color_t rgba[3];
	texel_t st[3];

	int bound = -1;
	int in_prim = 0;
	unsigned emitted = 0;
	unsigned drawn = 0;
	unsigned visible = 0;

	const unsigned ntri = g_index_count / 3;
	for (unsigned t = 0; t < ntri; t++) {
		const unsigned i0 = ru32(g_indices + t * 12);
		const unsigned i1 = ru32(g_indices + t * 12 + 4);
		const unsigned i2 = ru32(g_indices + t * 12 + 8);
		if (i0 >= g_vert_count || i1 >= g_vert_count || i2 >= g_vert_count) {
			continue;
		}
		CookVert v0, v1, v2;
		read_vert(g_verts + i0 * 28, &v0);
		read_vert(g_verts + i1 * 28, &v1);
		read_vert(g_verts + i2 * 28, &v2);
		if (!project_vert(&mvp, &v0, &clip[0], &cols[0], &sts[0]) ||
				!project_vert(&mvp, &v1, &clip[1], &cols[1], &sts[1]) ||
				!project_vert(&mvp, &v2, &clip[2], &cols[2], &sts[2])) {
			continue;
		}
		if (aabb_is_tiny()) {
			face_tint(&v0, &v1, &v2, cols);
		}
		const int slot = (int)v0.tex;
		if (q >= limit) {
			q = end_prim(q, &in_prim);
			q = draw_finish(q);
			send_packet(packet, q);
			q = packet->data;
			q = draw_framebuffer(q, 0, frame);
			q = draw_zbuffer(q, 0, z);
			if (!aabb_is_tiny()) {
				q = draw_enable_tests(q, 0, z);
			}
			bound = -1;
			emitted = 0;
		}
		if (slot != bound || emitted >= GS_BATCH_TRIS) {
			q = end_prim(q, &in_prim);
			emitted = 0;
			if (slot != bound) {
				if (!aabb_is_tiny()) {
					q = bind_tex(q, slot);
				}
				bound = slot;
			}
			q = draw_prim_start(q, 0, &prim, &color);
			in_prim = 1;
		}
		fit_ndc_to_frame(clip, 3, frame);
		draw_convert_xyz(xyz, 2048.0f, 2048.0f, 32, 3, clip);
		apply_gs_z(xyz, clip, 3, slot);
		draw_convert_rgbq(rgba, 3, clip, cols, 0x80);
		draw_convert_st(st, 3, clip, sts);
		u64 *dw = (u64 *)q;
		for (int k = 0; k < 3; k++) {
			*dw++ = rgba[k].rgbaq;
			*dw++ = st[k].uv;
			*dw++ = xyz[k].xyz;
		}
		q = (qword_t *)dw;
		emitted++;
		drawn++;
		if (clip_on_screen(&clip[0]) || clip_on_screen(&clip[1]) || clip_on_screen(&clip[2])) {
			visible++;
		}
	}
	for (int ly = 0; ly < g_ly_n; ly++) {
		const unsigned char *blob = g_ly_mesh[ly];
		const unsigned bsz = g_ly_sz[ly];
		if (!blob || bsz < 16 || blob[0] != 'M' || blob[1] != 'E' || blob[2] != 'S' || blob[3] != 'H') {
			continue;
		}
		const unsigned lverts = ru32(blob + 8);
		const unsigned lidx = ru32(blob + 12);
		const unsigned vert_bytes = lverts * 28;
		if (16 + vert_bytes + lidx * 4 > bsz || lverts == 0) {
			continue;
		}
		const unsigned char *lverts_p = blob + 16;
		const unsigned char *lidx_p = blob + 16 + vert_bytes;
		const unsigned lntri = lidx / 3;
		for (unsigned t = 0; t < lntri; t++) {
			const unsigned i0 = ru32(lidx_p + t * 12);
			const unsigned i1 = ru32(lidx_p + t * 12 + 4);
			const unsigned i2 = ru32(lidx_p + t * 12 + 8);
			if (i0 >= lverts || i1 >= lverts || i2 >= lverts) {
				continue;
			}
			CookVert v0, v1, v2;
			read_vert(lverts_p + i0 * 28, &v0);
			read_vert(lverts_p + i1 * 28, &v1);
			read_vert(lverts_p + i2 * 28, &v2);
			if (!project_vert(&mvp, &v0, &clip[0], &cols[0], &sts[0]) ||
					!project_vert(&mvp, &v1, &clip[1], &cols[1], &sts[1]) ||
					!project_vert(&mvp, &v2, &clip[2], &cols[2], &sts[2])) {
				continue;
			}
			if (aabb_is_tiny()) {
				face_tint(&v0, &v1, &v2, cols);
			}
			const int slot = (int)v0.tex;
			if (q >= limit) {
				q = end_prim(q, &in_prim);
				q = draw_finish(q);
				send_packet(packet, q);
				q = packet->data;
				q = draw_framebuffer(q, 0, frame);
				q = draw_zbuffer(q, 0, z);
				if (!aabb_is_tiny()) {
					q = draw_enable_tests(q, 0, z);
				}
				bound = -1;
				emitted = 0;
			}
			if (slot != bound || emitted >= GS_BATCH_TRIS) {
				q = end_prim(q, &in_prim);
				emitted = 0;
				if (slot != bound) {
					if (!aabb_is_tiny()) {
						q = bind_tex(q, slot);
					}
					bound = slot;
				}
				q = draw_prim_start(q, 0, &prim, &color);
				in_prim = 1;
			}
			fit_ndc_to_frame(clip, 3, frame);
			draw_convert_xyz(xyz, 2048.0f, 2048.0f, 32, 3, clip);
			apply_gs_z(xyz, clip, 3, slot);
			draw_convert_rgbq(rgba, 3, clip, cols, 0x80);
			draw_convert_st(st, 3, clip, sts);
			u64 *dw = (u64 *)q;
			for (int k = 0; k < 3; k++) {
				*dw++ = rgba[k].rgbaq;
				*dw++ = st[k].uv;
				*dw++ = xyz[k].xyz;
			}
			q = (qword_t *)dw;
			emitted++;
			drawn++;
			if (clip_on_screen(&clip[0]) || clip_on_screen(&clip[1]) || clip_on_screen(&clip[2])) {
				visible++;
			}
		}
	}
	q = end_prim(q, &in_prim);
	q = draw_disable_tests(q, 0, z);
	if (drawn == 0) {
		const float hx = -0.5f * (float)frame->width;
		const float hy = -0.5f * (float)frame->height;
		const float hw = (float)frame->width;
		const float hh = (float)frame->height;
		q = emit_filled(q, hx + 8.0f, hy + 8.0f, hx + hw * 0.5f - 8.0f, hy + hh - 8.0f, 200, 170, 48);
		q = emit_filled(q, hx + hw * 0.5f + 8.0f, hy + 8.0f, hx + hw - 8.0f, hy + hh - 8.0f, 40, 160, 90);
	}
	q = emit_hud(q, frame->width, frame->height);
	q = draw_finish(q);
	send_packet(packet, q);
	packet_free(packet);
}
