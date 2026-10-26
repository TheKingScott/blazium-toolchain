// MIT. The PS2 guest's draw (gs_draw.cpp) on the PS3's RSX: the same ABI 1
// MESH / GTEX / NODE packs, cameras, chunks and culling, node matrices,
// instances, fog, split-screen views and HUD; the PPU transforms and clips
// each triangle as the PS2's EE does, and the RSX rasterises them (a
// pass-through vertex program; the fragment program is the GS's MODULATE
// and FOG). Textures are expanded to A8R8G8B8 in RSX memory and filtered;
// the frame is the TV's (720p, else 480p/576p), the 3D view fills it (16:9
// on a wide TV) and the HUD keeps the PS2's 640x448 space, centred at 4:3.
// The cooked packs are little-endian; the PPU is big-endian, so every
// field is read byte by byte.

#include "gs_draw.h"

#include "rsx_shaders.h"

#include <malloc.h>
#include <math.h>
#include <ppu-types.h>
#include <rsx/rsx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysutil/video.h>
#include <unistd.h>

#ifndef BLAZIUM_PS2_COOK_ABI
#define BLAZIUM_PS2_COOK_ABI 1
#endif

#define GS_MAX_TEX 64
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

/* A GTEX texture in RSX memory, A8R8G8B8 (vram: its RSX offset). */
struct UploadedTex {
	int ready;
	int hw_psm; /* the cook's PSM (0 T4, 1 T8, 2 CT16) */
	int width;
	int height;
	int vram;
	void *mem;
};

struct Mat4 {
	float m[16];
};

struct color_f_t {
	float r, g, b, a;
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


/* ---- vertices: the cooked 28 bytes, little-endian ---- */

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

/* ---- the RSX ---- */

#define CB_SIZE 0x100000          /* command buffer */
#define HOST_SIZE (32 * 1024 * 1024) /* main memory the RSX can see (the command buffer is in it) */
#define LABEL_INDEX 255
/* the frame's vertices: written by the PPU, read by the RSX; two, so a frame
 * is built while the last one is drawn */
#define VB_VERTS (256 * 1024)

struct GVert {
	float x, y, z, w;
	unsigned char r, g, b, a;     /* COLOR0: the vertex colour (a: the fade's blend) */
	unsigned char fr, fg, fb, fa; /* COLOR1: the fog colour, fa the surface left (255: no fog) */
	float u, v;
};
typedef char gvert_is_32_bytes[sizeof(GVert) == 32 ? 1 : -1];

static gcmContextData *g_ctx;
static int g_booted;
static u32 g_dw = 1280, g_dh = 720;
static u32 g_color_pitch, g_depth_pitch;
static u32 *g_color_buf[2];
static u32 g_color_off[2];
static u32 *g_depth_buf;
static u32 g_depth_off;
static u32 g_fb;
static int g_first_flip = 1;
static u32 g_label = 1;
static rsxVertexProgram *g_vpo;
static rsxFragmentProgram *g_fpo;
static void *g_vp_ucode;
static u32 g_fp_off;
static int g_tex_unit;
static GVert *g_vb[2];
static u32 g_vb_off[2];
static GVert *g_vbuf; /* this frame's */
static unsigned g_vn, g_vmark;
static int g_bound = -3; /* the texture bound for the batch: -1 white, -2 nearest-sampled HUD */
static u32 *g_white;
static u32 g_white_off;
/* the HUD's space (the PS2's framebuffer, DISP.BIN) on the display */
static int g_hud_w = 640, g_hud_h = 448;
static float g_hud_sx = 1.5f, g_hud_sy = 1.607f, g_hud_ox = 160.0f;
/* the view being drawn (pixels): NDC -1..1 spans it */
static int g_vx, g_vy;

static void rsx_wait_idle(void)
{
	rsxSetWriteBackendLabel(g_ctx, LABEL_INDEX, g_label);
	rsxFlushBuffer(g_ctx);
	while (*(vu32 *)gcmGetLabelAddress(LABEL_INDEX) != g_label) {
		usleep(30);
	}
	g_label++;
}

static void set_target(u32 fb)
{
	gcmSurface sf;
	memset(&sf, 0, sizeof(sf));
	sf.colorFormat = GCM_SURFACE_X8R8G8B8;
	sf.colorTarget = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0] = g_color_off[fb];
	sf.colorPitch[0] = g_color_pitch;
	for (int i = 1; i < 4; i++) {
		sf.colorLocation[i] = GCM_LOCATION_RSX;
		sf.colorOffset[i] = 0;
		sf.colorPitch[i] = 64;
	}
	sf.depthFormat = GCM_SURFACE_ZETA_Z24S8;
	sf.depthLocation = GCM_LOCATION_RSX;
	sf.depthOffset = g_depth_off;
	sf.depthPitch = g_depth_pitch;
	sf.type = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias = GCM_SURFACE_CENTER_1;
	sf.width = (u16)g_dw;
	sf.height = (u16)g_dh;
	sf.x = 0;
	sf.y = 0;
	rsxSetSurface(g_ctx, &sf);
}

