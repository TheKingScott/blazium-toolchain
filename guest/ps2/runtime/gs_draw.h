// MIT. CPU-side GS draw of cooked MESH + GTEX (ABI 1). No VU1. No Godot.
// Positions in MESH are Godot Y-up; this unit maps them to GS (X right, Y down).

#ifndef BLAZIUM_PS2_GS_DRAW_H
#define BLAZIUM_PS2_GS_DRAW_H

#include <draw.h>
#include <tamtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns 1 if MESH ABI 1 parsed. node/gtex may be NULL.
int gs_draw_init(const unsigned char *mesh, unsigned mesh_sz,
		const unsigned char *gtex, unsigned gtex_sz,
		const unsigned char *node, unsigned node_sz);

int gs_draw_layer_add(const unsigned char *mesh, unsigned mesh_sz);
void gs_draw_layer_remove(const unsigned char *mesh);

int gs_draw_ready(void);

// Digital / stick: yaw around look-at, dolly along view distance.
void gs_draw_orbit(float yaw, float dolly);

// Script-driven Y-up world spin (rotate_y). Independent of pad orbit.
void gs_draw_set_world_yaw(float yaw);

// Input.get_vector / Node3D.translate: slide the look-at target on XZ.
void gs_draw_nudge(float dx, float dz);
void gs_draw_nudge3(float dx, float dy, float dz);
void gs_draw_look_point(float *x, float *y, float *z);
void gs_draw_set_eye(float x, float y, float z);
// Camera3D: Godot Y-up YXZ euler, look along local -Z. fov_deg is the cooked FOV.
void gs_draw_set_camera(float x, float y, float z, float pitch, float yaw, float roll, float fov_deg);
void gs_draw_set_ortho(int on);
void gs_draw_set_eye_height(float h);
void gs_draw_look(float yaw, float pitch, float roll);
void gs_draw_look_delta(float dyaw, float dpitch);
void gs_draw_orbit_sph(float yaw, float pitch, float dist);
void gs_draw_attach_offset(float x, float y, float z);
void gs_draw_shake(float x, float y, float z);
void gs_draw_hud_quad(int slot, int x, int y, int w, int h, int r, int g, int b);
void gs_draw_hud_text(int slot, int x, int y, int r, int g, int b, const char *str);
void gs_draw_clear_hud(void);
void gs_draw_sprt(int slot, int x, int y, int w, int h, int tex, int flip);
void gs_draw_clear_sprt(void);
void gs_draw_tile(int slot, int x, int y, int atlas);
void gs_draw_clear_tiles(void);
void gs_draw_set_fade(int a, int r, int g, int b);
void gs_draw_set_node_ofs(int node, float x, float y, float z);
void gs_draw_set_node_rot(int node, float rx, float ry, float rz);
void gs_draw_apply_node(int node, float *x, float *y, float *z);
int gs_draw_primary_mesh_node(void);
void gs_draw_overlay(framebuffer_t *frame, zbuffer_t *z);

// Scripted scenes (GDIR): per-node world correction m = W_now * W_cooked^-1
// as 3x4 row-major (NULL = identity), hidden nodes skip their triangles.
void gs_draw_nodes_begin(void);
void gs_draw_node_matrix(int node, const float *m);
void gs_draw_node_hidden(int node, int hidden);
void gs_draw_nodes_reset(void);
// Camera from the scene: no auto-framing, no pad orbit while locked.
void gs_draw_camera_lock(int on);
void gs_draw_camera_look(float ex, float ey, float ez, float tx, float ty, float tz, float fov_deg);
void gs_draw_set_clear(int r, int g, int b);
// HUD row style: kind 1 label (text only), 2 rect, 3 button; scale is the
// 5x7 glyph multiplier; align 0 left, 1 centre, 2 right.
void gs_draw_hud_style(int slot, int kind, int scale, int align, int hidden, int tr, int tg, int tb);

// Clear + textured tris + slot-0 HUD stamp. Sends GIF packets itself.
void gs_draw_scene(framebuffer_t *frame, zbuffer_t *z);

void gs_draw_camera(float *x, float *y, float *z);
float gs_draw_world_yaw(void);
void gs_draw_fill_mvp(float out[16], int width, int height);
int gs_draw_tex_info(int i, int *vram, int *w, int *h, int *psm);

// GS origin for a framebuffer (2048 - w/2, 2048 - h/2).
void gs_draw_fb_origin(int width, int height, float *ox, float *oy);

#ifdef __cplusplus
}
#endif

#endif
