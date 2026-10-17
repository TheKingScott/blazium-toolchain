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

/* Chunks: runs of up to GS_CHUNK_TRIS consecutive triangles of one node with
 * a bounding sphere (cooked space). A frame tests each chunk against the
 * view frustum and the far plane before touching its vertices, so track
 * behind the camera and hidden karts cost one test instead of a transform
 * per vertex. Built when a MESH is parsed. */
#define GS_CHUNK_TRIS 48
#define GS_CHUNK_SPAN 40.0f /* metres, any axis */
#define GS_MAX_CHUNKS 4096
struct MeshChunk {
	unsigned first;
	unsigned short n;
	unsigned short node;
	float c[3];
	float r;
};
static MeshChunk g_chunks[GS_MAX_CHUNKS];
static unsigned g_chunk_n;
static unsigned g_stat_tris, g_stat_chunks, g_stat_frames;
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
	/* Style (gs_draw_hud_style): kind 1 label (text only), 2 rect, 3 button;
	 * scale is the glyph size multiplier; align 0 left, 1 centre, 2 right. */
	int kind, scale, align, hidden;
	int tr, tg, tb;
	int styled;
	char text[65];
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
/* Per-node world correction (scripted Node3D transforms): verts are cooked in
 * world space, so a moved node supplies M = W_now * W_cooked^-1 (3x4, row
 * major). Visibility hides a node's triangles (the VM propagates to children). */
#define GS_MAX_NODES 512
static float g_nm[GS_MAX_NODES][12];
static unsigned char g_nm_on[GS_MAX_NODES];
static unsigned char g_nhide[GS_MAX_NODES];
static int g_nodes_live;
/* MESH trailer 'INST': instances of a cooked mesh (MultiMeshInstance3D).
 * A source node's own triangles are not drawn; each instance draws them
 * with its 3x4 delta in place of the node matrix. 'NFOG': nodes kept clear
 * of fog (material Disable Fog: skies, backdrops). */
#define GS_MAX_INST 4096
static const unsigned char *g_inst;
static unsigned g_inst_n;
static unsigned char g_isrc[GS_MAX_NODES];
static unsigned short g_isrc_c0[GS_MAX_NODES];
static unsigned short g_isrc_c1[GS_MAX_NODES];
static unsigned char g_nfog[GS_MAX_NODES];
/* Distance fog (PS2.set_fog): GS FOG with F from the vertex's view depth.
 * Every scene triangle goes out as XYZF2 with a 24-bit Z, fog or not. */
static int g_fog_on;
static int g_fog_r, g_fog_g, g_fog_b;
static float g_fog_near = 40.0f;
static float g_fog_far = 160.0f;
static int g_cam_locked;
static int g_mesh_dirty_vu; /* VU batches rebuilt on the next frame */
static int g_clear_r = 32;
static int g_clear_g = 64;
static int g_clear_b = 160;
static int g_fade_a;
static int g_fade_r;
static int g_fade_g;
static int g_fade_b;
/* Split screen (gs_draw_set_views): up to four views of the one scene, each
 * with its own camera, drawn one after another into its own part of the
 * frame (2: top and bottom halves; 3-4: quarters). The HUD goes on top once. */
#define GS_MAX_VIEWS 4
struct ViewCam {
	float ex, ey, ez, tx, ty, tz, fov;
};
static ViewCam g_views[GS_MAX_VIEWS];
static int g_view_n;
/* the size of the view being drawn: NDC -1..1 spans this many pixels */
static int g_vp_w = 640;
static int g_vp_h = 448;

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

/* The cooked vertex is CookVert byte for byte on the little-endian EE, so a
 * vertex is one 28-byte copy instead of seven assembled fields. */
typedef char cook_vert_is_28_bytes[sizeof(CookVert) == 28 ? 1 : -1];

static void read_vert(const unsigned char *p, CookVert *v)
{
	memcpy(v, p, sizeof(CookVert));
}

static void build_chunks(void)
{
	g_chunk_n = 0;
	const unsigned ntri = g_index_count / 3;
	unsigned t = 0;
	while (t < ntri && g_chunk_n < GS_MAX_CHUNKS) {
		MeshChunk *ch = &g_chunks[g_chunk_n];
		ch->first = t;
		ch->n = 0;
		ch->node = 0xffff;
		float lo[3] = { 1.0e9f, 1.0e9f, 1.0e9f };
		float hi[3] = { -1.0e9f, -1.0e9f, -1.0e9f };
		while (t < ntri && ch->n < GS_CHUNK_TRIS) {
			const unsigned i0 = ru32(g_indices + t * 12);
			if (i0 >= g_vert_count) {
				break;
			}
			const unsigned node = ru16(g_verts + i0 * 28 + 24);
			if (ch->n == 0) {
				ch->node = (unsigned short)node;
			} else if (node != ch->node) {
				break;
			}
			/* a run stops before it spreads past GS_CHUNK_SPAN: a long run
			 * reaches behind the camera and the EE has to clip all of it */
			if (ch->n > 0) {
				int too_big = 0;
				for (int k = 0; k < 3 && !too_big; k++) {
					const unsigned vi = ru32(g_indices + t * 12 + k * 4);
					if (vi >= g_vert_count) {
						continue;
					}
					const unsigned char *vp = g_verts + vi * 28;
					for (int a = 0; a < 3; a++) {
						const float f = rf32(vp + a * 4);
						const float l = f < lo[a] ? f : lo[a];
						const float h = f > hi[a] ? f : hi[a];
						if (h - l > GS_CHUNK_SPAN) {
							too_big = 1;
						}
					}
				}
				if (too_big) {
					break;
				}
			}
			for (int k = 0; k < 3; k++) {
				const unsigned vi = ru32(g_indices + t * 12 + k * 4);
				if (vi >= g_vert_count) {
					continue;
				}
				const unsigned char *vp = g_verts + vi * 28;
				for (int a = 0; a < 3; a++) {
					const float f = rf32(vp + a * 4);
					if (f < lo[a]) {
						lo[a] = f;
					}
					if (f > hi[a]) {
						hi[a] = f;
					}
				}
			}
			ch->n++;
			t++;
		}
		if (ch->n == 0) {
			/* bad index: let the per-triangle path reject it */
			ch->n = 1;
			ch->node = 0xffff;
			ch->r = -1.0f;
			t++;
		} else {
			float r2 = 0.0f;
			for (int a = 0; a < 3; a++) {
				ch->c[a] = 0.5f * (lo[a] + hi[a]);
				const float h = 0.5f * (hi[a] - lo[a]);
				r2 += h * h;
			}
			ch->r = sqrtf(r2);
		}
		g_chunk_n++;
	}
	if (t < ntri) {
		g_chunk_n = 0; /* table full: draw triangle by triangle, uncull'd */
	}
}