static int video_setup(void)
{
	static const u32 ids[] = { VIDEO_RESOLUTION_720, VIDEO_RESOLUTION_480, VIDEO_RESOLUTION_576 };
	videoResolution res;
	memset(&res, 0, sizeof(res));
	s32 id = -1;
	for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
		if (videoGetResolutionAvailability(VIDEO_PRIMARY, ids[i], VIDEO_ASPECT_AUTO, 0) != 1) {
			continue;
		}
		if (videoGetResolution(ids[i], &res) == 0) {
			id = (s32)ids[i];
			break;
		}
	}
	if (id < 0) {
		/* the mode the system is in already */
		videoState st;
		if (videoGetState(VIDEO_PRIMARY, 0, &st) != 0 || videoGetResolution(st.displayMode.resolution, &res) != 0) {
			printf("PS3 video: no usable resolution\n");
			return 0;
		}
		id = st.displayMode.resolution;
	}
	videoConfiguration cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.resolution = (u8)id;
	cfg.format = VIDEO_BUFFER_FORMAT_XRGB;
	cfg.aspect = VIDEO_ASPECT_AUTO;
	cfg.pitch = (u32)res.width * 4;
	if (videoConfigure(VIDEO_PRIMARY, &cfg, NULL, 0) != 0) {
		printf("PS3 video: videoConfigure failed\n");
		return 0;
	}
	g_dw = res.width;
	g_dh = res.height;
	return 1;
}

int gs_draw_boot(int hud_w, int hud_h)
{
	if (g_booted) {
		return 1;
	}
	void *host = memalign(1024 * 1024, HOST_SIZE);
	if (!host || rsxInit(&g_ctx, CB_SIZE, HOST_SIZE, host) != 0 || !g_ctx) {
		printf("PS3 RSX: rsxInit failed\n");
		return 0;
	}
	if (!video_setup()) {
		return 0;
	}
	rsx_wait_idle();
	gcmSetFlipMode(GCM_FLIP_VSYNC);
	g_color_pitch = g_dw * 4;
	g_depth_pitch = g_dw * 4;
	for (int i = 0; i < 2; i++) {
		g_color_buf[i] = (u32 *)rsxMemalign(64, g_dh * g_color_pitch);
		if (!g_color_buf[i]) {
			return 0;
		}
		rsxAddressToOffset(g_color_buf[i], &g_color_off[i]);
		gcmSetDisplayBuffer((u8)i, g_color_off[i], g_color_pitch, g_dw, g_dh);
	}
	g_depth_buf = (u32 *)rsxMemalign(64, g_dh * g_depth_pitch);
	if (!g_depth_buf) {
		return 0;
	}
	rsxAddressToOffset(g_depth_buf, &g_depth_off);
	for (int i = 0; i < 2; i++) {
		g_vb[i] = (GVert *)rsxMemalign(128, VB_VERTS * sizeof(GVert));
		if (!g_vb[i]) {
			printf("PS3 RSX: no room for the vertex buffers\n");
			return 0;
		}
		rsxAddressToOffset(g_vb[i], &g_vb_off[i]);
	}
	/* the programs: the vertex program's microcode goes in the command
	 * stream, the fragment program's has to be in RSX memory */
	g_vpo = (rsxVertexProgram *)(void *)gdir_vp_bin;
	g_fpo = (rsxFragmentProgram *)(void *)gdir_fp_bin;
	u32 vsize = 0, fsize = 0;
	void *fp_ucode = 0;
	rsxVertexProgramGetUCode(g_vpo, &g_vp_ucode, &vsize);
	rsxFragmentProgramGetUCode(g_fpo, &fp_ucode, &fsize);
	void *fp = rsxMemalign(64, fsize);
	if (!fp) {
		return 0;
	}
	memcpy(fp, fp_ucode, fsize);
	rsxAddressToOffset(fp, &g_fp_off);
	rsxProgramAttrib *tex = rsxFragmentProgramGetAttrib(g_fpo, "tex");
	g_tex_unit = tex ? tex->index : 0;
	/* a white texel: untextured triangles and the HUD's boxes */
	g_white = (u32 *)rsxMemalign(128, 16 * 4);
	if (!g_white) {
		return 0;
	}
	for (int i = 0; i < 16; i++) {
		g_white[i] = 0xffffffffu;
	}
	rsxAddressToOffset(g_white, &g_white_off);
	gs_draw_set_hud_size(hud_w, hud_h);
	g_booted = 1;
	set_target(g_fb);
	printf("PS3 RSX: %ux%u, HUD %dx%d at %.2fx%.2f\n", (unsigned)g_dw, (unsigned)g_dh, g_hud_w, g_hud_h, g_hud_sx, g_hud_sy);
	return 1;
}