static void parse_mesh_extras(const unsigned char *blob, unsigned sz, unsigned off)
{
	g_inst = 0;
	g_inst_n = 0;
	memset(g_isrc, 0, sizeof(g_isrc));
	memset(g_isrc_c0, 0, sizeof(g_isrc_c0));
	memset(g_isrc_c1, 0, sizeof(g_isrc_c1));
	memset(g_nfog, 0, sizeof(g_nfog));
	while (off + 8 <= sz) {
		const unsigned char *h = blob + off;
		const unsigned n = ru32(h + 4);
		off += 8;
		if (h[0] == 'I' && h[1] == 'N' && h[2] == 'S' && h[3] == 'T') {
			if (off + n * 52 > sz) {
				break;
			}
			for (unsigned i = 0; i < n; i++) {
				const unsigned node = ru16(blob + off + i * 52);
				if (node < GS_MAX_NODES) {
					g_isrc[node] = 1;
				}
			}
			/* one table: the cook writes the main list first; a source with
			 * no instances comes as a one-row list of its own after it */
			if (!g_inst) {
				g_inst = blob + off;
				g_inst_n = n < GS_MAX_INST ? n : GS_MAX_INST;
			}
			off += n * 52;
		} else if (h[0] == 'N' && h[1] == 'F' && h[2] == 'O' && h[3] == 'G') {
			if (off + n * 2 > sz) {
				break;
			}
			for (unsigned i = 0; i < n; i++) {
				const unsigned node = ru16(blob + off + i * 2);
				if (node < GS_MAX_NODES) {
					g_nfog[node] = 1;
				}
			}
			off += (n * 2 + 3) & ~3u;
		} else {
			break;
		}
	}
	/* each source's chunks are one contiguous run (its triangles are cooked
	 * together) */
	for (unsigned ci = 0; ci < g_chunk_n; ci++) {
		const unsigned nd = g_chunks[ci].node;
		if (nd >= GS_MAX_NODES || !g_isrc[nd]) {
			continue;
		}
		if (g_isrc[nd] == 1) {
			g_isrc_c0[nd] = (unsigned short)ci;
			g_isrc[nd] = 2;
		}
		g_isrc_c1[nd] = (unsigned short)(ci + 1);
	}
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
	build_chunks();
	parse_mesh_extras(blob, sz, 16 + vert_bytes + g_index_count * 4);
	g_mesh_dirty_vu = 1;
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
/* GS memory for a texture. graph_vram_allocate sizes it as if the texture
 * were stored row by row, but the GS stores it in 8 KiB pages of swizzled
 * blocks (PSMCT32 64x32, PSMCT16 64x64, PSMT8 128x64, PSMT4 128x128 a page).
 * A texture that is not whole pages touches more of them than that size
 * covers: 256x32 in PSMT8 spans two pages but got one, so the next texture
 * overwrote its right half. Round up to whole pages unless it fits in one
 * block. */
static int tex_vram_allocate(int w, int h, int psm)
{
	int pw = 0, ph = 0, bw = 0, bh = 0;
	switch (psm) {
		case GS_PSM_32:
		case GS_PSM_24:
			pw = 64; ph = 32; bw = 8; bh = 8;
			break;
		case GS_PSM_16:
		case GS_PSM_16S:
			pw = 64; ph = 64; bw = 16; bh = 8;
			break;
		case GS_PSM_8:
			pw = 128; ph = 64; bw = 16; bh = 16;
			break;
		case GS_PSM_4:
			pw = 128; ph = 128; bw = 32; bh = 16;
			break;
		default:
			return graph_vram_allocate(w, h, psm, GRAPH_ALIGN_BLOCK);
	}
	if (w > bw || h > bh) {
		w = (w + pw - 1) / pw * pw;
		h = (h + ph - 1) / ph * ph;
	}
	return graph_vram_allocate(w, h, psm, GRAPH_ALIGN_BLOCK);
}

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
	t->vram = tex_vram_allocate(t->width, t->height, GS_PSM_16);
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
		t->vram = tex_vram_allocate(t->width, t->height, t->hw_psm);
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
	t->vram = tex_vram_allocate(t->width, t->height, t->hw_psm);
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

static qword_t *bind_tex_ctx(qword_t *q, int slot, int ctx);

static qword_t *bind_tex(qword_t *q, int slot)
{
	return bind_tex_ctx(q, slot, 0);
}

/* A texture for drawing context ctx (0: the EE's triangles, 1: VU1's, so
 * the two can be in flight at once without trading TEX0/TEX1/CLAMP). */
static qword_t *bind_tex_ctx(qword_t *q, int slot, int ctx)
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
	texbuf.info.function = TEXTURE_FUNCTION_MODULATE;
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
	/* Godot UVs tile (flag UV 0..8): REPEAT, not the environment's CLAMP. */
	texwrap_t wrap;
	wrap.horizontal = WRAP_REPEAT;
	wrap.vertical = WRAP_REPEAT;
	wrap.minu = 0;
	wrap.maxu = (int)t->width - 1;
	wrap.minv = 0;
	wrap.maxv = (int)t->height - 1;
	q = draw_texture_sampling(q, ctx, &lod);
	q = draw_texture_wrapping(q, ctx, &wrap);
	q = draw_texturebuffer(q, ctx, &texbuf, &clut);
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
		/* 24-bit: scene triangles go out as XYZF2 (Z 24 bits, then fog) */
		z32 = (unsigned int)(t * 16777215.0f);
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
	(void)f; /* the view's size: the whole frame, or one split-screen view */
	const float sx = (float)(g_vp_w > 0 ? g_vp_w : 640) * 0.5f / 2048.0f;
	const float sy = (float)(g_vp_h > 0 ? g_vp_h : 448) * 0.5f / 2048.0f;
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

/* 5x7 rows packed in 7 bytes for ASCII 32..126. Bit 4 is the left column. */
static const unsigned char kFont5x7[95][7] = {
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
	{14, 8, 8, 8, 8, 8, 14}, /* [ */
	{16, 8, 4, 2, 1, 0, 0},
	{14, 2, 2, 2, 2, 2, 14},
	{4, 10, 17, 0, 0, 0, 0},
	{0, 0, 0, 0, 0, 0, 31},
	{8, 4, 0, 0, 0, 0, 0},
	{0, 0, 14, 1, 15, 17, 15}, /* a */
	{16, 16, 22, 25, 17, 17, 30},
	{0, 0, 14, 16, 16, 17, 14},
	{1, 1, 13, 19, 17, 17, 15},
	{0, 0, 14, 17, 31, 16, 14},
	{6, 9, 8, 28, 8, 8, 8},
	{0, 15, 17, 17, 15, 1, 14},
	{16, 16, 22, 25, 17, 17, 17},
	{4, 0, 12, 4, 4, 4, 14},
	{2, 0, 6, 2, 2, 18, 12},
	{16, 16, 18, 20, 24, 20, 18},
	{12, 4, 4, 4, 4, 4, 14},
	{0, 0, 26, 21, 21, 17, 17},
	{0, 0, 22, 25, 17, 17, 17},
	{0, 0, 14, 17, 17, 17, 14},
	{0, 0, 30, 17, 30, 16, 16},
	{0, 0, 13, 19, 15, 1, 1},
	{0, 0, 22, 25, 16, 16, 16},
	{0, 0, 14, 16, 14, 1, 30},
	{8, 8, 28, 8, 8, 9, 6},
	{0, 0, 17, 17, 17, 19, 13},
	{0, 0, 17, 17, 17, 10, 4},
	{0, 0, 17, 17, 21, 21, 10},
	{0, 0, 17, 10, 4, 10, 17},
	{0, 0, 17, 17, 15, 1, 14},
	{0, 0, 31, 2, 4, 8, 31},
	{2, 4, 4, 8, 4, 4, 2}, /* { */
	{4, 4, 4, 4, 4, 4, 4},
	{8, 4, 4, 2, 4, 4, 8},
	{0, 0, 8, 21, 2, 0, 0},
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

static packet_t *g_hud_packet;
static qword_t *g_hud_limit;

/* HUD text can outgrow one packet: send what is there and keep going (the
 * GS keeps the drawing environment between packets). */
static qword_t *hud_room(qword_t *q)
{
	if (g_hud_packet && g_hud_limit && q >= g_hud_limit) {
		send_packet(g_hud_packet, q);
		q = g_hud_packet->data;
	}
	return q;
}

static qword_t *emit_glyph(qword_t *q, float ox, float oy, int ch, int r, int g, int b, int scale)
{
	if (ch < 32 || ch > 126) {
		ch = '?';
	}
	const float sc = (float)(scale > 0 ? scale : 1);
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
				q = hud_room(q);
				q = emit_filled(q, ox + (float)start * sc, oy + (float)row * sc, ox + (float)(start + run) * sc, oy + (float)(row + 1) * sc, r, g, b);
				start = -1;
				run = 0;
			}
		}
		if (start >= 0) {
			q = hud_room(q);
			q = emit_filled(q, ox + (float)start * sc, oy + (float)row * sc, ox + (float)(start + run) * sc, oy + (float)(row + 1) * sc, r, g, b);
		}
	}
	return q;
}

/* One HUD slot's text: '\n' starts a new line, each line aligned in the box. */
static qword_t *emit_text_block(qword_t *q, float ox, float oy, const HudQuad *h, int r, int g, int b)
{
	const int sc = h->scale > 0 ? h->scale : 1;
	const int adv = 6 * sc;
	const int line_h = 9 * sc;
	const char *t = h->text;
	int lines = 1;
	for (const char *p = t; *p; p++) {
		if (*p == '\n') {
			lines++;
		}
	}
	int y = h->y + 2;
	if (h->styled && h->kind != 1) {
		/* Buttons and panels centre their text vertically. */
		const int total = lines * line_h - 2 * sc;
		y = h->y + (h->h - total) / 2;
	}
	while (*t) {
		int n = 0;
		while (t[n] && t[n] != '\n') {
			n++;
		}
		int x = h->x + 2;
		const int width = n * adv - sc;
		if (h->align == 1) {
			x = h->x + (h->w - width) / 2;
		} else if (h->align == 2) {
			x = h->x + h->w - width - 2;
		}
		for (int c = 0; c < n; c++) {
			if (t[c] != ' ') {
				q = emit_glyph(q, ox + (float)(x + c * adv), oy + (float)y, (int)(unsigned char)t[c], r, g, b, sc);
			}
		}
		t += n;
		if (*t == '\n') {
			t++;
		}
		y += line_h;
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
		const HudQuad *h = &g_hudq[i];
		if (!h->used || h->hidden) {
			continue;
		}
		q = hud_room(q);
		if (!h->styled) {
			/* Unstyled (legacy) rows: filled box, white text. */
			q = emit_filled(q, ox + (float)h->x, oy + (float)h->y, ox + (float)(h->x + h->w), oy + (float)(h->y + h->h), h->r, h->g, h->b);
			if (h->text[0]) {
				q = emit_text_block(q, ox, oy, h, 255, 255, 255);
			}
			continue;
		}
		if (h->kind != 1) {
			q = emit_filled(q, ox + (float)h->x, oy + (float)h->y, ox + (float)(h->x + h->w), oy + (float)(h->y + h->h), h->r, h->g, h->b);
		}
		if (h->text[0]) {
			q = emit_text_block(q, ox, oy, h, h->tr, h->tg, h->tb);
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
	if (g_have_aabb && !g_cam_locked) {
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
		while (str[i] && i < 64) {
			g_hudq[slot].text[i] = str[i];
			i++;
		}
	}
	g_hudq[slot].text[i] = 0;
}

void gs_draw_hud_style(int slot, int kind, int scale, int align, int hidden, int tr, int tg, int tb)
{
	if (slot < 0 || slot >= 16) {
		return;
	}
	g_hudq[slot].styled = 1;
	g_hudq[slot].kind = kind;
	g_hudq[slot].scale = scale > 0 ? scale : 1;
	g_hudq[slot].align = align;
	g_hudq[slot].hidden = hidden ? 1 : 0;
	g_hudq[slot].tr = tr;
	g_hudq[slot].tg = tg;
	g_hudq[slot].tb = tb;
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
	packet_t *packet = packet_init(GS_PACKET_QWORDS, PACKET_NORMAL);
	if (!packet) {
		return;
	}
	qword_t *q = packet->data;
	q = draw_framebuffer(q, 0, frame);
	g_hud_packet = packet;
	g_hud_limit = packet->data + (GS_PACKET_QWORDS - 80);
	q = emit_hud(q, frame->width, frame->height);
	g_hud_packet = 0;
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
	if (!g_cam_locked && (aabb_is_tiny() || cam_cannot_see_aabb(ex, ey, ez))) {
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
	if (g_cam_locked) {
		return;
	}
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

/* RGBAQ (Q = 1/w), ST, then XYZF2: 24-bit Z and the fog factor. */
#define GS_SCENE_REGLIST (((u64)GIF_REG_RGBAQ) << 0 | ((u64)GIF_REG_ST) << 4 | ((u64)GIF_REG_XYZF2) << 8)

static qword_t *end_prim(qword_t *q, int *in_prim)
{
	if (!*in_prim) {
		return q;
	}
	u64 *dw = (u64 *)q;
	if (((u32)dw) & 8) {
		*dw++ = 0;
	}
	q = draw_prim_end((qword_t *)dw, 3, GS_SCENE_REGLIST);
	*in_prim = 0;
	return q;
}

/* ---- Scene triangles: node transforms, near/guard-band clipping. ---- */

struct DrawCtx {
	packet_t *packet;
	qword_t *q;
	qword_t *limit;
	framebuffer_t *frame;
	zbuffer_t *z;
	const Mat4 *mvp;
	prim_t prim;
	color_t color;
	int bound;
	int in_prim;
	unsigned emitted;
	unsigned drawn;
	int tiny;
	int nofog; /* this node keeps clear of fog (F = 255) */
};

/* Clip-space vertex with its attributes, for Sutherland-Hodgman. */
struct PV {
	float x, y, z, w;
	float r, g, b, a;
	float s, t;
};

static void node_world(const CookVert *v, float *x, float *y, float *z)
{
	float px = v->x;
	float py = v->y;
	float pz = v->z;
	const int n = (int)v->node;
	if (g_nodes_live && n < GS_MAX_NODES && g_nm_on[n]) {
		const float *m = g_nm[n];
		const float qx = m[0] * px + m[1] * py + m[2] * pz + m[3];
		const float qy = m[4] * px + m[5] * py + m[6] * pz + m[7];
		const float qz = m[8] * px + m[9] * py + m[10] * pz + m[11];
		px = qx;
		py = qy;
		pz = qz;
	}
	if (n < 32) {
		px += g_nofs[n][0];
		py += g_nofs[n][1];
		pz += g_nofs[n][2];
		const float ry = g_nrot[n][1];
		if (ry != 0.0f) {
			const float c = cosf(ry);
			const float s = sinf(ry);
			const float rx = px * c - pz * s;
			pz = px * s + pz * c;
			px = rx;
		}
	}
	*x = px;
	*y = py;
	*z = pz;
}

static void to_pv(const Mat4 *mvp, const CookVert *v, PV *o)
{
	float px, py, pz;
	node_world(v, &px, &py, &pz);
	xform(mvp, px, py, pz, &o->x, &o->y, &o->z, &o->w);
	o->r = (float)v->r / 255.0f;
	o->g = (float)v->g / 255.0f;
	o->b = (float)v->b / 255.0f;
	o->a = (float)v->a / 255.0f;
	o->s = v->u;
	o->t = v->v;
}

/* Plane distance: near (w >= kNearW) plus a guard band that keeps GS XY in
 * 12.4 range after fit_ndc_to_frame (|x| <= 6w, |y| <= 8w). */
#define CLIP_NEAR_W 0.25f
static float plane_d(const PV *v, int p)
{
	switch (p) {
		case 0:
			return v->w - CLIP_NEAR_W;
		case 1:
			return 6.0f * v->w - v->x;
		case 2:
			return 6.0f * v->w + v->x;
		case 3:
			return 8.0f * v->w - v->y;
		default:
			return 8.0f * v->w + v->y;
	}
}

static void pv_lerp(const PV *a, const PV *b, float t, PV *o)
{
	o->x = a->x + (b->x - a->x) * t;
	o->y = a->y + (b->y - a->y) * t;
	o->z = a->z + (b->z - a->z) * t;
	o->w = a->w + (b->w - a->w) * t;
	o->r = a->r + (b->r - a->r) * t;
	o->g = a->g + (b->g - a->g) * t;
	o->b = a->b + (b->b - a->b) * t;
	o->a = a->a + (b->a - a->a) * t;
	o->s = a->s + (b->s - a->s) * t;
	o->t = a->t + (b->t - a->t) * t;
}

static int clip_poly(PV *poly, int n)
{
	PV tmp[9];
	for (int p = 0; p < 5 && n >= 3; p++) {
		int all_in = 1;
		for (int i = 0; i < n; i++) {
			if (plane_d(&poly[i], p) < 0.0f) {
				all_in = 0;
				break;
			}
		}
		if (all_in) {
			continue;
		}
		int m = 0;
		for (int i = 0; i < n && m < 8; i++) {
			const PV *a = &poly[i];
			const PV *b = &poly[(i + 1) % n];
			const float da = plane_d(a, p);
			const float db = plane_d(b, p);
			if (da >= 0.0f) {
				tmp[m++] = *a;
			}
			if ((da >= 0.0f) != (db >= 0.0f) && m < 8) {
				pv_lerp(a, b, da / (da - db), &tmp[m++]);
			}
		}
		for (int i = 0; i < m; i++) {
			poly[i] = tmp[i];
		}
		n = m;
	}
	return n;
}

static void ctx_flush(DrawCtx *c)
{
	c->q = end_prim(c->q, &c->in_prim);
	c->q = draw_finish(c->q);
	send_packet(c->packet, c->q);
	c->q = c->packet->data;
	c->q = draw_framebuffer(c->q, 0, c->frame);
	c->q = draw_zbuffer(c->q, 0, c->z);
	if (!c->tiny) {
		c->q = draw_enable_tests(c->q, 0, c->z);
	}
	c->bound = -1;
	c->emitted = 0;
}

/* GS fog factor: 255 = no fog, 0 = all fog colour; linear in view depth. */
static unsigned fog_byte(float w)
{
	const float span = g_fog_far - g_fog_near;
	float t = span > 0.001f ? (g_fog_far - w) / span : 1.0f;
	if (t < 0.0f) {
		t = 0.0f;
	}
	if (t > 1.0f) {
		t = 1.0f;
	}
	return (unsigned)(t * 255.0f);
}

static void emit_clipped(DrawCtx *c, const PV *a, const PV *b, const PV *d, int slot)
{
	vertex_f_t clip[3];
	color_f_t cols[3];
	texel_f_t sts[3];
	xyz_t xyz[3];
	color_t rgba[3];
	texel_t st[3];
	const PV *v[3] = { a, b, d };
	for (int k = 0; k < 3; k++) {
		const float w = v[k]->w;
		const float z = v[k]->z;
		clip[k].x = v[k]->x / w;
		clip[k].y = v[k]->y / w;
		/* GS ZTST is only GEQUAL/GREATER: invert NDC z so near writes high Z. */
		float nz = -z / w;
		if (nz > 0.999f) {
			nz = 0.999f;
		}
		if (nz < -1.0f) {
			nz = -1.0f;
		}
		clip[k].z = nz;
		clip[k].w = w;
		cols[k].r = v[k]->r;
		cols[k].g = v[k]->g;
		cols[k].b = v[k]->b;
		cols[k].a = v[k]->a;
		sts[k].s = v[k]->s;
		sts[k].t = v[k]->t;
		sts[k].r = 0.0f;
		sts[k].q = 1.0f;
	}
	if (c->q >= c->limit) {
		ctx_flush(c);
	}
	if (slot != c->bound || c->emitted >= GS_BATCH_TRIS) {
		c->q = end_prim(c->q, &c->in_prim);
		c->emitted = 0;
		if (slot != c->bound) {
			if (!c->tiny) {
				c->q = bind_tex(c->q, slot);
			}
			c->bound = slot;
		}
		c->q = draw_prim_start(c->q, 0, &c->prim, &c->color);
		c->in_prim = 1;
	}
	fit_ndc_to_frame(clip, 3, c->frame);
	draw_convert_xyz(xyz, 2048.0f, 2048.0f, 32, 3, clip);
	apply_gs_z(xyz, clip, 3, slot);
	draw_convert_rgbq(rgba, 3, clip, cols, 0x80);
	draw_convert_st(st, 3, clip, sts);
	u64 *dw = (u64 *)c->q;
	for (int k = 0; k < 3; k++) {
		u64 f = 255;
		if (g_fog_on && !c->nofog) {
			f = fog_byte(clip[k].w);
		}
		*dw++ = rgba[k].rgbaq;
		*dw++ = st[k].uv;
		*dw++ = (u64)xyz[k].x | ((u64)xyz[k].y << 16) | ((u64)(xyz[k].z & 0xffffff) << 32) | (f << 56);
	}
	c->q = (qword_t *)dw;
	c->emitted++;
	c->drawn++;
}

static void draw_tri(DrawCtx *c, const CookVert *v0, const CookVert *v1, const CookVert *v2)
{
	if (g_nodes_live) {
		const int n = (int)v0->node;
		if (n < GS_MAX_NODES && g_nhide[n]) {
			return;
		}
	}
	c->nofog = v0->node < GS_MAX_NODES ? g_nfog[v0->node] : 0;
	PV poly[9];
	to_pv(c->mvp, v0, &poly[0]);
	to_pv(c->mvp, v1, &poly[1]);
	to_pv(c->mvp, v2, &poly[2]);
	/* Whole triangle outside one frustum side: drop before any work. */
	if ((poly[0].x > poly[0].w && poly[1].x > poly[1].w && poly[2].x > poly[2].w) ||
			(poly[0].x < -poly[0].w && poly[1].x < -poly[1].w && poly[2].x < -poly[2].w) ||
			(poly[0].y > poly[0].w && poly[1].y > poly[1].w && poly[2].y > poly[2].w) ||
			(poly[0].y < -poly[0].w && poly[1].y < -poly[1].w && poly[2].y < -poly[2].w) ||
			(poly[0].w < CLIP_NEAR_W && poly[1].w < CLIP_NEAR_W && poly[2].w < CLIP_NEAR_W)) {
		return;
	}
	if (c->tiny) {
		color_f_t tint[3];
		face_tint(v0, v1, v2, tint);
		for (int k = 0; k < 3; k++) {
			poly[k].r = tint[k].r;
			poly[k].g = tint[k].g;
			poly[k].b = tint[k].b;
		}
	}
	const int n = clip_poly(poly, 3);
	for (int i = 1; i + 1 < n; i++) {
		emit_clipped(c, &poly[0], &poly[i], &poly[i + 1], (int)v0->tex);
	}
}

static void draw_range(DrawCtx *c, unsigned t0, unsigned t1)
{
	for (unsigned t = t0; t < t1; t++) {
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
		draw_tri(c, &v0, &v1, &v2);
		g_stat_tris++;
	}
}

/* ---- VU1 scene path ----------------------------------------------------
 * Chunks that are wholly in front of the near plane and inside the GS guard
 * band need no clipping, so they skip the EE's per-vertex work: their
 * triangles are prepared once per pack as VU-ready batches (24 triangles:
 * a GIF tag, positions, texture coordinates, colours) and each frame the EE
 * only writes a DMA chain of REF tags to them, with one small block per
 * node (matrix, viewport, fog). vu1_scene.vsm transforms them and kicks
 * them to the GS. Everything else (near the camera, off the guard band)
 * still goes through the EE clipper. */
#if defined(BLAZIUM_PS2_HAS_VU1) || defined(BLAZIUM_PS2_VU1_EMU)
#define GS_VU1 1
extern u32 VU1Scene_CodeStart __attribute__((section(".vudata")));
extern u32 VU1Scene_CodeEnd __attribute__((section(".vudata")));

#define VU_BATCH_TRIS 24
#define VU_BLOCK_QW 7
#define VU_MAX_BLOCKS 2048
#define VU_MAX_DRAWS 4096
#define VU_CHAIN_QW 12288 /* 3 qwords a batch, plus texture changes */

struct VuBatch {
	qword_t *blk; /* 1 + 3n qwords */
	unsigned short n; /* vertices */
	unsigned short tex;
};

#ifdef GS_VU1_DEFAULT_OFF
static int g_vu_on = 0;
#else
static int g_vu_on = 1; /* gs_draw_use_vu1() */
#endif
static int g_vu_kicked;
static int g_vu_ready;
static VuBatch *g_vb;
static unsigned g_vb_n;
static qword_t *g_vb_mem;
static unsigned g_chunk_vb0[GS_MAX_CHUNKS];
static unsigned short g_chunk_vbn[GS_MAX_CHUNKS];
static qword_t *g_vu_fb; /* per-frame blocks */
static unsigned g_vu_fb_n;
static unsigned g_vu_draw_chunk[VU_MAX_DRAWS];
static unsigned short g_vu_draw_blk[VU_MAX_DRAWS];
static unsigned g_vu_draw_n;
static int g_vu_nodeblk[GS_MAX_NODES];
static int g_vu_nodeblk_static;
static qword_t *g_vu_chain;
static Mat4 g_vu_mvp;
static float g_vu_gb[5][4]; /* guard band + near, normalised */
static int g_vu_w = 640, g_vu_h = 448;
static unsigned g_vu_stat_batches;

static void vu_free_batches(void)
{
	if (g_vb_mem) {
		free(g_vb_mem);
		g_vb_mem = 0;
	}
	if (g_vb) {
		free(g_vb);
		g_vb = 0;
	}
	g_vb_n = 0;
}

/* VIF codes */
#define VIF_NOP 0x00000000u
#define VIF_STCYCL(cl, wl) (0x01000000u | ((wl) << 8) | (cl))
#define VIF_OFFSET(o) (0x02000000u | (o))
#define VIF_BASE(b) (0x03000000u | (b))
#define VIF_FLUSH 0x11000000u
#define VIF_MSCAL(a) (0x14000000u | (a))
#define VIF_MPG(n, a) (0x4A000000u | (((n) & 0xFF) << 16) | (a))
#define VIF_DIRECT(n) (0x50000000u | (n))
#define VIF_UNPACK_V4_32(n, a) (0x6C000000u | (((n) & 0xFF) << 16) | 0x8000u | (a)) /* TOPS-relative */
/* DMA tags (with the two VIF codes in the upper half) */
#define DMA_ID_CNT 1u
#define DMA_ID_REF 3u
#define DMA_ID_END 7u

static qword_t *vu_tag(qword_t *q, unsigned id, unsigned qwc, const void *addr, u32 vif0, u32 vif1)
{
	u64 *dw = (u64 *)q;
	dw[0] = (u64)(qwc & 0xFFFF) | ((u64)id << 28) | ((u64)(u32)(unsigned long)addr << 32);
	dw[1] = (u64)vif0 | ((u64)vif1 << 32);
	return q + 1;
}

static void vu_send(qword_t *chain)
{
	FlushCache(0);
	dma_channel_send_chain(DMA_CHANNEL_VIF1, chain, 0, DMA_FLAG_TRANSFERTAG, 0);
	dma_channel_wait(DMA_CHANNEL_VIF1, 0);
}

static int vu_init(void)
{
	if (g_vu_ready) {
		return 1;
	}
	if (!g_vu_chain) {
		g_vu_chain = (qword_t *)memalign(64, VU_CHAIN_QW * 16);
		g_vu_fb = (qword_t *)memalign(64, VU_MAX_BLOCKS * VU_BLOCK_QW * 16);
		if (!g_vu_chain || !g_vu_fb) {
			return 0;
		}
	}
	const unsigned bytes = (unsigned)((unsigned long)&VU1Scene_CodeEnd - (unsigned long)&VU1Scene_CodeStart);
	const unsigned ninstr = bytes / 8;
	qword_t *q = g_vu_chain;
	dma_channel_initialize(DMA_CHANNEL_VIF1, NULL, 0);
	/* the program (one MPG: under 256 instructions), then the double buffer */
	q = vu_tag(q, DMA_ID_REF, bytes / 16, &VU1Scene_CodeStart, VIF_NOP, VIF_MPG(ninstr, 0));
	q = vu_tag(q, DMA_ID_CNT, 0, 0, VIF_STCYCL(1, 1), VIF_BASE(8));
	q = vu_tag(q, DMA_ID_END, 0, 0, VIF_OFFSET(496), VIF_NOP);
	vu_send(g_vu_chain);
	g_vu_ready = 1;
	return 1;
}

static u32 vu_col(unsigned char c)
{
	return (u32)((float)c / 255.0f * 128.0f);
}

/* Every chunk's triangles as VU batches: a new batch at 24 triangles or a
 * texture change. Built when a MESH is parsed. */
static void vu_build_batches(void)
{
	vu_free_batches();
	if (g_chunk_n == 0) {
		return;
	}
	/* count first: batches, then qwords */
	unsigned nb = 0;
	unsigned nqw = 0;
	for (unsigned ci = 0; ci < g_chunk_n; ci++) {
		const MeshChunk *ch = &g_chunks[ci];
		if (ch->r < 0.0f) {
			continue;
		}
		unsigned cur_tex = 0xFFFF, cur_n = 0;
		for (unsigned t = ch->first; t < ch->first + ch->n; t++) {
			const unsigned i0 = ru32(g_indices + t * 12);
			const unsigned tex = i0 < g_vert_count ? ru16(g_verts + i0 * 28 + 26) : 0;
			if (tex != cur_tex || cur_n == VU_BATCH_TRIS) {
				nb++;
				nqw += 1;
				cur_tex = tex;
				cur_n = 0;
			}
			cur_n++;
			nqw += 9;
		}
	}
	g_vb = (VuBatch *)malloc(nb * sizeof(VuBatch));
	g_vb_mem = (qword_t *)memalign(64, nqw * 16);
	if (!g_vb || !g_vb_mem) {
		vu_free_batches();
		return;
	}
	qword_t *at = g_vb_mem;
	for (unsigned ci = 0; ci < g_chunk_n; ci++) {
		MeshChunk *ch = &g_chunks[ci];
		g_chunk_vb0[ci] = g_vb_n;
		g_chunk_vbn[ci] = 0;
		if (ch->r < 0.0f) {
			continue;
		}
		unsigned t = ch->first;
		const unsigned end = ch->first + ch->n;
		while (t < end) {
			const unsigned i0 = ru32(g_indices + t * 12);
			const unsigned tex = i0 < g_vert_count ? ru16(g_verts + i0 * 28 + 26) : 0;
			unsigned nt = 0;
			while (t + nt < end && nt < VU_BATCH_TRIS) {
				const unsigned j0 = ru32(g_indices + (t + nt) * 12);
				const unsigned tj = j0 < g_vert_count ? ru16(g_verts + j0 * 28 + 26) : 0;
				if (tj != tex) {
					break;
				}
				nt++;
			}
			const unsigned n = nt * 3;
			VuBatch *b = &g_vb[g_vb_n++];
			b->blk = at;
			b->n = (unsigned short)n;
			b->tex = (unsigned short)tex;
			g_chunk_vbn[ci]++;
			/* GIF tag: PACKED ST, RGBAQ, XYZF2; triangles, Gouraud, textured,
			 * fogged (F = 255 is no fog); n in the unused w word */
			const u64 prim = 3u | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 9); /* CTXT 2 */
			u64 *dw = (u64 *)at;
			dw[0] = (u64)n | (1ull << 15) | (1ull << 46) | (prim << 47) | (3ull << 60);
			dw[1] = (u64)GIF_REG_ST | ((u64)GIF_REG_RGBAQ << 4) | ((u64)GIF_REG_XYZF2 << 8) | ((u64)n << 32);
			float *pos = (float *)(at + 1);
			float *st = (float *)(at + 1 + n);
			u32 *col = (u32 *)(at + 1 + 2 * n);
			for (unsigned k = 0; k < n; k++) {
				const unsigned vi = ru32(g_indices + (t + k / 3) * 12 + (k % 3) * 4);
				CookVert v;
				memset(&v, 0, sizeof(v));
				if (vi < g_vert_count) {
					read_vert(g_verts + vi * 28, &v);
				}
				pos[k * 4 + 0] = v.x;
				pos[k * 4 + 1] = v.y;
				pos[k * 4 + 2] = v.z;
				pos[k * 4 + 3] = 1.0f;
				st[k * 4 + 0] = v.u;
				st[k * 4 + 1] = v.v;
				st[k * 4 + 2] = 1.0f;
				st[k * 4 + 3] = 0.0f;
				col[k * 4 + 0] = vu_col(v.r);
				col[k * 4 + 1] = vu_col(v.g);
				col[k * 4 + 2] = vu_col(v.b);
				col[k * 4 + 3] = 0x80;
			}
			at += 1 + 3 * n;
			t += nt;
		}
	}
	FlushCache(0);
}

static void vu_frame_begin(const Mat4 *mvp, int w, int h)
{
	if (g_vu_on && !g_vu_ready) {
		vu_init();
	}
	g_vu_fb_n = 0;
	g_vu_draw_n = 0;
	g_vu_stat_batches = 0;
	g_vu_nodeblk_static = -1;
	memset(g_vu_nodeblk, 0xFF, sizeof(g_vu_nodeblk));
	g_vu_mvp = *mvp;
	g_vu_w = w > 0 ? w : 640;
	g_vu_h = h > 0 ? h : 448;
	const float *m = mvp->m;
	for (int k = 0; k < 4; k++) {
		const float rx = m[k * 4 + 0], ry = m[k * 4 + 1], rw = m[k * 4 + 3];
		g_vu_gb[0][k] = 6.0f * rw - rx;
		g_vu_gb[1][k] = 6.0f * rw + rx;
		g_vu_gb[2][k] = 8.0f * rw - ry;
		g_vu_gb[3][k] = 8.0f * rw + ry;
		g_vu_gb[4][k] = rw;
	}
	g_vu_gb[4][3] -= CLIP_NEAR_W;
	for (int p = 0; p < 5; p++) {
		const float l = sqrtf(g_vu_gb[p][0] * g_vu_gb[p][0] + g_vu_gb[p][1] * g_vu_gb[p][1] + g_vu_gb[p][2] * g_vu_gb[p][2]);
		if (l > 1e-6f) {
			for (int k = 0; k < 4; k++) {
				g_vu_gb[p][k] /= l;
			}
		}
	}
}

/* One per-frame block: MVP x node (z row (0,0,0,1)), viewport, fog. */
static int vu_block(const float *nm, int nofog)
{
	if (g_vu_fb_n >= VU_MAX_BLOCKS) {
		return -1;
	}
	Mat4 m = g_vu_mvp;
	if (nm) {
		Mat4 n;
		mat_ident(&n);
		for (int r = 0; r < 3; r++) {
			for (int c = 0; c < 4; c++) {
				n.m[c * 4 + r] = nm[r * 4 + c];
			}
		}
		mat_mul(&g_vu_mvp, &n, &m);
	}
	for (int c = 0; c < 4; c++) {
		m.m[c * 4 + 2] = c == 3 ? 1.0f : 0.0f;
	}
	float *f = (float *)(g_vu_fb + g_vu_fb_n * VU_BLOCK_QW);
	memcpy(f, m.m, 64);
	f[16] = (float)g_vu_w * 0.5f;
	f[17] = -(float)g_vu_h * 0.5f;
	f[18] = 0.25f * 16777215.0f;
	f[19] = 0.0f;
	f[20] = 2048.0f;
	f[21] = 2048.0f;
	f[22] = 0.0f;
	f[23] = 0.0f;
	if (g_fog_on && !nofog && g_fog_far > g_fog_near) {
		const float span = g_fog_far - g_fog_near;
		f[24] = -255.0f / span;
		f[25] = 255.0f * g_fog_far / span;
	} else {
		f[24] = 0.0f;
		f[25] = 255.0f;
	}
	f[26] = 255.0f;
	f[27] = 0.0f;
	return (int)g_vu_fb_n++;
}

/* Queue a chunk for VU1 if it needs no clipping: returns 1 if taken. The
 * centre is already in cooked space after any node matrix. */
static int vu_take(const DrawCtx *c, unsigned ci, float cx, float cy, float cz, float r, int node, int fresh)
{
	if (!g_vu_on || !g_vu_ready || !g_vb || c->tiny || g_tex_count == 0 || g_chunk_vbn[ci] == 0 || g_vu_draw_n >= VU_MAX_DRAWS) {
		return 0;
	}
	for (int p = 0; p < 5; p++) {
		if (g_vu_gb[p][0] * cx + g_vu_gb[p][1] * cy + g_vu_gb[p][2] * cz + g_vu_gb[p][3] < r) {
			return 0;
		}
	}
	const int moved = node >= 0 && node < GS_MAX_NODES && g_nodes_live && g_nm_on[node];
	const int nofog = node >= 0 && node < GS_MAX_NODES ? g_nfog[node] : 0;
	int blk;
	if (fresh) {
		blk = vu_block(moved ? g_nm[node] : 0, nofog);
	} else if (moved || nofog) {
		blk = g_vu_nodeblk[node];
		if (blk < 0) {
			blk = g_vu_nodeblk[node] = vu_block(moved ? g_nm[node] : 0, nofog);
		}
	} else {
		if (g_vu_nodeblk_static < 0) {
			g_vu_nodeblk_static = vu_block(0, 0);
		}
		blk = g_vu_nodeblk_static;
	}
	if (blk < 0) {
		return 0;
	}
	g_vu_draw_chunk[g_vu_draw_n] = ci;
	g_vu_draw_blk[g_vu_draw_n] = (unsigned short)blk;
	g_vu_draw_n++;
	return 1;
}

/* The frame's VU chain. It runs while the EE clips the rest; vu_wait()
 * before the HUD. */
static void vu_kick(void)
{
	if (g_vu_draw_n == 0 || !vu_init()) {
		return;
	}
	qword_t *q = g_vu_chain;
	qword_t *lim = g_vu_chain + VU_CHAIN_QW - 40;
	int cur_tex = -1;
	for (unsigned d = 0; d < g_vu_draw_n; d++) {
		const unsigned ci = g_vu_draw_chunk[d];
		const qword_t *fb = g_vu_fb + g_vu_draw_blk[d] * VU_BLOCK_QW;
		for (unsigned b = g_chunk_vb0[ci]; b < g_chunk_vb0[ci] + g_chunk_vbn[ci]; b++) {
			const VuBatch *vb = &g_vb[b];
			if (q >= lim) {
				q = vu_tag(q, DMA_ID_END, 0, 0, VIF_FLUSH, VIF_NOP);
				vu_send(g_vu_chain);
				q = g_vu_chain;
			}
			if ((int)vb->tex != cur_tex) {
				/* texture: after everything kicked so far, straight to the GIF */
				qword_t *tag = q++;
				qword_t *data = q;
				q = bind_tex_ctx(q, vb->tex, 1);
				PACK_GIFTAG(q, GIF_SET_TAG(0, 1, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
				q++;
				const unsigned k = (unsigned)(q - data);
				vu_tag(tag, DMA_ID_CNT, k, 0, VIF_FLUSH, VIF_DIRECT(k));
				cur_tex = vb->tex;
			}
			q = vu_tag(q, DMA_ID_REF, VU_BLOCK_QW, fb, VIF_NOP, VIF_UNPACK_V4_32(VU_BLOCK_QW, 0));
			q = vu_tag(q, DMA_ID_REF, 1 + 3u * vb->n, vb->blk, VIF_NOP, VIF_UNPACK_V4_32(1 + 3u * vb->n, VU_BLOCK_QW));
			q = vu_tag(q, DMA_ID_CNT, 0, 0, VIF_NOP, VIF_MSCAL(0));
			g_vu_stat_batches++;
		}
	}
	q = vu_tag(q, DMA_ID_END, 0, 0, VIF_FLUSH, VIF_NOP);
	FlushCache(0);
	dma_channel_send_chain(DMA_CHANNEL_VIF1, g_vu_chain, 0, DMA_FLAG_TRANSFERTAG, 0);
	g_vu_kicked = 1;
}

static void vu_wait(void)
{
	if (!g_vu_kicked) {
		return;
	}
	g_vu_kicked = 0;
	dma_channel_wait(DMA_CHANNEL_VIF1, 0);
#ifndef BLAZIUM_PS2_VU1_EMU
	/* The DMA is done once the VIF has the last tag; the VIF can still be
	 * in that FLUSH, and the last XGKICK still on its way to the GS. The
	 * HUD must not get in ahead of it: wait for the VIF (status, stalls and
	 * FIFO) and then for PATH1 (active path, queued request) to go idle. */
	volatile u32 *vif1_stat = (volatile u32 *)0x10003C00;
	volatile u32 *gif_stat = (volatile u32 *)0x10003020;
	for (int i = 0; i < 4000000 && (*vif1_stat & 0x1F00000Fu); i++) {
	}
	for (int i = 0; i < 4000000 && (*gif_stat & ((3u << 10) | (1u << 6))); i++) {
	}
#endif
}
#else
static void vu_build_batches(void) {}
static void vu_frame_begin(const Mat4 *, int, int) {}
static int vu_take(const DrawCtx *, unsigned, float, float, float, float, int, int) { return 0; }
static void vu_kick(void) {}
static void vu_wait(void) {}
#endif

static unsigned g_vu_stat_frame(void)
{
#ifdef GS_VU1
	return g_vu_stat_batches;
#else
	return 0;
#endif
}

static unsigned g_vu_draw_n_frame(void)
{
#ifdef GS_VU1
	return g_vu_draw_n;
#else
	return 0;
#endif
}

void gs_draw_use_vu1(int on)
{
#ifdef GS_VU1
	g_vu_on = on ? 1 : 0;
#else
	(void)on;
#endif
}

/* With VU1 on, the EE first only sorts the frame's chunks: VU1's go in the
 * chain, its own wait here until the chain is kicked, then it clips them
 * while VU1 draws. */
struct CpuJob {
	unsigned ci;
	int inst; /* -1: an ordinary chunk; else a node whose matrix is m */
	float m[12];
};
#define GS_MAX_JOBS 2048
static CpuJob g_jobs[GS_MAX_JOBS];
static unsigned g_job_n;
static int g_defer;

static void defer_or_draw(DrawCtx *c, unsigned ci, int inst_node, const float *m)
{
	const MeshChunk *ch = &g_chunks[ci];
	if (g_defer && g_job_n < GS_MAX_JOBS) {
		CpuJob *j = &g_jobs[g_job_n++];
		j->ci = ci;
		j->inst = inst_node;
		if (m) {
			memcpy(j->m, m, sizeof(j->m));
		}
		return;
	}
	draw_range(c, ch->first, ch->first + ch->n);
}

/* 3x4 row-major a * b */
static void m34_mul(const float *a, const float *b, float *o)
{
	for (int r = 0; r < 3; r++) {
		for (int k = 0; k < 4; k++) {
			o[r * 4 + k] = a[r * 4 + 0] * b[0 + k] + a[r * 4 + 1] * b[4 + k] + a[r * 4 + 2] * b[8 + k] +
					(k == 3 ? a[r * 4 + 3] : 0.0f);
		}
	}
}

/* Every instance: its source node's chunks, culled by the moved sphere (and
 * dropped past the fog), drawn with the instance delta as the node matrix. */
static void draw_instances(DrawCtx *c, float pl[6][4])
{
	if (!g_inst || g_inst_n == 0) {
		return;
	}
	const int live = g_nodes_live;
	g_nodes_live = 1;
	for (unsigned i = 0; i < g_inst_n; i++) {
		const unsigned char *row = g_inst + i * 52;
		const unsigned nd = ru16(row);
		if (nd >= GS_MAX_NODES || g_isrc[nd] != 2 || g_nhide[nd]) {
			continue;
		}
		float d[12];
		for (int k = 0; k < 12; k++) {
			d[k] = rf32(row + 4 + k * 4);
		}
		float m[12];
		if (g_nm_on[nd]) {
			m34_mul(g_nm[nd], d, m);
		} else {
			memcpy(m, d, sizeof(m));
		}
		/* largest axis scale, for the sphere radius */
		float sc2 = 0.0f;
		for (int k = 0; k < 3; k++) {
			const float l = m[k] * m[k] + m[4 + k] * m[4 + k] + m[8 + k] * m[8 + k];
			if (l > sc2) {
				sc2 = l;
			}
		}
		if (sc2 < 1e-12f) {
			continue;
		}
		const float sc = sqrtf(sc2);
		float keep[12];
		const unsigned char keep_on = g_nm_on[nd];
		memcpy(keep, g_nm[nd], sizeof(keep));
		memcpy(g_nm[nd], m, sizeof(m));
		g_nm_on[nd] = 1;
		c->nofog = g_nfog[nd];
		for (unsigned ci = g_isrc_c0[nd]; ci < g_isrc_c1[nd]; ci++) {
			const MeshChunk *ch = &g_chunks[ci];
			const float cx = m[0] * ch->c[0] + m[1] * ch->c[1] + m[2] * ch->c[2] + m[3];
			const float cy = m[4] * ch->c[0] + m[5] * ch->c[1] + m[6] * ch->c[2] + m[7];
			const float cz = m[8] * ch->c[0] + m[9] * ch->c[1] + m[10] * ch->c[2] + m[11];
			const float r = ch->r * sc;
			int out = 0;
			for (int p = 0; p < 6 && !out; p++) {
				if (pl[p][0] * cx + pl[p][1] * cy + pl[p][2] * cz + pl[p][3] < -r) {
					out = 1;
				}
			}
			/* wholly in the fog: the near plane's distance is the view depth */
			if (!out && g_fog_on && !c->nofog &&
					pl[4][0] * cx + pl[4][1] * cy + pl[4][2] * cz + pl[4][3] + CLIP_NEAR_W - r > g_fog_far) {
				out = 1;
			}
			if (out) {
				continue;
			}
			g_stat_chunks++;
			if (vu_take(c, ci, cx, cy, cz, r, (int)nd, 1)) {
				continue;
			}
			defer_or_draw(c, ci, (int)nd, m);
		}
		memcpy(g_nm[nd], keep, sizeof(keep));
		g_nm_on[nd] = keep_on;
	}
	g_nodes_live = live;
}

/* Send what the EE has so far (the clear, fog, and context 2 for VU1), kick
 * VU1's chain, then clip the EE's own chunks while it runs. */
static void run_deferred(DrawCtx *c)
{
	if (!g_defer) {
		return;
	}
	g_defer = 0;
	ctx_flush(c);
	vu_kick();
	const int live = g_nodes_live;
	for (unsigned k = 0; k < g_job_n; k++) {
		const CpuJob *j = &g_jobs[k];
		const MeshChunk *ch = &g_chunks[j->ci];
		if (j->inst >= 0) {
			const int nd = j->inst;
			float keep[12];
			const unsigned char keep_on = g_nm_on[nd];
			memcpy(keep, g_nm[nd], sizeof(keep));
			memcpy(g_nm[nd], j->m, sizeof(j->m));
			g_nm_on[nd] = 1;
			g_nodes_live = 1;
			c->nofog = g_nfog[nd];
			draw_range(c, ch->first, ch->first + ch->n);
			memcpy(g_nm[nd], keep, sizeof(keep));
			g_nm_on[nd] = keep_on;
			g_nodes_live = live;
		} else {
			c->nofog = ch->node < GS_MAX_NODES ? g_nfog[ch->node] : 0;
			draw_range(c, ch->first, ch->first + ch->n);
		}
	}
	g_job_n = 0;
}

void gs_draw_nodes_begin(void)
{
	g_nodes_live = 1;
}

void gs_draw_node_matrix(int node, const float *m)
{
	if (node < 0 || node >= GS_MAX_NODES) {
		return;
	}
	if (!m) {
		g_nm_on[node] = 0;
		return;
	}
	memcpy(g_nm[node], m, sizeof(g_nm[node]));
	g_nm_on[node] = 1;
}

void gs_draw_node_hidden(int node, int hidden)
{
	if (node < 0 || node >= GS_MAX_NODES) {
		return;
	}
	g_nhide[node] = hidden ? 1 : 0;
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

static qword_t *view_regs(qword_t *q, int ctx, float ox, float oy, int x0, int y0, int x1, int y1)
{
	PACK_GIFTAG(q, GIF_SET_TAG(2, 0, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
	q++;
	PACK_GIFTAG(q, GS_SET_XYOFFSET((int)(ox * 16.0f), (int)(oy * 16.0f)), GS_REG_XYOFFSET + ctx);
	q++;
	PACK_GIFTAG(q, GS_SET_SCISSOR(x0, x1, y0, y1), GS_REG_SCISSOR + ctx);
	q++;
	return q;
}

void gs_draw_set_views(int n)
{
	g_view_n = n >= 2 && n <= GS_MAX_VIEWS ? n : 0;
}

void gs_draw_view_look(int i, float ex, float ey, float ez, float tx, float ty, float tz, float fov_deg)
{
	if (i < 0 || i >= GS_MAX_VIEWS) {
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

/* split-screen view i of n in a w x h frame */
static void view_rect(int i, int n, int w, int h, int *x, int *y, int *vw, int *vh)
{
	if (n == 2) {
		*x = 0;
		*y = i * (h / 2);
		*vw = w;
		*vh = h / 2;
		return;
	}
	*x = (i & 1) * (w / 2);
	*y = (i >> 1) * (h / 2);
	*vw = w / 2;
	*vh = h / 2;
}

/* One view of the scene: the camera as it stands, NDC -1..1 onto the
 * vw x vh rectangle at (vx, vy). split: one of several views, so the view
 * sets its own XYOFFSET and SCISSOR and keeps the camera it was given. */
static qword_t *draw_view(packet_t *packet, qword_t *q, qword_t *limit, framebuffer_t *frame, zbuffer_t *z, float ox,
		float oy, int vx, int vy, int vw, int vh, int split, unsigned *drawn, int *in_prim)
{
	const float vox = split ? 2048.0f - (float)vx - (float)(vw / 2) : ox;
	const float voy = split ? 2048.0f - (float)vy - (float)(vh / 2) : oy;
	Mat4 world, view, proj, tmp, mvp;
	yaw_y(&world, g_world_yaw);
	float ex = 0.0f, ey = 0.0f, ez = 0.0f;
	eye_now(&ex, &ey, &ez);
	if (!split && !g_cam_locked && (aabb_is_tiny() || cam_cannot_see_aabb(ex, ey, ez))) {
		frame_aabb();
		eye_now(&ex, &ey, &ez);
	}
	look_at(&view, ex, ey, ez, g_look_x, g_look_y, g_look_z);
	if (g_ortho && !g_have_aabb) {
		ortho_proj(&proj, (float)vw / (float)(vh ? vh : 1), 0.25f, 400.0f);
	} else {
		perspective(&proj, g_fov, (float)vw / (float)(vh ? vh : 1), 0.25f, 400.0f);
	}
	mat_mul(&view, &world, &tmp);
	mat_mul(&proj, &tmp, &mvp);
	if (g_mesh_dirty_vu) {
		g_mesh_dirty_vu = 0;
		vu_build_batches();
	}
	g_vp_w = vw;
	g_vp_h = vh;
	vu_frame_begin(&mvp, vw, vh);
	if (split) {
		/* this view's part of the frame: NDC centre on its middle, nothing
		 * drawn past its edges (VU1 leaves the guard band to the scissor) */
		q = view_regs(q, 0, vox, voy, vx, vy, vx + vw - 1, vy + vh - 1);
	}

	DrawCtx c;
	memset(&c, 0, sizeof(c));
	c.packet = packet;
	c.q = q;
	c.limit = limit;
	c.frame = frame;
	c.z = z;
	c.mvp = &mvp;
	c.bound = -1;
	c.prim.type = PRIM_TRIANGLE;
	c.prim.shading = PRIM_SHADE_GOURAUD;
	c.prim.mapping = (g_tex_count > 0 && !aabb_is_tiny()) ? DRAW_ENABLE : DRAW_DISABLE;
	c.prim.mapping_type = PRIM_MAP_ST;
	c.prim.colorfix = PRIM_UNFIXED;
	c.prim.fogging = g_fog_on ? DRAW_ENABLE : DRAW_DISABLE;
	if (g_fog_on) {
		PACK_GIFTAG(q, GIF_SET_TAG(1, 0, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
		q++;
		PACK_GIFTAG(q, GS_SET_FOGCOL(g_fog_r, g_fog_g, g_fog_b), GS_REG_FOGCOL);
		q++;
	}
#ifdef GS_VU1
	g_job_n = 0;
	/* the GS has one CLUT buffer for both contexts: palettised textures
	 * keep VU1 and the EE in turn */
	int any_clut = 0;
	for (int t = 0; t < g_tex_count; t++) {
		if (g_tex[t].ready && g_tex[t].clut_count > 0) {
			any_clut = 1;
		}
	}
	g_defer = g_vu_on && g_vu_ready && g_vb && !c.tiny && !any_clut;
	if (g_vu_on && g_vu_ready && g_vb && !c.tiny) {
		/* context 2 mirrors context 1 for VU1's triangles */
		q = draw_setup_environment(q, 1, frame, z);
		q = draw_primitive_xyoffset(q, 1, vox, voy);
		q = draw_enable_tests(q, 1, z);
		if (split) {
			q = view_regs(q, 1, vox, voy, vx, vy, vx + vw - 1, vy + vh - 1);
		}
		if (g_fog_on) {
			PACK_GIFTAG(q, GIF_SET_TAG(1, 0, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
			q++;
			PACK_GIFTAG(q, GS_SET_FOGCOL(g_fog_r, g_fog_g, g_fog_b), GS_REG_FOGCOL);
			q++;
		}
	}
#endif
	c.q = q;
	c.color.r = 0x80;
	c.color.g = 0x80;
	c.color.b = 0x80;
	c.color.a = 0x80;
	c.color.q = 1.0f;
	c.tiny = aabb_is_tiny();

	const unsigned ntri = g_index_count / 3;
	if (g_chunk_n == 0) {
		draw_range(&c, 0, ntri);
	} else {
		/* frustum planes from the rows of mvp (column-major): left, right,
		 * bottom, top, near (w >= CLIP_NEAR_W), far (w <= 400) */
		const float *m = mvp.m;
		float pl[6][4];
		for (int k = 0; k < 4; k++) {
			const float rx = m[k * 4 + 0], ry = m[k * 4 + 1], rw = m[k * 4 + 3];
			pl[0][k] = rw + rx;
			pl[1][k] = rw - rx;
			pl[2][k] = rw + ry;
			pl[3][k] = rw - ry;
			pl[4][k] = rw;
			pl[5][k] = -rw;
		}
		pl[4][3] -= CLIP_NEAR_W;
		pl[5][3] += 400.0f;
		for (int p = 0; p < 6; p++) {
			const float l = sqrtf(pl[p][0] * pl[p][0] + pl[p][1] * pl[p][1] + pl[p][2] * pl[p][2]);
			if (l > 1e-6f) {
				for (int k = 0; k < 4; k++) {
					pl[p][k] /= l;
				}
			}
		}
		for (unsigned ci = 0; ci < g_chunk_n; ci++) {
			const MeshChunk *ch = &g_chunks[ci];
			const unsigned n = ch->node;
			if (n < GS_MAX_NODES && g_isrc[n]) {
				continue; /* drawn once per instance below */
			}
			c.nofog = n < GS_MAX_NODES ? g_nfog[n] : 0;
			if (ch->r >= 0.0f && n != 0xffff) {
				if (g_nodes_live && n < GS_MAX_NODES && g_nhide[n]) {
					continue;
				}
				int cull = 1;
				float cx = ch->c[0], cy = ch->c[1], cz = ch->c[2];
				if (g_nodes_live && n < GS_MAX_NODES && g_nm_on[n]) {
					const float *w = g_nm[n];
					const float qx = w[0] * cx + w[1] * cy + w[2] * cz + w[3];
					const float qy = w[4] * cx + w[5] * cy + w[6] * cz + w[7];
					const float qz = w[8] * cx + w[9] * cy + w[10] * cz + w[11];
					cx = qx;
					cy = qy;
					cz = qz;
				}
				if (n < 32 && (g_nofs[n][0] != 0.0f || g_nofs[n][1] != 0.0f || g_nofs[n][2] != 0.0f ||
									  g_nrot[n][1] != 0.0f)) {
					cull = 0; /* legacy tape offsets: positions move outside g_nm */
				}
				if (cull) {
					int out = 0;
					for (int p = 0; p < 6 && !out; p++) {
						if (pl[p][0] * cx + pl[p][1] * cy + pl[p][2] * cz + pl[p][3] < -ch->r) {
							out = 1;
						}
					}
					/* wholly in the fog: nothing to see */
					if (!out && g_fog_on && !c.nofog &&
							pl[4][0] * cx + pl[4][1] * cy + pl[4][2] * cz + pl[4][3] + CLIP_NEAR_W - ch->r > g_fog_far) {
						out = 1;
					}
					if (out) {
						continue;
					}
					g_stat_chunks++;
					if (vu_take(&c, ci, cx, cy, cz, ch->r, (int)n, 0)) {
						continue;
					}
					defer_or_draw(&c, ci, -1, 0);
					continue;
				}
			}
			g_stat_chunks++;
			draw_range(&c, ch->first, ch->first + ch->n);
		}
		draw_instances(&c, pl);
		run_deferred(&c);
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
			draw_tri(&c, &v0, &v1, &v2);
		}
	}
#ifdef GS_VU1
	if (g_vu_draw_n > 0) {
		/* the EE's triangles and VU1's both in, then the HUD (or the next
		 * view, which reuses VU1's blocks) */
		run_deferred(&c);
		ctx_flush(&c);
		if (!g_vu_kicked) {
			vu_kick();
		}
		vu_wait();
	}
#endif
	*drawn += c.drawn + g_vu_draw_n_frame();
	*in_prim = c.in_prim;
	return c.q;
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
	q = draw_clear(q, 0, ox, oy, (float)frame->width, (float)frame->height, g_clear_r, g_clear_g, g_clear_b);
	if (!aabb_is_tiny()) {
		q = draw_enable_tests(q, 0, z);
	}
	if (!g_ready) {
		g_hud_packet = packet;
		g_hud_limit = limit;
		q = emit_hud(q, frame->width, frame->height);
		g_hud_packet = 0;
		q = draw_finish(q);
		send_packet(packet, q);
		packet_free(packet);
		return;
	}

	const int nv = g_view_n >= 2 ? g_view_n : 1;
	unsigned drawn = 0;
	int in_prim = 0;
	if (nv == 1) {
		q = draw_view(packet, q, limit, frame, z, ox, oy, 0, 0, frame->width, frame->height, 0, &drawn, &in_prim);
	} else {
		/* each view with its own camera; the scripted one comes back after */
		const float kx = g_cam_x, ky = g_cam_y, kz = g_cam_z;
		const float lx = g_look_x, lyy = g_look_y, lz = g_look_z, kf = g_fov;
		const float ax = g_attach_x, ay = g_attach_y, az = g_attach_z, eh = g_eye_h;
		g_attach_x = g_attach_y = g_attach_z = 0.0f;
		g_eye_h = 0.0f;
		for (int v = 0; v < nv; v++) {
			int vx = 0, vy = 0, vw = 0, vh = 0;
			view_rect(v, nv, frame->width, frame->height, &vx, &vy, &vw, &vh);
			const ViewCam *vc = &g_views[v];
			g_cam_x = vc->ex;
			g_cam_y = vc->ey;
			g_cam_z = vc->ez;
			g_look_x = vc->tx;
			g_look_y = vc->ty;
			g_look_z = vc->tz;
			g_fov = vc->fov;
			if (v > 0) {
				q = end_prim(q, &in_prim);
			}
			q = draw_view(packet, q, limit, frame, z, ox, oy, vx, vy, vw, vh, 1, &drawn, &in_prim);
		}
		g_cam_x = kx;
		g_cam_y = ky;
		g_cam_z = kz;
		g_look_x = lx;
		g_look_y = lyy;
		g_look_z = lz;
		g_fov = kf;
		g_attach_x = ax;
		g_attach_y = ay;
		g_attach_z = az;
		g_eye_h = eh;
		g_vp_w = frame->width;
		g_vp_h = frame->height;
		q = end_prim(q, &in_prim);
		/* the HUD in whole-frame coordinates again */
		q = view_regs(q, 0, ox, oy, 0, 0, frame->width - 1, frame->height - 1);
	}
#ifdef GS_FRAMELOG
	{
		/* one line a frame: where the camera is and what the frame cost */
		static unsigned fl_frame;
		unsigned vu_verts = 0;
#ifdef GS_VU1
		for (unsigned d = 0; d < g_vu_draw_n; d++) {
			const unsigned ci = g_vu_draw_chunk[d];
			for (unsigned b = g_chunk_vb0[ci]; b < g_chunk_vb0[ci] + g_chunk_vbn[ci]; b++) {
				vu_verts += g_vb[b].n;
			}
		}
#endif
		extern unsigned gdir_vm_last_ops(void);
		printf("FRAME %u cam %.1f %.1f %.1f chunks %u ee_tested %u ee_drawn %u vu_batches %u vu_tris %u ops %u\n", fl_frame++,
				g_cam_x, g_cam_y, g_cam_z, g_stat_chunks, g_stat_tris, drawn, g_vu_stat_frame(), vu_verts / 3,
				gdir_vm_last_ops());
	}
#endif
#ifdef GS_STATS
	if (++g_stat_frames % 300 == 0) {
		printf("GS stats: %u chunks, %u of %u tris tested, %u drawn by the EE, %u VU1 batches (this frame)\n", g_stat_chunks,
				g_stat_tris, g_index_count / 3, drawn, g_vu_stat_frame());
	}
#endif
	g_stat_tris = 0;
	g_stat_chunks = 0;
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
	g_hud_packet = packet;
	g_hud_limit = limit;
	q = emit_hud(q, frame->width, frame->height);
	g_hud_packet = 0;
	q = draw_finish(q);
	send_packet(packet, q);
	packet_free(packet);
}