void gs_draw_set_hud_size(int w, int h)
{
	g_hud_w = w > 0 ? w : 640;
	g_hud_h = h > 0 ? h : 448;
	/* the PS2's frame fills a 4:3 TV's height: its pixels are as wide as
	 * 4:3 makes them, centred on a wide TV */
	const float th = (float)g_dh;
	const float tw = th * 4.0f / 3.0f > (float)g_dw ? (float)g_dw : th * 4.0f / 3.0f;
	g_hud_sx = tw / (float)g_hud_w;
	g_hud_sy = th / (float)g_hud_h;
	g_hud_ox = ((float)g_dw - tw) * 0.5f;
}

void gs_draw_display_size(int *w, int *h)
{
	if (w) {
		*w = (int)g_dw;
	}
	if (h) {
		*h = (int)g_dh;
	}
}

/* ---- textures ---- */

/* PS2 PSMCT16 (and the cooked CLUT): R 0-4, G 5-9, B 10-14, alpha bit 15 */
static u32 argb_from_555(unsigned c)
{
	const unsigned r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
	return 0xff000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2));
}

static unsigned clut555(const unsigned char *clut, unsigned clut_n, unsigned idx)
{
	if (!clut || idx >= clut_n) {
		return 0;
	}
	return (unsigned)clut[idx * 2] | ((unsigned)clut[idx * 2 + 1] << 8);
}

static void free_textures(void)
{
	int any = 0;
	for (int i = 0; i < GS_MAX_TEX; i++) {
		if (g_tex[i].mem) {
			any = 1;
		}
	}
	if (!any) {
		return;
	}
	/* the frame the RSX is drawing may still sample them */
	rsx_wait_idle();
	for (int i = 0; i < GS_MAX_TEX; i++) {
		if (g_tex[i].mem) {
			rsxFree(g_tex[i].mem);
		}
	}
}

static int upload_one(UploadedTex *t, const unsigned char *data, unsigned data_n, const unsigned char *clut, unsigned clut_n)
{
	const unsigned n = (unsigned)t->width * (unsigned)t->height;
	u32 *dst = (u32 *)rsxMemalign(128, n * 4);
	if (!dst) {
		printf("PS3 RSX: no room for a %dx%d texture\n", t->width, t->height);
		return 0;
	}
	for (unsigned i = 0; i < n; i++) {
		unsigned c = 0;
		if (t->hw_psm == COOK_PSM_T4) {
			const unsigned s = i / 2;
			const unsigned idx = s < data_n ? ((data[s] >> ((i & 1u) * 4u)) & 0xfu) : 0;
			c = clut_n ? clut555(clut, clut_n, idx) : idx * 0x0842u;
		} else if (t->hw_psm == COOK_PSM_T8) {
			const unsigned idx = i < data_n ? data[i] : 0;
			c = clut_n ? clut555(clut, clut_n, idx) : (idx >> 3) * 0x0421u;
		} else {
			c = i * 2 + 1 < data_n ? ((unsigned)data[i * 2] | ((unsigned)data[i * 2 + 1] << 8)) : 0;
		}
		dst[i] = argb_from_555(c);
	}
	t->mem = dst;
	u32 off = 0;
	rsxAddressToOffset(dst, &off);
	t->vram = (int)off;
	t->ready = 1;
	return 1;
}

static void parse_upload_gtex(const unsigned char *blob, unsigned sz)
{
	free_textures();
	g_tex_count = 0;
	memset(g_tex, 0, sizeof(g_tex));
	g_bound = -3;
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
		t->hw_psm = (int)blob[off + 4];
		const unsigned clut_count = ru16(blob + off + 6);
		const unsigned data_n = ru32(blob + off + 8);
		off += 12;
		if (off + data_n + clut_count * 2 > sz) {
			break;
		}
		const unsigned char *data = blob + off;
		off += data_n;
		const unsigned char *clut = blob + off;
		off += clut_count * 2;
		if (t->width <= 0 || t->height <= 0 || t->width > 4096 || t->height > 4096) {
			continue;
		}
		/* a texture that fails keeps its slot (drawn white), so the
		 * vertices' slot numbers still match */
		upload_one(t, data, data_n, clut, clut_count);
		g_tex_count++;
	}
}

/* Bind slot (or -1 white, -2 white sampled nearest) for the next batch. */
static void bind_tex(int slot, int nearest)
{
	u32 off = g_white_off;
	int w = 4, h = 4;
	if (slot >= 0 && slot < g_tex_count && g_tex[slot].ready) {
		off = (u32)g_tex[slot].vram;
		w = g_tex[slot].width;
		h = g_tex[slot].height;
	}
	gcmTexture tex;
	memset(&tex, 0, sizeof(tex));
	tex.format = (u8)(GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN);
	tex.mipmap = 1;
	tex.dimension = GCM_TEXTURE_DIMS_2D;
	tex.cubemap = GCM_FALSE;
	tex.remap = ((GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) |
			(GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) |
			(GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) |
			(GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT) |
			(GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) |
			(GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
			(GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) |
			(GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT));
	tex.width = (u16)w;
	tex.height = (u16)h;
	tex.depth = 1;
	tex.location = GCM_LOCATION_RSX;
	tex.pitch = (u32)w * 4;
	tex.offset = off;
	rsxLoadTexture(g_ctx, (u8)g_tex_unit, &tex);
	rsxTextureControl(g_ctx, (u8)g_tex_unit, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
	const u8 f = nearest ? GCM_TEXTURE_NEAREST : GCM_TEXTURE_LINEAR;
	rsxTextureFilter(g_ctx, (u8)g_tex_unit, 0, f, f, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	/* Godot UVs tile (flag UV 0..8): REPEAT */
	rsxTextureWrapMode(g_ctx, (u8)g_tex_unit, GCM_TEXTURE_REPEAT, GCM_TEXTURE_REPEAT, GCM_TEXTURE_REPEAT, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
}

/* ---- the frame's vertex stream ---- */

/* draw what was written since the last batch */
static void flush_batch(void)
{
	if (g_vn > g_vmark) {
		rsxDrawVertexArray(g_ctx, GCM_TYPE_TRIANGLES, g_vmark, g_vn - g_vmark);
	}
	g_vmark = g_vn;
}

/* the texture for what comes next (key: slot, or -1 white, -2 white nearest,
 * slot + 1000 nearest) */
static void use_tex(int key)
{
	if (key == g_bound) {
		return;
	}
	flush_batch();
	if (key >= 1000) {
		bind_tex(key - 1000, 1);
	} else if (key == -2) {
		bind_tex(-1, 1);
	} else {
		bind_tex(key, 0);
	}
	g_bound = key;
}

/* room for n more vertices this frame */
static int vb_room(unsigned n)
{
	return g_vbuf && g_vn + n <= VB_VERTS;
}

static GVert *vb_put(void)
{
	return &g_vbuf[g_vn++];
}

static void set_viewport(int x, int y, int w, int h)
{
	flush_batch();
	float scale[4], offset[4];
	scale[0] = (float)w * 0.5f;
	scale[1] = (float)h * -0.5f;
	scale[2] = 0.5f;
	scale[3] = 0.0f;
	offset[0] = (float)x + (float)w * 0.5f;
	offset[1] = (float)y + (float)h * 0.5f;
	offset[2] = 0.5f;
	offset[3] = 0.0f;
	rsxSetViewport(g_ctx, (u16)x, (u16)y, (u16)w, (u16)h, 0.0f, 1.0f, scale, offset);
	rsxSetScissor(g_ctx, (u16)x, (u16)y, (u16)w, (u16)h);
	g_vx = x;
	g_vy = y;
	g_vp_w = w;
	g_vp_h = h;
}

static void set_depth(int on)
{
	flush_batch();
	rsxSetDepthTestEnable(g_ctx, on ? GCM_TRUE : GCM_FALSE);
	rsxSetDepthWriteEnable(g_ctx, on ? GCM_TRUE : GCM_FALSE);
}

static void set_blend(int on)
{
	flush_batch();
	rsxSetBlendEnable(g_ctx, on ? GCM_TRUE : GCM_FALSE);
}

/* A screen rectangle in display pixels (two triangles), untextured or with
 * the whole of the bound texture. Coordinates relative to the viewport. */
static void put_rect(float x0, float y0, float x1, float y1, int r, int g, int b, int a, float u0, float v0, float u1, float v1)
{
	if (!vb_room(6)) {
		return;
	}
	const float sx = 2.0f / (float)g_vp_w, sy = 2.0f / (float)g_vp_h;
	const float X0 = x0 * sx - 1.0f, X1 = x1 * sx - 1.0f;
	const float Y0 = 1.0f - y0 * sy, Y1 = 1.0f - y1 * sy;
	const float px[6] = { X0, X1, X1, X0, X1, X0 };
	const float py[6] = { Y0, Y0, Y1, Y0, Y1, Y1 };
	const float pu[6] = { u0, u1, u1, u0, u1, u0 };
	const float pv[6] = { v0, v0, v1, v0, v1, v1 };
	for (int k = 0; k < 6; k++) {
		GVert *o = vb_put();
		o->x = px[k];
		o->y = py[k];
		o->z = 0.0f;
		o->w = 1.0f;
		o->r = (unsigned char)r;
		o->g = (unsigned char)g;
		o->b = (unsigned char)b;
		o->a = (unsigned char)a;
		o->fr = o->fg = o->fb = 0;
		o->fa = 255;
		o->u = pu[k];
		o->v = pv[k];
	}
}

static int clamp255(int v)
{
	return v < 0 ? 0 : (v > 255 ? 255 : v);
}

/* the HUD's space (hud_w x hud_h) to display pixels, whole pixels so the
 * 5x7 font's strokes stay even */
static float hx(float x)
{
	return floorf(g_hud_ox + x * g_hud_sx + 0.5f);
}

static float hy(float y)
{
	return floorf(y * g_hud_sy + 0.5f);
}

static void emit_filled(float x0, float y0, float x1, float y1, int r, int g, int b)
{
	use_tex(-2);
	put_rect(hx(x0), hy(y0), hx(x1), hy(y1), clamp255(r), clamp255(g), clamp255(b), 255, 0.0f, 0.0f, 1.0f, 1.0f);
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

/* Coplanar layers. The PC scenes lay some surfaces exactly on others (the
 * showroom's ring pattern on its floor). The GS draws them in order with
 * GEQUAL, so the later one wins its exact ties; the RSX's float depth breaks
 * those ties into stripes. So at load each triangle that lies in the plane
 * of an earlier node's triangle gets a layer number (how many nodes before
 * it share that plane), and its depth is pulled forward by that many steps. */
#define LAYER_HASH 16384
struct PlaneKey {
	int k[4];  /* normal (1/1024) and distance (mm) */
	unsigned short node; /* the latest node seen in it */
	unsigned char n;     /* how many nodes so far */
	unsigned char used;
};
static PlaneKey *g_planes;
static unsigned char *g_tri_layer;
static unsigned g_tri_layer_n;

static void build_layers(void)
{
	const unsigned ntri = g_index_count / 3;
	free(g_tri_layer);
	g_tri_layer = (unsigned char *)calloc(ntri ? ntri : 1, 1);
	g_tri_layer_n = g_tri_layer ? ntri : 0;
	if (!g_planes) {
		g_planes = (PlaneKey *)malloc(sizeof(PlaneKey) * LAYER_HASH);
	}
	if (!g_tri_layer || !g_planes) {
		g_tri_layer_n = 0;
		return;
	}
	memset(g_planes, 0, sizeof(PlaneKey) * LAYER_HASH);
	for (unsigned t = 0; t < ntri; t++) {
		const unsigned i0 = ru32(g_indices + t * 12), i1 = ru32(g_indices + t * 12 + 4), i2 = ru32(g_indices + t * 12 + 8);
		if (i0 >= g_vert_count || i1 >= g_vert_count || i2 >= g_vert_count) {
			continue;
		}
		CookVert a, b, c;
		read_vert(g_verts + i0 * 28, &a);
		read_vert(g_verts + i1 * 28, &b);
		read_vert(g_verts + i2 * 28, &c);
		const float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
		const float vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
		float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
		const float l = sqrtf(nx * nx + ny * ny + nz * nz);
		if (l < 1e-6f) {
			continue;
		}
		nx /= l;
		ny /= l;
		nz /= l;
		/* one orientation per plane, whichever way it faces */
		if (nx < -1e-4f || (nx < 1e-4f && (ny < -1e-4f || (ny < 1e-4f && nz < 0.0f)))) {
			nx = -nx;
			ny = -ny;
			nz = -nz;
		}
		const float d = nx * a.x + ny * a.y + nz * a.z;
		int k[4] = { (int)floorf(nx * 1024.0f + 0.5f), (int)floorf(ny * 1024.0f + 0.5f), (int)floorf(nz * 1024.0f + 0.5f),
			(int)floorf(d * 1000.0f + 0.5f) };
		unsigned h = (unsigned)(k[0] * 73856093) ^ (unsigned)(k[1] * 19349663) ^ (unsigned)(k[2] * 83492791) ^ (unsigned)(k[3] * 2654435761u);
		for (unsigned probe = 0; probe < LAYER_HASH; probe++) {
			PlaneKey *p = &g_planes[(h + probe) & (LAYER_HASH - 1)];
			if (!p->used) {
				p->used = 1;
				memcpy(p->k, k, sizeof(k));
				p->node = a.node;
				p->n = 1;
				break;
			}
			if (memcmp(p->k, k, sizeof(k)) == 0) {
				if (p->node != a.node) {
					p->node = a.node;
					if (p->n < 255) {
						p->n++;
					}
				}
				g_tri_layer[t] = (unsigned char)(p->n - 1);
				break;
			}
		}
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
	build_layers();
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

static void emit_glyph(float ox, float oy, int ch, int r, int g, int b, int scale)
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
				emit_filled(ox + (float)start * sc, oy + (float)row * sc, ox + (float)(start + run) * sc, oy + (float)(row + 1) * sc, r, g, b);
				start = -1;
				run = 0;
			}
		}
		if (start >= 0) {
			emit_filled(ox + (float)start * sc, oy + (float)row * sc, ox + (float)(start + run) * sc, oy + (float)(row + 1) * sc, r, g, b);
		}
	}
}

/* One HUD slot's text: '\n' starts a new line, each line aligned in the box. */
static void emit_text_block(const HudQuad *h, int r, int g, int b)
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
				emit_glyph((float)(x + c * adv), (float)y, (int)(unsigned char)t[c], r, g, b, sc);
			}
		}
		t += n;
		if (*t == '\n') {
			t++;
		}
		y += line_h;
	}
}

/* A sprite or tile: the whole texture over the rectangle (HUD space). */
static void emit_textured(float x0, float y0, float x1, float y1, int tex)
{
	use_tex(tex + 1000);
	float X0 = hx(x0), X1 = hx(x1);
	float u0 = 0.0f, u1 = 1.0f;
	if (X1 < X0) {
		/* flipped */
		const float t = X0;
		X0 = X1;
		X1 = t;
		u0 = 1.0f;
		u1 = 0.0f;
	}
	put_rect(X0, hy(y0), X1, hy(y1), 255, 255, 255, 255, u0, 0.0f, u1, 1.0f);
}

/* The HUD over the whole frame: boxes and text, sprites, tiles, the fade. */
static void emit_hud(void)
{
	set_viewport(0, 0, (int)g_dw, (int)g_dh);
	set_depth(0);
	for (int i = 0; i < 16; i++) {
		const HudQuad *h = &g_hudq[i];
		if (!h->used || h->hidden) {
			continue;
		}
		if (!h->styled) {
			/* Unstyled (legacy) rows: filled box, white text. */
			emit_filled((float)h->x, (float)h->y, (float)(h->x + h->w), (float)(h->y + h->h), h->r, h->g, h->b);
			if (h->text[0]) {
				emit_text_block(h, 255, 255, 255);
			}
			continue;
		}
		if (h->kind != 1) {
			emit_filled((float)h->x, (float)h->y, (float)(h->x + h->w), (float)(h->y + h->h), h->r, h->g, h->b);
		}
		if (h->text[0]) {
			emit_text_block(h, h->tr, h->tg, h->tb);
		}
	}
	for (int i = 0; i < g_sprt_n && i < 32; i++) {
		if (!g_sprt[i].used) {
			continue;
		}
		const int tex = g_sprt[i].tex;
		float x0 = (float)g_sprt[i].x;
		float y0 = (float)g_sprt[i].y;
		float x1 = x0 + (float)(g_sprt[i].w > 0 ? g_sprt[i].w : 16);
		float y1 = y0 + (float)(g_sprt[i].h > 0 ? g_sprt[i].h : 16);
		if (g_sprt[i].flip & 1) {
			float t = x0;
			x0 = x1;
			x1 = t;
		}
		if (tex >= 0 && tex < g_tex_count && g_tex[tex].ready) {
			emit_textured(x0, y0, x1, y1, tex);
		} else {
			emit_filled(x0 < x1 ? x0 : x1, y0, x0 < x1 ? x1 : x0, y1, 200, 80, 180);
		}
	}
	for (int i = 0; i < g_tile_n && i < 256; i++) {
		if (!g_tile[i].used) {
			continue;
		}
		const float x0 = (float)(g_tile[i].x * 16);
		const float y0 = (float)(g_tile[i].y * 16);
		const int atlas = g_tile[i].atlas;
		if (atlas >= 0 && atlas < g_tex_count && g_tex[atlas].ready) {
			emit_textured(x0, y0, x0 + 16.0f, y0 + 16.0f, atlas);
		} else {
			emit_filled(x0, y0, x0 + 16.0f, y0 + 16.0f, 40, 120, 80);
		}
	}
	if (g_fade_a > 0) {
		/* the whole display, the PS2's alpha (0..128) */
		use_tex(-2);
		set_blend(1);
		const int a = g_fade_a >= 128 ? 255 : g_fade_a * 2;
		put_rect(0.0f, 0.0f, (float)g_dw, (float)g_dh, g_fade_r, g_fade_g, g_fade_b, a, 0.0f, 0.0f, 1.0f, 1.0f);
		set_blend(0);
	}
	flush_batch();
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

/* ---- Scene triangles: node transforms, near/guard-band clipping. ---- */

struct DrawCtx {
	const Mat4 *mvp;
	unsigned drawn;
	int tiny;
	int nofog; /* this node keeps clear of fog */
	int layer; /* the triangle's coplanar layer (build_layers) */
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


/* One clipped triangle into the frame's vertices. Depth as the GS has it:
 * 1/w over the 24-bit Z (near high there, low here), less one step per
 * texture slot so a later slot (the cook's ground layers and decals) wins
 * over an earlier one on the same plane. The RSX divides by w itself, so
 * texture coordinates stay perspective-correct. */
static void emit_clipped(DrawCtx *c, const PV *a, const PV *b, const PV *d, int slot)
{
	if (!vb_room(3)) {
		return;
	}
	use_tex(c->tiny || g_tex_count == 0 ? -1 : (slot >= 0 && slot < g_tex_count ? slot : 0));
	const PV *v[3] = { a, b, d };
	const int fog = g_fog_on && !c->nofog;
	for (int k = 0; k < 3; k++) {
		float w = v[k]->w;
		float zw = w;
		if (zw < 0.25f) {
			zw = 0.25f;
		}
		if (zw > 400.0f) {
			zw = 400.0f;
		}
		float t = 0.25f / zw;
		if (t > 1.0f) {
			t = 1.0f;
		}
		/* the GS's step is exact in its integer Z; here the float divide
		 * blurs the last bits, so a slot is 32 of the 24 bits' steps, and
		 * a coplanar layer 64 */
		t -= (float)(slot * 32 + c->layer * 64) * (1.0f / 16777215.0f);
		/* NDC z: -1 at the near plane, towards 1 far off */
		const float nz = 1.0f - 2.0f * t;
		GVert *o = vb_put();
		o->x = v[k]->x;
		o->y = v[k]->y;
		o->z = nz * w;
		o->w = w;
		o->r = (unsigned char)clamp255((int)(v[k]->r * 255.0f + 0.5f));
		o->g = (unsigned char)clamp255((int)(v[k]->g * 255.0f + 0.5f));
		o->b = (unsigned char)clamp255((int)(v[k]->b * 255.0f + 0.5f));
		o->a = 255;
		o->fr = (unsigned char)clamp255(g_fog_r);
		o->fg = (unsigned char)clamp255(g_fog_g);
		o->fb = (unsigned char)clamp255(g_fog_b);
		o->fa = (unsigned char)(fog ? fog_byte(w) : 255u);
		o->u = v[k]->s;
		o->v = v[k]->t;
	}
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
		c->layer = t < g_tri_layer_n ? g_tri_layer[t] : 0;
		draw_tri(c, &v0, &v1, &v2);
		c->layer = 0;
		g_stat_tris++;
	}
}

/* (The PS2 defers the EE's chunks while VU1 draws; the PS3 draws them in turn.) */
static int vu_take(const DrawCtx *, unsigned, float, float, float, float, int, int)
{
	return 0;
}

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


/* the per-frame RSX state: target, programs, vertex arrays, tests */
static void frame_begin(void)
{
	g_vbuf = g_vb[g_fb];
	g_vn = 0;
	g_vmark = 0;
	g_bound = -3;
	set_target(g_fb);
	rsxSetColorMask(g_ctx, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A);
	rsxSetColorMaskMrt(g_ctx, 0);
	set_viewport(0, 0, (int)g_dw, (int)g_dh);
	for (int i = 0; i < 8; i++) {
		rsxSetViewportClip(g_ctx, (u8)i, (u16)g_dw, (u16)g_dh);
	}
	rsxSetZMinMaxControl(g_ctx, GCM_FALSE, GCM_TRUE, GCM_FALSE);
	rsxSetShadeModel(g_ctx, GCM_SHADE_MODEL_SMOOTH);
	rsxSetCullFaceEnable(g_ctx, GCM_FALSE);
	rsxSetDepthFunc(g_ctx, GCM_LEQUAL);
	rsxSetBlendFunc(g_ctx, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
	rsxSetBlendEquation(g_ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
	rsxSetBlendEnable(g_ctx, GCM_FALSE);
	rsxSetUserClipPlaneControl(g_ctx, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
			GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE);
	const u32 base = g_vb_off[g_fb];
	rsxBindVertexArrayAttrib(g_ctx, GCM_VERTEX_ATTRIB_POS, 0, base, sizeof(GVert), 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(g_ctx, GCM_VERTEX_ATTRIB_COLOR0, 0, base + 16, sizeof(GVert), 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(g_ctx, GCM_VERTEX_ATTRIB_COLOR1, 0, base + 20, sizeof(GVert), 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(g_ctx, GCM_VERTEX_ATTRIB_TEX0, 0, base + 24, sizeof(GVert), 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxLoadVertexProgram(g_ctx, g_vpo, g_vp_ucode);
	rsxLoadFragmentProgramLocation(g_ctx, g_fpo, g_fp_off, GCM_LOCATION_RSX);
	rsxInvalidateTextureCache(g_ctx, GCM_INVALIDATE_TEXTURE);
	rsxSetClearColor(g_ctx, 0xff000000u | ((u32)clamp255(g_clear_r) << 16) | ((u32)clamp255(g_clear_g) << 8) | (u32)clamp255(g_clear_b));
	rsxSetClearDepthStencil(g_ctx, 0xffffff00u);
	rsxClearSurface(g_ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A | GCM_CLEAR_S | GCM_CLEAR_Z);
	rsxSetDepthTestEnable(g_ctx, GCM_TRUE);
	rsxSetDepthWriteEnable(g_ctx, GCM_TRUE);
}

/* Show the frame: wait for the last flip (so its vertex buffer, the next
 * frame's, is free), queue this one's for the next VSync. */
static void frame_end(void)
{
	flush_batch();
	if (!g_first_flip) {
		while (gcmGetFlipStatus() != 0) {
			usleep(200);
		}
	}
	gcmResetFlipStatus();
	gcmSetFlip(g_ctx, (u8)g_fb);
	rsxFlushBuffer(g_ctx);
	gcmSetWaitFlip(g_ctx);
	g_fb ^= 1;
	g_first_flip = 0;
}

/* One view of the scene: the camera as it stands, NDC -1..1 onto the
 * vw x vh rectangle at (vx, vy). split: one of several views, so it keeps
 * the camera it was given. */
static void draw_view(int vx, int vy, int vw, int vh, int split, unsigned *drawn)
{
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
	set_viewport(vx, vy, vw, vh);
	set_depth(!aabb_is_tiny());

	DrawCtx c;
	memset(&c, 0, sizeof(c));
	c.mvp = &mvp;
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
					defer_or_draw(&c, ci, -1, 0);
					continue;
				}
			}
			g_stat_chunks++;
			draw_range(&c, ch->first, ch->first + ch->n);
		}
		draw_instances(&c, pl);
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
	flush_batch();
	*drawn += c.drawn;
}

void gs_draw_scene(void)
{
	if (!g_booted) {
		return;
	}
	frame_begin();
	if (g_ready) {
		const int nv = g_view_n >= 2 ? g_view_n : 1;
		unsigned drawn = 0;
		if (nv == 1) {
			draw_view(0, 0, (int)g_dw, (int)g_dh, 0, &drawn);
		} else {
			/* each view with its own camera; the scripted one comes back after */
			const float kx = g_cam_x, ky = g_cam_y, kz = g_cam_z;
			const float lx = g_look_x, lyy = g_look_y, lz = g_look_z, kf = g_fov;
			const float ax = g_attach_x, ay = g_attach_y, az = g_attach_z, eh = g_eye_h;
			g_attach_x = g_attach_y = g_attach_z = 0.0f;
			g_eye_h = 0.0f;
			for (int v = 0; v < nv; v++) {
				int vx = 0, vy = 0, vw = 0, vh = 0;
				view_rect(v, nv, (int)g_dw, (int)g_dh, &vx, &vy, &vw, &vh);
				const ViewCam *vc = &g_views[v];
				g_cam_x = vc->ex;
				g_cam_y = vc->ey;
				g_cam_z = vc->ez;
				g_look_x = vc->tx;
				g_look_y = vc->ty;
				g_look_z = vc->tz;
				g_fov = vc->fov;
				draw_view(vx, vy, vw, vh, 1, &drawn);
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
		}
#ifdef GS_STATS
		if (++g_stat_frames % 300 == 0) {
			printf("RSX stats: %u chunks, %u of %u tris tested, %u drawn, %u vertices\n", g_stat_chunks, g_stat_tris,
					g_index_count / 3, drawn, g_vn);
		}
#endif
		g_stat_tris = 0;
		g_stat_chunks = 0;
		if (drawn == 0) {
			set_viewport(0, 0, (int)g_dw, (int)g_dh);
			set_depth(0);
			emit_filled(8.0f, 8.0f, (float)g_hud_w * 0.5f - 8.0f, (float)g_hud_h - 8.0f, 200, 170, 48);
			emit_filled((float)g_hud_w * 0.5f + 8.0f, 8.0f, (float)g_hud_w - 8.0f, (float)g_hud_h - 8.0f, 40, 160, 90);
		}
	}
	emit_hud();
	frame_end();
}
