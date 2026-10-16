// MIT. Consume cooked CAM/HIT/HUD/TILE/NAV/ANIM/FMV. FMV/IPU is a named skip.

#include "sys_io.h"
#include "iso_path.h"

#include "gs_draw.h"
#include "pack_io.h"
#include "pad_io.h"
#include "sfx_io.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef BLAZIUM_PS2_COOK_ABI
#define BLAZIUM_PS2_COOK_ABI 1
#endif

static int s_ok;
static int s_cam_n;
static int s_cam_i;
static int s_cam_def;
static float s_cam_x[8];
static float s_cam_y[8];
static float s_cam_z[8];
static float s_cam_pitch[8];
static float s_cam_yaw[8];
static float s_cam_roll[8];
static float s_cam_fov[8];
static unsigned short s_cam_flags[8];
static unsigned short s_cam_node[8];
static int s_sprt_n;
static int s_sprt_loaded;
static int s_hit_loaded;
static int s_tile_loaded;
static int s_hud_loaded;
static int s_anim_loaded;
static int s_sprt_node[32];
static int s_sprt_tex[32];
static short s_sprt_x[32];
static short s_sprt_y[32];
static short s_sprt_w[32];
static short s_sprt_h[32];
static unsigned char s_sprt_flip[32];
static unsigned char s_sprt_frame[32];
static unsigned char s_hit_flags[64];
static unsigned char s_hit_kind[64];
static unsigned char s_hit_layer[64];
static unsigned char s_hit_on[64];
static unsigned short s_hit_flo[64];
static unsigned short s_hit_fhi[64];
static unsigned short s_hit_node[64];
static int s_hitstop_ms;
static int s_invuln_ms;
static int s_flip;
static float s_kb_x, s_kb_y, s_kb_z;
static float s_shake_amp;
static float s_shake_left;
static unsigned s_rng;
static int s_tw_on[16];
static float s_tw_from[16];
static float s_tw_to[16];
static float s_tw_t[16];
static float s_tw_dur[16];
static int s_tw_kind[16];
static int s_tw_last;
static int s_tm_on[16];
static float s_tm_left[16];
static int s_tm_last;
static int s_part_n;
static int s_part_loaded;
static int s_part_mode[16];
static int s_part_max[16];
static float s_part_rate[16];
static float s_part_life[16];
static float s_part_t[16];
static int s_live_n;
static float s_live_x[64];
static float s_live_y[64];
static float s_live_life[64];
static int s_live_mode[64];
static int s_path_n;
static float s_path_x[128];
static float s_path_y[128];
static float s_path_z[128];
static float s_path_off;
static int s_navm_nv;
static int s_navm_nt;
static float s_navm_x[128];
static float s_navm_y[128];
static float s_navm_z[128];
static unsigned short s_navm_a[64];
static unsigned short s_navm_b[64];
static unsigned short s_navm_c[64];
static int s_vehl_n;
static int s_vehl_mode;
static float s_vehl_thrust;
static float s_vehl_steer;
static float s_eye_h;
static int s_hit_n;
static float s_hit_mn[64][3];
static float s_hit_mx[64][3];
static unsigned char s_hit_kit[64];
static float s_px;
static float s_py;
static float s_pz;
static int s_on_floor;
static int s_on_wall;
static int s_on_ceil;
static int s_player_ok;
static int s_hud_n;
static unsigned char s_hud_kind[16];
static char s_hud_text[16][65];
static int s_hud_x[16];
static int s_hud_y[16];
static int s_hud_w[16];
static int s_hud_h[16];
static int s_hud_r[16];
static int s_hud_g[16];
static int s_hud_b[16];
static int s_hud_focus;
static int s_btn_n;
static int s_btn_slot[16];
static int s_paused;
static int s_hud_just_accept;
static char s_hud_act[65];
static float s_say_left;
static int s_say_done;
static int s_tile_n;
static short s_tile_x[256];
static short s_tile_y[256];
static unsigned char s_tile_flags[256];
static unsigned char s_tile_atlas[256];
static unsigned long long s_over_prev;
static unsigned long long s_over_now;
static int s_over_entered;
static unsigned char s_ent_kit[64];
static int s_ent_idx[64];
static int s_ent_n;
static unsigned char s_left_kit[64];
static int s_left_n;
static float s_ray_x;
static float s_ray_y;
static float s_ray_z;
static float s_fade_left;
static float s_fade_dur;
static int s_fade_r;
static int s_fade_g;
static int s_fade_b;
static int s_fade_pack;
static int s_fade_phase;
static int s_nav_n;
static int s_anim_n;
static float s_anim_len;
static int s_anim_node;
static int s_anim_keys;
static float s_ak_t[64];
static float s_ak_px[64];
static float s_ak_py[64];
static float s_ak_pz[64];
static float s_ak_rx[64];
static float s_ak_ry[64];
static float s_ak_rz[64];
static float s_anim_t;
static float s_anim_speed;
static int s_anim_playing;
static int s_fmv_n;
static int s_hp = 3;
static int s_frame;
static int s_nav_i;
static float s_nav_x;
static float s_nav_z;
static char s_fmv_err[80];

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

static FILE *open_sys(const char *host, const char *iso_bs, const char *iso)
{
	FILE *f = fopen(host, "rb");
	if (f) {
		return f;
	}
	f = iso_fopen(iso_bs, "rb");
	if (f) {
		return f;
	}
	return iso_fopen(iso, "rb");
}

static unsigned char *read_sys(const char *host, const char *iso_bs, const char *iso, unsigned *sz)
{
	FILE *f = open_sys(host, iso_bs, iso);
	if (!f) {
		return 0;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return 0;
	}
	long n = ftell(f);
	if (n < 8) {
		fclose(f);
		return 0;
	}
	rewind(f);
	unsigned char *buf = (unsigned char *)malloc((size_t)n);
	if (!buf) {
		fclose(f);
		return 0;
	}
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		fclose(f);
		return 0;
	}
	fclose(f);
	*sz = (unsigned)n;
	return buf;
}

static void apply_cam_i(int i);

static int mag4(const unsigned char *b, char a, char c, char d, char e)
{
	return b[0] == (unsigned char)a && b[1] == (unsigned char)c && b[2] == (unsigned char)d && b[3] == (unsigned char)e;
}

static void apply_cam(const unsigned char *b, unsigned sz)
{
	if (!b || sz < 8 || !mag4(b, 'C', 'A', 'M', ' ') || ru16(b + 4) != BLAZIUM_PS2_COOK_ABI) {
		return;
	}
	s_cam_n = (int)ru16(b + 6);
	if (s_cam_n > 8) {
		s_cam_n = 8;
	}
	if (s_cam_n < 1 || sz < 8 + 32) {
		return;
	}
	for (int i = 0; i < s_cam_n; i++) {
		if (8 + (unsigned)(i + 1) * 32 > sz) {
			s_cam_n = i;
			break;
		}
		const unsigned char *r = b + 8 + (unsigned)i * 32;
		s_cam_node[i] = (unsigned short)ru16(r);
		s_cam_flags[i] = (unsigned short)ru16(r + 2);
		s_cam_x[i] = rf32(r + 4);
		s_cam_y[i] = rf32(r + 8);
		s_cam_z[i] = rf32(r + 12);
		s_cam_pitch[i] = rf32(r + 16);
		s_cam_yaw[i] = rf32(r + 20);
		s_cam_roll[i] = rf32(r + 24);
		s_cam_fov[i] = rf32(r + 28);
		if (s_cam_flags[i] & 1) {
			s_cam_def = i;
		}
	}
	s_cam_i = s_cam_def;
	if (s_cam_i < 0 || s_cam_i >= s_cam_n) {
		s_cam_i = 0;
	}
	apply_cam_i(s_cam_i);
}

static void apply_hit(const unsigned char *b, unsigned sz)
{
	if (!b || sz < 8 || !mag4(b, 'H', 'I', 'T', ' ') || ru16(b + 4) != BLAZIUM_PS2_COOK_ABI) {
		return;
	}
	s_hit_n = (int)ru16(b + 6);
	if (s_hit_n > 64) {
		s_hit_n = 64;
	}
	const unsigned rec = (sz >= 8 + (unsigned)s_hit_n * 35) ? 35u : 28u;
	for (int i = 0; i < s_hit_n; i++) {
		if (8 + (unsigned)(i + 1) * rec > sz) {
			s_hit_n = i;
			break;
		}
		const unsigned char *r = b + 8 + (unsigned)i * rec;
		s_hit_node[i] = (unsigned short)ru16(r);
		s_hit_kit[i] = r[2];
		s_hit_mn[i][0] = rf32(r + 4);
		s_hit_mn[i][1] = rf32(r + 8);
		s_hit_mn[i][2] = rf32(r + 12);
		s_hit_mx[i][0] = rf32(r + 16);
		s_hit_mx[i][1] = rf32(r + 20);
		s_hit_mx[i][2] = rf32(r + 24);
		s_hit_flags[i] = 2;
		s_hit_kind[i] = 0;
		s_hit_layer[i] = 1;
		s_hit_on[i] = 1;
		s_hit_flo[i] = 0;
		s_hit_fhi[i] = 0xffff;
		if (rec >= 35) {
			s_hit_flags[i] = r[28];
			s_hit_kind[i] = r[29];
			s_hit_layer[i] = r[30];
			s_hit_flo[i] = (unsigned short)ru16(r + 31);
			s_hit_fhi[i] = (unsigned short)ru16(r + 33);
			s_hit_on[i] = (s_hit_flags[i] & 2) ? 1 : 0;
		}
	}
	s_hit_loaded = 1;
}

/* HUD kind byte: bits 0-1 kind (1 label, 2 rect, 3 button), bits 2-3 align,
 * bits 4-6 glyph scale - 1, bit 7 hidden. Scripts change hidden/text/pos. */
static int hud_base(int i)
{
	return s_hud_kind[i] & 3;
}

static int hud_hidden(int i)
{
	return (s_hud_kind[i] >> 7) & 1;
}

static void redraw_hud(void)
{
	s_btn_n = 0;
	for (int i = 0; i < s_hud_n && i < 16; i++) {
		int r = s_hud_r[i];
		int g = s_hud_g[i];
		int b = s_hud_b[i];
		const int base = hud_base(i);
		if (base == 3 && !hud_hidden(i)) {
			s_btn_slot[s_btn_n++] = i;
			if (s_hud_focus == i) {
				r = 255;
				g = 220;
				b = 40;
			}
		}
		gs_draw_hud_quad(i, s_hud_x[i], s_hud_y[i], s_hud_w[i], s_hud_h[i], r, g, b);
		const char *label = s_hud_text[i];
		const char *bar = strchr(s_hud_text[i], '|');
		if (bar) {
			label = bar + 1;
		}
		gs_draw_hud_text(i, s_hud_x[i], s_hud_y[i], r, g, b, label);
		const int tr = base == 1 ? r : 255;
		const int tg = base == 1 ? g : 255;
		const int tb = base == 1 ? b : 255;
		const int focused_text = (base == 3 && s_hud_focus == i);
		gs_draw_hud_style(i, base, ((s_hud_kind[i] >> 4) & 7) + 1, (s_hud_kind[i] >> 2) & 3, hud_hidden(i),
				focused_text ? 20 : tr, focused_text ? 20 : tg, focused_text ? 40 : tb);
	}
}

static void apply_hud(const unsigned char *b, unsigned sz)
{
	if (!b || sz < 8 || !mag4(b, 'H', 'U', 'D', ' ') || ru16(b + 4) != BLAZIUM_PS2_COOK_ABI) {
		return;
	}
	s_hud_n = (int)ru16(b + 6);
	const unsigned rec = 76;
	if (8 + (unsigned)s_hud_n * rec > sz) {
		s_hud_n = 0;
		return;
	}
	if (s_hud_n > 16) {
		s_hud_n = 16;
	}
	s_btn_n = 0;
	s_hud_focus = -1;
	for (int i = 0; i < s_hud_n; i++) {
		const unsigned char *r = b + 8 + (unsigned)i * rec;
		s_hud_x[i] = (int)(short)ru16(r);
		s_hud_y[i] = (int)(short)ru16(r + 2);
		s_hud_w[i] = (int)(short)ru16(r + 4);
		s_hud_h[i] = (int)(short)ru16(r + 6);
		s_hud_kind[i] = r[8];
		s_hud_r[i] = r[9];
		s_hud_g[i] = r[10];
		s_hud_b[i] = r[11];
		memcpy(s_hud_text[i], r + 12, 64);
		s_hud_text[i][64] = 0;
		if ((s_hud_kind[i] & 3) == 3 && !(s_hud_kind[i] & 0x80) && s_hud_focus < 0) {
			s_hud_focus = i;
		}
	}
	redraw_hud();
}

static void apply_tile(const unsigned char *b, unsigned sz)
{
	if (!b || sz < 10 || !mag4(b, 'T', 'I', 'L', 'E') || ru16(b + 4) != BLAZIUM_PS2_COOK_ABI) {
		return;
	}
	s_tile_n = (int)ru16(b + 6);
	if (s_tile_n > 256) {
		s_tile_n = 256;
	}
	const unsigned rec = 6;
	gs_draw_clear_tiles();
	for (int i = 0; i < s_tile_n; i++) {
		if (10 + (unsigned)(i + 1) * rec > sz) {
			s_tile_n = i;
			break;
		}
		const unsigned char *r = b + 10 + (unsigned)i * rec;
		s_tile_x[i] = (short)ru16(r);
		s_tile_y[i] = (short)ru16(r + 2);
		s_tile_flags[i] = r[4];
		s_tile_atlas[i] = r[5];
		if (s_tile_flags[i] & 1) {
			gs_draw_tile(i, s_tile_x[i], s_tile_y[i], s_tile_atlas[i]);
		}
	}
}

static void apply_nav(const unsigned char *b, unsigned sz)
{
	if (!b || sz < 10 || !mag4(b, 'N', 'A', 'V', ' ') || ru16(b + 4) != BLAZIUM_PS2_COOK_ABI) {
		return;
	}
	s_nav_n = (int)ru16(b + 6);
	if (s_nav_n > 0 && sz >= 10 + 16) {
		s_nav_x = rf32(b + 10);
		s_nav_z = rf32(b + 18);
	}
}

static void apply_anim_pose(void)
{
	if (s_anim_keys < 1) {
		return;
	}
	float t = s_anim_t;
	if (t < 0.0f) {
		t = 0.0f;
	}
	if (s_anim_len > 0.0f && t > s_anim_len) {
		t = s_anim_len;
	}
	int a = 0;
	int b = 0;
	for (int i = 0; i < s_anim_keys; i++) {
		if (s_ak_t[i] <= t) {
			a = i;
		}
		if (s_ak_t[i] >= t) {
			b = i;
			break;
		}
		b = i;
	}
	float u = 0.0f;
	const float dt = s_ak_t[b] - s_ak_t[a];
	if (dt > 0.0001f) {
		u = (t - s_ak_t[a]) / dt;
	}
	const float px = s_ak_px[a] + (s_ak_px[b] - s_ak_px[a]) * u;
	const float py = s_ak_py[a] + (s_ak_py[b] - s_ak_py[a]) * u;
	const float pz = s_ak_pz[a] + (s_ak_pz[b] - s_ak_pz[a]) * u;
	const float rx = s_ak_rx[a] + (s_ak_rx[b] - s_ak_rx[a]) * u;
	const float ry = s_ak_ry[a] + (s_ak_ry[b] - s_ak_ry[a]) * u;
	const float rz = s_ak_rz[a] + (s_ak_rz[b] - s_ak_rz[a]) * u;
	gs_draw_set_node_ofs(s_anim_node, px, py, pz);
	gs_draw_set_node_rot(s_anim_node, rx, ry, rz);
}

static void apply_anim(const unsigned char *b, unsigned sz)
{
	if (!b || sz < 8 || !mag4(b, 'A', 'N', 'I', 'M') || ru16(b + 4) != BLAZIUM_PS2_COOK_ABI) {
		return;
	}
	s_anim_n = (int)ru16(b + 6);
	s_anim_keys = 0;
	s_anim_node = 0;
	s_anim_len = 1.0f;
	s_anim_t = 0.0f;
	s_anim_speed = 1.0f;
	s_anim_playing = 1;
	s_anim_loaded = 1;
	if (s_anim_n < 1 || sz < 8 + 36) {
		return;
	}
	const unsigned char *clip = b + 8;
	s_anim_len = rf32(clip + 32);
	s_anim_keys = (int)ru16(clip + 36);
	s_anim_node = (int)(short)ru16(clip + 38);
	if (s_anim_keys > 64) {
		s_anim_keys = 64;
	}
	unsigned off = 40;
	for (int k = 0; k < s_anim_keys; k++) {
		if (off + 28 > sz) {
			s_anim_keys = k;
			break;
		}
		s_ak_t[k] = rf32(clip + off);
		s_ak_px[k] = rf32(clip + off + 4);
		s_ak_py[k] = rf32(clip + off + 8);
		s_ak_pz[k] = rf32(clip + off + 12);
		s_ak_rx[k] = rf32(clip + off + 16);
		s_ak_ry[k] = rf32(clip + off + 20);
		s_ak_rz[k] = rf32(clip + off + 24);
		off += 28;
	}
	apply_anim_pose();
}

static void apply_sprt(const unsigned char *b, unsigned sz)
{
	s_sprt_n = 0;
	s_sprt_loaded = 0;
	gs_draw_clear_sprt();
	if (!b || sz < 8 || !mag4(b, 'S', 'P', 'R', 'T') || ru16(b + 4) != BLAZIUM_PS2_COOK_ABI) {
		return;
	}
	s_sprt_n = (int)ru16(b + 6);
	if (s_sprt_n > 32) {
		s_sprt_n = 32;
	}
	const unsigned rec = 16;
	for (int i = 0; i < s_sprt_n; i++) {
		if (8 + (unsigned)(i + 1) * rec > sz) {
			s_sprt_n = i;
			break;
		}
		const unsigned char *r = b + 8 + (unsigned)i * rec;
		s_sprt_node[i] = (int)(short)ru16(r);
		s_sprt_tex[i] = (int)ru16(r + 2);
		s_sprt_x[i] = (short)ru16(r + 4);
		s_sprt_y[i] = (short)ru16(r + 6);
		s_sprt_w[i] = (short)ru16(r + 8);
		s_sprt_h[i] = (short)ru16(r + 10);
		s_sprt_flip[i] = r[12];
		s_sprt_frame[i] = r[13];
		gs_draw_sprt(i, s_sprt_x[i], s_sprt_y[i], s_sprt_w[i], s_sprt_h[i], s_sprt_tex[i], s_sprt_flip[i]);
	}
	s_sprt_loaded = s_sprt_n > 0;
}

static void apply_part(const unsigned char *b, unsigned sz)
{
	s_part_n = 0;
	s_part_loaded = 0;
	if (!b || sz < 8 || !mag4(b, 'P', 'A', 'R', 'T') || ru16(b + 4) != BLAZIUM_PS2_COOK_ABI) {
		return;
	}
	s_part_n = (int)ru16(b + 6);
	if (s_part_n > 16) {
		s_part_n = 16;
	}
	const unsigned rec = 22;
	for (int i = 0; i < s_part_n; i++) {
		if (8 + (unsigned)(i + 1) * rec > sz) {
			s_part_n = i;
			break;
		}
		const unsigned char *r = b + 8 + (unsigned)i * rec;
		s_part_mode[i] = r[2];
		s_part_max[i] = (int)ru16(r + 4);
		s_part_rate[i] = rf32(r + 6);
		s_part_life[i] = rf32(r + 10);
		s_part_t[i] = 0.0f;
		if (s_part_max[i] < 1) {
			s_part_max[i] = 1;
		}
		if (s_part_max[i] > 64) {
			s_part_max[i] = 64;
		}
	}
	s_part_loaded = s_part_n > 0;
}

static void apply_path(const unsigned char *b, unsigned sz)
{
	s_path_n = 0;
	s_path_off = 0.0f;
	if (!b || sz < 8 || !mag4(b, 'P', 'A', 'T', 'H') || ru16(b + 4) != BLAZIUM_PS2_COOK_ABI) {
		return;
	}
	s_path_n = (int)ru16(b + 6);
	if (s_path_n > 128) {
		s_path_n = 128;
	}
	for (int i = 0; i < s_path_n; i++) {
		if (8 + (unsigned)(i + 1) * 16 > sz) {
			s_path_n = i;
			break;
		}
		const unsigned char *r = b + 8 + (unsigned)i * 16;
		s_path_x[i] = rf32(r);
		s_path_y[i] = rf32(r + 4);
		s_path_z[i] = rf32(r + 8);
	}
}

static void apply_navm(const unsigned char *b, unsigned sz)
{
	s_navm_nv = 0;
	s_navm_nt = 0;
	if (!b || sz < 10 || !mag4(b, 'N', 'A', 'V', 'M') || ru16(b + 4) != BLAZIUM_PS2_COOK_ABI) {
		return;
	}
	s_navm_nv = (int)ru16(b + 6);
	s_navm_nt = (int)ru16(b + 8);
	if (s_navm_nv > 128) {
		s_navm_nv = 128;
	}
	if (s_navm_nt > 64) {
		s_navm_nt = 64;
	}
	unsigned off = 10;
	for (int i = 0; i < s_navm_nv; i++) {
		if (off + 12 > sz) {
			s_navm_nv = i;
			break;
		}
		s_navm_x[i] = rf32(b + off);
		s_navm_y[i] = rf32(b + off + 4);
		s_navm_z[i] = rf32(b + off + 8);
		off += 12;
	}
	for (int i = 0; i < s_navm_nt; i++) {
		if (off + 6 > sz) {
			s_navm_nt = i;
			break;
		}
		s_navm_a[i] = (unsigned short)ru16(b + off);
		s_navm_b[i] = (unsigned short)ru16(b + off + 2);
		s_navm_c[i] = (unsigned short)ru16(b + off + 4);
		off += 6;
	}
}

static void apply_vehl(const unsigned char *b, unsigned sz)
{
	s_vehl_n = 0;
	if (!b || sz < 8 || !mag4(b, 'V', 'E', 'H', 'L') || ru16(b + 4) != BLAZIUM_PS2_COOK_ABI) {
		return;
	}
	s_vehl_n = (int)ru16(b + 6);
	if (s_vehl_n < 1 || sz < 8 + 44) {
		return;
	}
	s_vehl_mode = b[10];
	s_vehl_thrust = rf32(b + 16);
	s_vehl_steer = 0.0f;
}

/* Pack 0 CAM00 HUD00 NAV00 plus CAM%02d HIT%02d HUD%02d TILE%02d NAV%02d ANIM%02d SPRT%02d PART PATH NAVM VEHL. */
static void load_sys_named(int pack, const char *stem,
		void (*apply)(const unsigned char *, unsigned))
{
	char host[40];
	char iso_bs[48];
	char iso[48];
	sprintf(host, "host:%s%02d.bin", stem, pack);
	sprintf(iso_bs, "cdrom0:\\%s%02d.BIN;1", stem, pack);
	sprintf(iso, "cdrom0:%s%02d.BIN;1", stem, pack);
	unsigned sz = 0;
	unsigned char *buf = read_sys(host, iso_bs, iso, &sz);
	if (buf) {
		apply(buf, sz);
		free(buf);
		s_ok = 1;
	}
}

int sys_io_load_pack(int pack)
{
	if (pack < 0) {
		pack = 0;
	}
	s_cam_n = s_hit_n = s_hud_n = s_tile_n = s_nav_n = s_anim_n = s_sprt_n = 0;
	s_sprt_loaded = s_hit_loaded = 0;
	s_anim_keys = 0;
	s_btn_n = 0;
	s_hud_focus = -1;
	load_sys_named(pack, "CAM", apply_cam);
	load_sys_named(pack, "HIT", apply_hit);
	load_sys_named(pack, "HUD", apply_hud);
	load_sys_named(pack, "TILE", apply_tile);
	load_sys_named(pack, "NAV", apply_nav);
	load_sys_named(pack, "ANIM", apply_anim);
	load_sys_named(pack, "SPRT", apply_sprt);
	load_sys_named(pack, "PART", apply_part);
	load_sys_named(pack, "PATH", apply_path);
	load_sys_named(pack, "NAVM", apply_navm);
	load_sys_named(pack, "VEHL", apply_vehl);
	s_tile_loaded = s_tile_n > 0;
	s_hud_loaded = s_hud_n > 0;
	s_anim_loaded = s_anim_n > 0;
	return s_ok;
}

int sys_io_init(void)
{
	s_ok = 0;
	s_cam_n = s_cam_i = s_hit_n = s_hud_n = s_tile_n = s_nav_n = s_anim_n = s_fmv_n = 0;
	s_shake_amp = 0.0f;
	s_shake_left = 0.0f;
	s_rng = 1;
	s_tw_last = -1;
	s_tm_last = -1;
	for (int i = 0; i < 16; i++) {
		s_tw_on[i] = 0;
		s_tm_on[i] = 0;
		s_tm_left[i] = 0.0f;
	}
	s_hp = 3;
	s_frame = 0;
	s_nav_i = 0;
	s_hit_n = 0;
	s_tile_n = 0;
	s_on_floor = s_on_wall = s_on_ceil = 0;
	s_player_ok = 0;
	s_px = s_py = s_pz = 0.0f;
	s_over_prev = s_over_now = 0;
	s_over_entered = 0;
	s_ent_n = 0;
	s_left_n = 0;
	s_paused = 0;
	s_hud_just_accept = 0;
	s_hud_act[0] = 0;
	s_ray_x = s_ray_y = s_ray_z = 0.0f;
	s_fade_left = s_fade_dur = 0.0f;
	s_fade_r = s_fade_g = s_fade_b = 0;
	s_fade_pack = -1;
	s_fade_phase = 0;
	s_say_left = 0.0f;
	s_say_done = 1;
	s_anim_speed = 1.0f;
	gs_draw_set_fade(0, 0, 0, 0);
	memcpy(s_fmv_err, "FMV/IPU not shipped on PS2 ABI 1 (no license-clean decoder)", 59);
	s_fmv_err[59] = 0;
	sys_io_load_pack(0);
	gs_draw_look_point(&s_px, &s_py, &s_pz);
	s_player_ok = 1;
	unsigned sz = 0;
	unsigned char *fmv = read_sys("host:FMV00.bin", "cdrom0:\\FMV00.BIN;1", "cdrom0:FMV00.BIN;1", &sz);
	if (fmv) {
		if (mag4(fmv, 'F', 'M', 'V', ' ') && ru16(fmv + 4) == BLAZIUM_PS2_COOK_ABI) {
			s_fmv_n = (int)ru16(fmv + 6);
		}
		free(fmv);
		s_ok = 1;
	}
	return s_ok;
}

int sys_io_loaded(void) { return s_ok; }
int sys_io_cam_count(void) { return s_cam_n; }
int sys_io_hit_count(void) { return s_hit_n; }
int sys_io_hud_count(void) { return s_hud_n; }
int sys_io_tile_count(void) { return s_tile_n; }
int sys_io_nav_count(void) { return s_nav_n; }
int sys_io_anim_count(void) { return s_anim_n; }
int sys_io_fmv_count(void) { return s_fmv_n; }
const char *sys_io_fmv_error(void) { return s_fmv_err; }

void sys_io_say(const char *line)
{
	if (s_hud_n < 1) {
		s_hud_n = 1;
		s_hud_x[0] = 8;
		s_hud_y[0] = 8;
		s_hud_w[0] = 96;
		s_hud_h[0] = 16;
		s_hud_r[0] = 255;
		s_hud_g[0] = 255;
		s_hud_b[0] = 80;
		s_hud_kind[0] = 1;
	}
	if (line && line[0]) {
		unsigned i = 0;
		while (line[i] && i < 64) {
			s_hud_text[0][i] = line[i];
			i++;
		}
		s_hud_text[0][i] = 0;
	} else if (s_hud_text[0][0] == 0) {
		memcpy(s_hud_text[0], "PS2", 4);
	}
	s_say_left = 0.35f;
	s_say_done = 0;
	redraw_hud();
}

int sys_io_say_done(void)
{
	return s_say_done;
}

void sys_io_hud_set_hidden(int slot, int hidden)
{
	if (slot < 0 || slot >= s_hud_n) {
		return;
	}
	const unsigned char k = hidden ? (unsigned char)(s_hud_kind[slot] | 0x80) : (unsigned char)(s_hud_kind[slot] & 0x7f);
	if (k == s_hud_kind[slot]) {
		return;
	}
	s_hud_kind[slot] = k;
	if (hidden && s_hud_focus == slot) {
		s_hud_focus = -1;
		for (int i = 0; i < s_hud_n; i++) {
			if ((s_hud_kind[i] & 3) == 3 && !(s_hud_kind[i] & 0x80)) {
				s_hud_focus = i;
				break;
			}
		}
	}
	redraw_hud();
}

int sys_io_hud_hidden(int slot)
{
	if (slot < 0 || slot >= s_hud_n) {
		return 1;
	}
	return (s_hud_kind[slot] >> 7) & 1;
}

void sys_io_hud_set_pos(int slot, int x, int y)
{
	if (slot < 0 || slot >= s_hud_n) {
		return;
	}
	s_hud_x[slot] = x;
	s_hud_y[slot] = y;
	redraw_hud();
}

void sys_io_hud_get_pos(int slot, int *x, int *y)
{
	if (slot < 0 || slot >= s_hud_n) {
		*x = *y = 0;
		return;
	}
	*x = s_hud_x[slot];
	*y = s_hud_y[slot];
}

void sys_io_hud_set_color(int slot, int r, int g, int b)
{
	if (slot < 0 || slot >= s_hud_n) {
		return;
	}
	s_hud_r[slot] = r;
	s_hud_g[slot] = g;
	s_hud_b[slot] = b;
	redraw_hud();
}

const char *sys_io_hud_text(int slot)
{
	if (slot < 0 || slot >= s_hud_n) {
		return "";
	}
	return s_hud_text[slot];
}

int sys_io_hud_focus(void)
{
	return s_hud_focus;
}

void sys_io_hud_set_focus(int slot)
{
	if (slot < 0 || slot >= s_hud_n || (s_hud_kind[slot] & 3) != 3) {
		return;
	}
	s_hud_focus = slot;
	redraw_hud();
}

void sys_io_set_hud_text(int slot, const char *text)
{
	if (slot < 0 || slot >= 16) {
		slot = 0;
	}
	if (slot >= s_hud_n) {
		s_hud_n = slot + 1;
	}
	unsigned i = 0;
	if (text) {
		while (text[i] && i < 64) {
			s_hud_text[slot][i] = text[i];
			i++;
		}
	}
	s_hud_text[slot][i] = 0;
	redraw_hud();
}

void sys_io_nav_follow(float speed, float delta)
{
	if (s_navm_nt >= 1 && s_navm_nv >= 1) {
		int i = s_nav_i;
		if (i < 0 || i >= s_navm_nv) {
			i = 0;
		}
		sys_io_follow_node(s_navm_x[i], s_navm_z[i], speed, delta);
		return;
	}
	if (s_nav_n < 2) {
		return;
	}
	gs_draw_nudge(speed * delta, 0.0f);
	(void)s_nav_x;
	(void)s_nav_z;
}

int sys_io_nav_next(int from, int to)
{
	(void)to;
	int n = from + 1;
	if (n >= s_nav_n) {
		n = 0;
	}
	s_nav_i = n;
	return n;
}

int sys_io_set_hp(int hp)
{
	s_hp = hp;
	return s_hp;
}

int sys_io_get_hp(void)
{
	return s_hp;
}

void sys_io_set_frame(int i)
{
	s_frame = i;
	if (s_anim_keys > 0) {
		int k = i;
		if (k < 0) {
			k = 0;
		}
		if (k >= s_anim_keys) {
			k = s_anim_keys - 1;
		}
		s_anim_t = s_ak_t[k];
		apply_anim_pose();
	}
}

void sys_io_seek_anim(float sec)
{
	s_anim_t = sec;
	apply_anim_pose();
}

void sys_io_set_anim_speed(float s)
{
	s_anim_speed = s;
}

void sys_io_play_anim(int name_or_index)
{
	(void)name_or_index;
	s_anim_playing = 1;
	s_anim_t = 0.0f;
	apply_anim_pose();
}

void sys_io_stop_anim(void)
{
	s_anim_playing = 0;
}

int sys_io_play_fmv(void)
{
	return 0;
}

void sys_io_tick(float delta)
{
	if (pad_io_just_pressed(12)) {
		s_paused = !s_paused;
	}
	if (s_hitstop_ms > 0) {
		s_hitstop_ms -= (int)(delta * 1000.0f);
		if (s_hitstop_ms < 0) {
			s_hitstop_ms = 0;
		}
	}
	if (s_invuln_ms > 0) {
		s_invuln_ms -= (int)(delta * 1000.0f);
		if (s_invuln_ms < 0) {
			s_invuln_ms = 0;
		}
	}
	if (s_hitstop_ms > 0) {
		return;
	}
	if (s_anim_n > 0 && s_anim_playing) {
		s_frame++;
		s_anim_t += delta * s_anim_speed;
		if (s_anim_len > 0.0f && s_anim_t > s_anim_len) {
			s_anim_t = s_anim_len;
		}
		apply_anim_pose();
	}
	if (s_say_left > 0.0f) {
		s_say_left -= delta;
		if (s_say_left <= 0.0f) {
			s_say_left = 0.0f;
			s_say_done = 1;
		}
	}
	if (s_btn_n > 0) {
		int idx = 0;
		for (int i = 0; i < s_btn_n; i++) {
			if (s_btn_slot[i] == s_hud_focus) {
				idx = i;
				break;
			}
		}
		if (pad_io_just_pressed(0) || pad_io_just_pressed(2)) {
			idx--;
			if (idx < 0) {
				idx = s_btn_n - 1;
			}
			s_hud_focus = s_btn_slot[idx];
			redraw_hud();
		}
		if (pad_io_just_pressed(1) || pad_io_just_pressed(3)) {
			idx++;
			if (idx >= s_btn_n) {
				idx = 0;
			}
			s_hud_focus = s_btn_slot[idx];
			redraw_hud();
		}
		if (pad_io_just_pressed(4)) {
			sfx_io_play();
			s_hud_just_accept = 1;
			s_hud_act[0] = 0;
			if (s_hud_focus >= 0 && s_hud_focus < 16) {
				const char *src = s_hud_text[s_hud_focus];
				int n = 0;
				while (src[n] && src[n] != '|' && n < 64) {
					s_hud_act[n] = src[n];
					n++;
				}
				s_hud_act[n] = 0;
				if (!s_hud_act[0]) {
					strncpy(s_hud_act, src, 64);
					s_hud_act[64] = 0;
				}
			}
		}
	}
	if (s_paused) {
		return;
	}
	if (s_shake_left <= 0.0f) {
		gs_draw_shake(0.0f, 0.0f, 0.0f);
	} else {
		s_shake_left -= delta * 1000.0f;
		if (s_shake_left <= 0.0f) {
			s_shake_left = 0.0f;
			gs_draw_shake(0.0f, 0.0f, 0.0f);
		} else {
			s_rng = s_rng * 1103515245u + 12345u;
			const float nx = ((float)((s_rng >> 16) & 0x7fff) / 16384.0f) - 1.0f;
			s_rng = s_rng * 1103515245u + 12345u;
			const float ny = ((float)((s_rng >> 16) & 0x7fff) / 16384.0f) - 1.0f;
			s_rng = s_rng * 1103515245u + 12345u;
			const float nz = ((float)((s_rng >> 16) & 0x7fff) / 16384.0f) - 1.0f;
			gs_draw_shake(nx * s_shake_amp, ny * s_shake_amp, nz * s_shake_amp);
		}
	}
	for (int i = 0; i < 16; i++) {
		if (s_tw_on[i]) {
			s_tw_t[i] += delta;
			float u = (s_tw_dur[i] <= 0.0f) ? 1.0f : (s_tw_t[i] / s_tw_dur[i]);
			if (u >= 1.0f) {
				u = 1.0f;
				s_tw_on[i] = 0;
			}
			const float v = s_tw_from[i] + (s_tw_to[i] - s_tw_from[i]) * u;
			if (s_tw_kind[i] == 1) {
				sfx_io_music_set_vol(v);
			}
		}
		if (s_tm_on[i]) {
			s_tm_left[i] -= delta;
			if (s_tm_left[i] <= 0.0f) {
				s_tm_left[i] = 0.0f;
				s_tm_on[i] = 0;
			}
		}
	}
	if (s_fade_phase == 1) {
		s_fade_left -= delta;
		if (s_fade_left <= 0.0f || s_fade_dur <= 0.0f) {
			s_fade_left = 0.0f;
			gs_draw_set_fade(128, s_fade_r, s_fade_g, s_fade_b);
			if (s_fade_pack >= 0) {
				pack_io_swap(s_fade_pack);
				s_fade_pack = -1;
			}
			s_fade_phase = 2;
			s_fade_left = s_fade_dur > 0.0f ? s_fade_dur : 0.01f;
		} else {
			const float u = 1.0f - (s_fade_left / s_fade_dur);
			gs_draw_set_fade((int)(u * 128.0f), s_fade_r, s_fade_g, s_fade_b);
		}
	} else if (s_fade_phase == 2 || s_fade_left > 0.0f) {
		s_fade_left -= delta;
		if (s_fade_left <= 0.0f || s_fade_dur <= 0.0f) {
			s_fade_left = 0.0f;
			s_fade_phase = 0;
			gs_draw_set_fade(0, s_fade_r, s_fade_g, s_fade_b);
		} else {
			const float u = s_fade_left / s_fade_dur;
			gs_draw_set_fade((int)(u * 128.0f), s_fade_r, s_fade_g, s_fade_b);
		}
	}
	for (int e = 0; e < s_part_n; e++) {
		s_part_t[e] += delta * s_part_rate[e];
		while (s_part_t[e] >= 1.0f && s_live_n < 64) {
			s_part_t[e] -= 1.0f;
			s_live_x[s_live_n] = s_px;
			s_live_y[s_live_n] = s_py + 0.5f;
			s_live_life[s_live_n] = s_part_life[e] > 0.05f ? s_part_life[e] : 0.5f;
			s_live_mode[s_live_n] = s_part_mode[e];
			s_live_n++;
		}
	}
	{
		int w = 0;
		for (int i = 0; i < s_live_n; i++) {
			s_live_life[i] -= delta;
			if (s_live_life[i] <= 0.0f) {
				continue;
			}
			s_live_y[i] += delta * 0.4f;
			if (s_live_mode[i] == 2 && i > 0) {
				s_live_x[i] = s_live_x[i - 1] + 2.0f;
			}
			const int slot = 8 + (w % 8);
			int szq = (s_live_mode[i] == 3) ? 2 : ((s_live_mode[i] == 2) ? 12 : 6);
			gs_draw_hud_quad(slot, 40 + (int)s_live_x[i], 40 + (int)s_live_y[i], szq, szq, 220, 180, 80);
			s_live_x[w] = s_live_x[i];
			s_live_y[w] = s_live_y[i];
			s_live_life[w] = s_live_life[i];
			s_live_mode[w] = s_live_mode[i];
			w++;
		}
		s_live_n = w;
	}
	if (pad_io_just_pressed(8)) {
		sys_io_next_cam();
	}
	if (pad_io_just_pressed(9)) {
		sys_io_prev_cam();
	}
}

void sys_io_look(float yaw, float pitch, float roll)
{
	gs_draw_look(yaw, pitch, roll);
}

void sys_io_look_stick(float delta)
{
	float sx = 0.0f;
	float sy = 0.0f;
	pad_io_stick(1, &sx, &sy);
	if (sx == 0.0f && sy == 0.0f) {
		pad_io_stick(0, &sx, &sy);
		sx *= 0.35f;
		sy *= 0.35f;
	}
	gs_draw_look_delta(sx * 1.6f * delta, -sy * 1.2f * delta);
}

void sys_io_orbit(float yaw, float pitch, float dist)
{
	gs_draw_orbit_sph(yaw, pitch, dist);
}

void sys_io_attach(float ox, float oy, float oz)
{
	gs_draw_attach_offset(ox, oy, oz);
}

void sys_io_shake(float amp, float ms)
{
	if (amp < 0.0f) {
		amp = 0.0f;
	}
	s_shake_amp = amp;
	s_shake_left = ms;
	if (ms <= 0.0f || amp <= 0.0f) {
		s_shake_left = 0.0f;
		gs_draw_shake(0.0f, 0.0f, 0.0f);
	}
}

static void apply_cam_i(int i)
{
	if (i < 0 || i >= s_cam_n) {
		return;
	}
	s_cam_i = i;
	gs_draw_set_ortho((s_cam_flags[i] & 2) ? 1 : 0);
	if (s_cam_flags[i] & 2) {
		gs_draw_set_camera(s_cam_x[i], s_cam_y[i] + 12.0f, s_cam_z[i], -1.2f, 0.0f, 0.0f, s_cam_fov[i]);
	} else {
		gs_draw_set_camera(s_cam_x[i], s_cam_y[i], s_cam_z[i], s_cam_pitch[i], s_cam_yaw[i], s_cam_roll[i], s_cam_fov[i]);
	}
}

void sys_io_next_cam(void)
{
	if (s_cam_n < 1) {
		return;
	}
	apply_cam_i((s_cam_i + 1) % s_cam_n);
}

void sys_io_prev_cam(void)
{
	if (s_cam_n < 1) {
		return;
	}
	int i = s_cam_i - 1;
	if (i < 0) {
		i = s_cam_n - 1;
	}
	apply_cam_i(i);
}

int sys_io_cam_index(void)
{
	return s_cam_i;
}

int sys_io_set_cam(int i)
{
	if (i < 0 || i >= s_cam_n) {
		return 0;
	}
	apply_cam_i(i);
	return 1;
}

static int node_named(const char *name)
{
	const unsigned char *blob = 0;
	unsigned sz = 0;
	pack_io_current_node(&blob, &sz);
	if (!name || !blob || sz < 8) {
		return -1;
	}
	const unsigned count = ru16(blob + 6);
	const unsigned rec = (8 + count * 138u <= sz) ? 138u : 74u;
	for (unsigned i = 0; i < count; i++) {
		if (8 + (i + 1) * rec > sz) {
			break;
		}
		char n[33];
		memcpy(n, blob + 8 + i * rec + 42, 32);
		n[32] = 0;
		if (strcmp(n, name) == 0) {
			return (int)i;
		}
	}
	return -1;
}

int sys_io_set_cam_name(const char *name)
{
	const int nid = node_named(name);
	if (nid < 0) {
		return 0;
	}
	for (int i = 0; i < s_cam_n; i++) {
		if ((int)s_cam_node[i] == nid) {
			apply_cam_i(i);
			return 1;
		}
	}
	return 0;
}

int sys_io_set_default_cam(int i)
{
	if (i < 0 || i >= s_cam_n) {
		return 0;
	}
	s_cam_def = i;
	apply_cam_i(i);
	return 1;
}

int sys_io_default_cam(void)
{
	return s_cam_def;
}

int sys_io_make_current(const char *name)
{
	return sys_io_set_cam_name(name);
}

int sys_io_tween_start(float from, float to, float sec, int kind)
{
	int slot = -1;
	for (int i = 0; i < 16; i++) {
		if (!s_tw_on[i]) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		return -1;
	}
	s_tw_on[slot] = 1;
	s_tw_from[slot] = from;
	s_tw_to[slot] = to;
	s_tw_t[slot] = 0.0f;
	s_tw_dur[slot] = sec < 0.0f ? 0.0f : sec;
	s_tw_kind[slot] = kind;
	s_tw_last = slot;
	if (s_tw_dur[slot] <= 0.0f) {
		s_tw_on[slot] = 0;
		if (kind == 1) {
			sfx_io_music_set_vol(to);
		}
	}
	return slot;
}

void sys_io_kill_tweens(void)
{
	for (int i = 0; i < 16; i++) {
		s_tw_on[i] = 0;
	}
}

int sys_io_tween_count(void)
{
	int n = 0;
	for (int i = 0; i < 16; i++) {
		if (s_tw_on[i]) {
			n++;
		}
	}
	return n;
}

int sys_io_tween_done(int id)
{
	if (id < 0) {
		id = s_tw_last;
	}
	if (id < 0 || id >= 16) {
		return 1;
	}
	return s_tw_on[id] ? 0 : 1;
}

int sys_io_timer_start(float sec)
{
	int slot = -1;
	for (int i = 0; i < 16; i++) {
		if (!s_tm_on[i]) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		return -1;
	}
	s_tm_on[slot] = 1;
	s_tm_left[slot] = sec < 0.0f ? 0.0f : sec;
	s_tm_last = slot;
	if (s_tm_left[slot] <= 0.0f) {
		s_tm_on[slot] = 0;
	}
	return slot;
}

int sys_io_timer_done(int id)
{
	if (id < 0) {
		id = s_tm_last;
	}
	if (id < 0 || id >= 16) {
		return 1;
	}
	return s_tm_on[id] ? 0 : 1;
}

static int hit_active(int i)
{
	if (!s_hit_on[i]) {
		return 0;
	}
	const unsigned fr = (unsigned)(s_frame < 0 ? 0 : s_frame);
	if (fr < s_hit_flo[i] || fr > s_hit_fhi[i]) {
		return 0;
	}
	return 1;
}

static int hit_solid(int i)
{
	if (!hit_active(i)) {
		return 0;
	}
	if (s_hit_kind[i] == 1 || s_hit_kind[i] == 2 || s_hit_kind[i] == 3) {
		return 0;
	}
	const unsigned char k = s_hit_kit[i];
	return k != 1 && k != 4 && k != 6 && k != 7 && k != 10;
}

static int aabb_overlap(float px, float py, float pz, int i)
{
	const float hx = 0.35f;
	const float hy = 0.85f;
	const float hz = 0.35f;
	const float mn0 = px - hx;
	const float mx0 = px + hx;
	const float mn1 = py;
	const float mx1 = py + hy;
	const float mn2 = pz - hz;
	const float mx2 = pz + hz;
	if (mx0 < s_hit_mn[i][0] || mn0 > s_hit_mx[i][0]) {
		return 0;
	}
	if (mx1 < s_hit_mn[i][1] || mn1 > s_hit_mx[i][1]) {
		return 0;
	}
	if (!(s_hit_flags[i] & 1)) {
		if (mx2 < s_hit_mn[i][2] || mn2 > s_hit_mx[i][2]) {
			return 0;
		}
	}
	return 1;
}

static int layer_ok(int i, int mask)
{
	if (mask == 0 || mask == 255) {
		return 1;
	}
	return (mask & (int)s_hit_layer[i]) != 0;
}

static int hits_at(float px, float py, float pz, int mask)
{
	for (int i = 0; i < s_hit_n; i++) {
		if (hit_solid(i) && layer_ok(i, mask) && aabb_overlap(px, py, pz, i)) {
			return 1;
		}
	}
	return 0;
}

static int enters_hit(float ox, float oy, float oz, float nx, float ny, float nz, int mask)
{
	for (int i = 0; i < s_hit_n; i++) {
		if (!hit_solid(i) || !layer_ok(i, mask)) {
			continue;
		}
		if (!aabb_overlap(ox, oy, oz, i) && aabb_overlap(nx, ny, nz, i)) {
			return 1;
		}
	}
	return 0;
}

void sys_io_slide(float vx, float vy, float vz, float delta, int mask)
{
	if (s_hitstop_ms > 0) {
		return;
	}
	if (mask == 0) {
		mask = 255;
	}
	if (!s_player_ok) {
		gs_draw_look_point(&s_px, &s_py, &s_pz);
		s_player_ok = 1;
	}
	s_on_floor = s_on_wall = s_on_ceil = 0;
	const float dx = vx * delta;
	const float dy = vy * delta;
	const float dz = vz * delta;
	float nx = s_px;
	float ny = s_py;
	float nz = s_pz;
	if (dx != 0.0f) {
		if (enters_hit(s_px, s_py, s_pz, s_px + dx, s_py, s_pz, mask)) {
			s_on_wall = 1;
		} else {
			nx += dx;
		}
	}
	if (dz != 0.0f) {
		if (enters_hit(nx, s_py, s_pz, nx, s_py, s_pz + dz, mask)) {
			s_on_wall = 1;
		} else {
			nz += dz;
		}
	}
	if (dy != 0.0f) {
		if (enters_hit(nx, s_py, nz, nx, s_py + dy, nz, mask)) {
			if (dy < 0.0f) {
				s_on_floor = 1;
			} else {
				s_on_ceil = 1;
			}
		} else {
			ny += dy;
		}
	}
	if (hits_at(nx, ny - 0.08f, nz, mask)) {
		s_on_floor = 1;
	}
	gs_draw_nudge3(nx - s_px, ny - s_py, nz - s_pz);
	s_px = nx;
	s_py = ny;
	s_pz = nz;
}

int sys_io_on_floor(void)
{
	return s_on_floor;
}

int sys_io_on_wall(void)
{
	return s_on_wall;
}

int sys_io_on_ceiling(void)
{
	return s_on_ceil;
}

int sys_io_hit_hazard(float x, float z)
{
	for (int i = 0; i < s_hit_n; i++) {
		if (s_hit_kit[i] == 5 && aabb_overlap(x, s_py, z, i)) {
			return 1;
		}
	}
	return 0;
}

static void ensure_player(void)
{
	if (!s_player_ok) {
		gs_draw_look_point(&s_px, &s_py, &s_pz);
		s_player_ok = 1;
	}
}

void sys_io_overlap_refresh(void)
{
	ensure_player();
	unsigned long long now = 0;
	for (int i = 0; i < s_hit_n && i < 64; i++) {
		if (hit_active(i) && aabb_overlap(s_px, s_py, s_pz, i)) {
			now |= (1ull << i);
		}
	}
	const unsigned long long entered = now & ~s_over_prev;
	const unsigned long long left = s_over_prev & ~now;
	s_over_entered = entered != 0;
	s_ent_n = 0;
	s_left_n = 0;
	for (int i = 0; i < s_hit_n && i < 64; i++) {
		if (entered & (1ull << i)) {
			s_ent_kit[s_ent_n] = s_hit_kit[i];
			s_ent_idx[s_ent_n] = i;
			s_ent_n++;
		}
		if (left & (1ull << i)) {
			s_left_kit[s_left_n] = s_hit_kit[i];
			s_left_n++;
		}
	}
	s_over_now = now;
	s_over_prev = now;
}

int sys_io_overlaps(void)
{
	return s_over_now != 0;
}

int sys_io_overlaps_entered(void)
{
	return s_over_entered;
}

int sys_io_has_overlapping(void)
{
	return s_over_now != 0;
}

int sys_io_hitbox_kind(void)
{
	for (int i = 0; i < s_hit_n && i < 64; i++) {
		if (s_over_now & (1ull << i)) {
			return (int)s_hit_kind[i];
		}
	}
	return 0;
}

int sys_io_hitbox_kit(void)
{
	for (int i = 0; i < s_hit_n && i < 64; i++) {
		if (s_over_now & (1ull << i)) {
			return (int)s_hit_kit[i];
		}
	}
	return 0;
}

int sys_io_entered_kit_count(void)
{
	return s_ent_n;
}

int sys_io_entered_kit_at(int i)
{
	if (i < 0 || i >= s_ent_n) {
		return 0;
	}
	return (int)s_ent_kit[i];
}

int sys_io_left_kit_count(void)
{
	return s_left_n;
}

int sys_io_left_kit_at(int i)
{
	if (i < 0 || i >= s_left_n) {
		return 0;
	}
	return (int)s_left_kit[i];
}

int sys_io_disable_entered_kit(int kit)
{
	int n = 0;
	for (int i = 0; i < s_ent_n; i++) {
		if (s_ent_kit[i] == (unsigned char)kit) {
			const int idx = s_ent_idx[i];
			if (idx >= 0 && idx < s_hit_n) {
				s_hit_on[idx] = 0;
				n = 1;
			}
		}
	}
	return n;
}

static int ray_aabb(float ox, float oy, float oz, float dx, float dy, float dz, float dist, int i, float *t_hit)
{
	float tmin = 0.0f;
	float tmax = dist;
	const float o[3] = { ox, oy, oz };
	const float d[3] = { dx, dy, dz };
	for (int a = 0; a < 3; a++) {
		const float mn = s_hit_mn[i][a];
		const float mx = s_hit_mx[i][a];
		if (d[a] > -0.00001f && d[a] < 0.00001f) {
			if (o[a] < mn || o[a] > mx) {
				return 0;
			}
			continue;
		}
		const float inv = 1.0f / d[a];
		float t0 = (mn - o[a]) * inv;
		float t1 = (mx - o[a]) * inv;
		if (t0 > t1) {
			const float tmp = t0;
			t0 = t1;
			t1 = tmp;
		}
		if (t0 > tmin) {
			tmin = t0;
		}
		if (t1 < tmax) {
			tmax = t1;
		}
		if (tmin > tmax) {
			return 0;
		}
	}
	*t_hit = tmin;
	return 1;
}

int sys_io_raycast(float ox, float oy, float oz, float dx, float dy, float dz, float dist, int mask)
{
	if (dist <= 0.0f) {
		return 0;
	}
	float best = dist;
	int hit = 0;
	for (int i = 0; i < s_hit_n; i++) {
		if (!hit_active(i)) {
			continue;
		}
		if (mask != 0 && mask != 255 && (mask & (int)s_hit_layer[i]) == 0) {
			continue;
		}
		float t = 0.0f;
		if (ray_aabb(ox, oy, oz, dx, dy, dz, dist, i, &t) && t >= 0.0f && t < best) {
			best = t;
			hit = 1;
		}
	}
	if (hit) {
		s_ray_x = ox + dx * best;
		s_ray_y = oy + dy * best;
		s_ray_z = oz + dz * best;
	}
	return hit;
}

void sys_io_ray_point(float *x, float *y, float *z)
{
	if (x) {
		*x = s_ray_x;
	}
	if (y) {
		*y = s_ray_y;
	}
	if (z) {
		*z = s_ray_z;
	}
}

int sys_io_tile_at(float x, float y)
{
	const short cx = (short)floorf(x);
	const short cy = (short)floorf(y);
	for (int i = 0; i < s_tile_n; i++) {
		if (s_tile_x[i] == cx && s_tile_y[i] == cy) {
			return (int)s_tile_flags[i];
		}
	}
	return 0;
}

int sys_io_tile_solid_at(float x, float y)
{
	return sys_io_tile_at(x, y) != 0;
}

void sys_io_set_fade(float alpha, float r, float g, float b)
{
	s_fade_left = 0.0f;
	s_fade_dur = 0.0f;
	s_fade_r = (int)r;
	s_fade_g = (int)g;
	s_fade_b = (int)b;
	int a = (int)(alpha * 128.0f);
	if (a < 0) {
		a = 0;
	}
	if (a > 128) {
		a = 128;
	}
	gs_draw_set_fade(a, s_fade_r, s_fade_g, s_fade_b);
}

void sys_io_scene_fade(float sec)
{
	s_fade_r = 0;
	s_fade_g = 0;
	s_fade_b = 0;
	if (sec <= 0.0f) {
		s_fade_left = 0.0f;
		s_fade_dur = 0.0f;
		s_fade_phase = 0;
		if (s_fade_pack >= 0) {
			pack_io_swap(s_fade_pack);
			s_fade_pack = -1;
		}
		gs_draw_set_fade(0, 0, 0, 0);
		return;
	}
	s_fade_dur = sec;
	s_fade_left = sec;
	s_fade_phase = 1;
	gs_draw_set_fade(0, 0, 0, 0);
}

void sys_io_set_fade_pack(int pack)
{
	s_fade_pack = pack;
}

void sys_io_move_planar(float ax, float ay, float speed, float delta)
{
	if (s_hitstop_ms > 0) {
		return;
	}
	sys_io_slide(ax * speed + s_kb_x, 0.0f, ay * speed + s_kb_z, delta, 255);
	s_kb_x *= 0.85f;
	s_kb_z *= 0.85f;
}

void sys_io_follow_node(float tx, float tz, float speed, float delta)
{
	float dx = tx - s_px;
	float dz = tz - s_pz;
	const float len = sqrtf(dx * dx + dz * dz);
	if (len < 0.05f) {
		return;
	}
	sys_io_move_planar(dx / len, dz / len, speed, delta);
}

int sys_io_set_hitbox_enabled(int node, int on)
{
	int n = 0;
	for (int i = 0; i < s_hit_n; i++) {
		if ((int)s_hit_node[i] == node) {
			s_hit_on[i] = on ? 1 : 0;
			n = 1;
		}
	}
	return n;
}

int sys_io_set_hitbox_layer(int node, int layer)
{
	int n = 0;
	for (int i = 0; i < s_hit_n; i++) {
		if ((int)s_hit_node[i] == node) {
			s_hit_layer[i] = (unsigned char)(layer & 255);
			n = 1;
		}
	}
	return n;
}

int sys_io_get_hitbox_layer(int node)
{
	for (int i = 0; i < s_hit_n; i++) {
		if ((int)s_hit_node[i] == node) {
			return (int)s_hit_layer[i];
		}
	}
	return 0;
}

int sys_io_hurt(int amount, float vx, float vy, float vz)
{
	if (s_invuln_ms > 0) {
		return sys_io_get_hp();
	}
	int hp = sys_io_get_hp() - amount;
	if (hp < 0) {
		hp = 0;
	}
	sys_io_set_hp(hp);
	sys_io_knockback(vx, vy, vz);
	return hp;
}

void sys_io_set_hitstop(float ms)
{
	s_hitstop_ms = (int)ms;
}

void sys_io_set_invuln(float ms)
{
	s_invuln_ms = (int)ms;
}

int sys_io_is_invuln(void)
{
	return s_invuln_ms > 0;
}

void sys_io_knockback(float vx, float vy, float vz)
{
	s_kb_x = vx;
	s_kb_y = vy;
	s_kb_z = vz;
}

void sys_io_set_flip(int flip)
{
	s_flip = flip;
	for (int i = 0; i < s_sprt_n; i++) {
		s_sprt_flip[i] = (unsigned char)flip;
		gs_draw_sprt(i, s_sprt_x[i], s_sprt_y[i], s_sprt_w[i], s_sprt_h[i], s_sprt_tex[i], s_sprt_flip[i]);
	}
}

int sys_io_sprite_count(void)
{
	return s_sprt_n;
}

int sys_io_load_sprites(int pack)
{
	if (pack < 0) {
		pack = pack_io_current();
	}
	if (pack > 0 && !pack_io_is_loaded(pack)) {
		if (!pack_io_can_fit(pack) || !pack_io_instantiate(pack)) {
			return 0;
		}
	}
	s_sprt_n = 0;
	load_sys_named(pack, "SPRT", apply_sprt);
	return s_sprt_loaded;
}

int sys_io_unload_sprites(void)
{
	s_sprt_n = 0;
	s_sprt_loaded = 0;
	gs_draw_clear_sprt();
	return 1;
}

int sys_io_sprites_loaded(void)
{
	return s_sprt_loaded;
}

int sys_io_load_anims(int pack)
{
	load_sys_named(pack, "ANIM", apply_anim);
	return s_anim_loaded;
}

int sys_io_unload_anims(void)
{
	s_anim_n = 0;
	s_anim_keys = 0;
	s_anim_loaded = 0;
	s_anim_playing = 0;
	return 1;
}

int sys_io_anims_loaded(void)
{
	return s_anim_loaded;
}

int sys_io_load_hits(int pack)
{
	load_sys_named(pack, "HIT", apply_hit);
	return s_hit_loaded;
}

int sys_io_unload_hits(void)
{
	s_hit_n = 0;
	s_hit_loaded = 0;
	return 1;
}

int sys_io_hits_loaded(void)
{
	return s_hit_loaded;
}

int sys_io_load_tiles(int pack)
{
	load_sys_named(pack, "TILE", apply_tile);
	s_tile_loaded = s_tile_n > 0;
	return s_tile_loaded;
}

int sys_io_unload_tiles(void)
{
	s_tile_n = 0;
	s_tile_loaded = 0;
	gs_draw_clear_tiles();
	return 1;
}

int sys_io_tiles_loaded(void)
{
	return s_tile_loaded;
}

int sys_io_load_hud(int pack)
{
	load_sys_named(pack, "HUD", apply_hud);
	s_hud_loaded = s_hud_n > 0;
	return s_hud_loaded;
}

int sys_io_unload_hud(void)
{
	s_hud_n = 0;
	s_hud_loaded = 0;
	for (int i = 0; i < 16; i++) {
		s_hud_text[i][0] = 0;
	}
	gs_draw_clear_hud();
	return 1;
}

int sys_io_hud_loaded(void)
{
	return s_hud_loaded;
}

int sys_io_set_cell(int x, int y, int flags)
{
	for (int i = 0; i < s_tile_n; i++) {
		if (s_tile_x[i] == (short)x && s_tile_y[i] == (short)y) {
			s_tile_flags[i] = (unsigned char)flags;
			if (flags & 1) {
				gs_draw_tile(i, x, y, s_tile_atlas[i]);
			}
			return 1;
		}
	}
	if (s_tile_n >= 256) {
		return 0;
	}
	s_tile_x[s_tile_n] = (short)x;
	s_tile_y[s_tile_n] = (short)y;
	s_tile_flags[s_tile_n] = (unsigned char)flags;
	s_tile_atlas[s_tile_n] = 0;
	gs_draw_tile(s_tile_n, x, y, 0);
	s_tile_n++;
	s_tile_loaded = 1;
	return 1;
}

int sys_io_erase_cell(int x, int y)
{
	int w = 0;
	int found = 0;
	for (int i = 0; i < s_tile_n; i++) {
		if (s_tile_x[i] == (short)x && s_tile_y[i] == (short)y) {
			found = 1;
			continue;
		}
		s_tile_x[w] = s_tile_x[i];
		s_tile_y[w] = s_tile_y[i];
		s_tile_flags[w] = s_tile_flags[i];
		s_tile_atlas[w] = s_tile_atlas[i];
		w++;
	}
	s_tile_n = w;
	gs_draw_clear_tiles();
	for (int i = 0; i < s_tile_n; i++) {
		if (s_tile_flags[i] & 1) {
			gs_draw_tile(i, s_tile_x[i], s_tile_y[i], s_tile_atlas[i]);
		}
	}
	return found;
}

void sys_io_spawn_ofs(float x, float y, float z)
{
	const int mesh_node = gs_draw_primary_mesh_node();
	gs_draw_set_node_ofs(0, x, y, z);
	if (mesh_node != 0) {
		gs_draw_set_node_ofs(mesh_node, x, y, z);
	}
	s_px = x;
	s_py = y;
	s_pz = z;
	s_player_ok = 1;
}

void sys_io_get_pos(float *x, float *y, float *z)
{
	if (x) {
		*x = s_px;
	}
	if (y) {
		*y = s_py;
	}
	if (z) {
		*z = s_pz;
	}
}

int sys_io_paused(void)
{
	return s_paused;
}

void sys_io_set_paused(int on)
{
	s_paused = on ? 1 : 0;
}

int sys_io_btn_count(void)
{
	return s_btn_n;
}

int sys_io_hud_just_accept(void)
{
	const int v = s_hud_just_accept;
	s_hud_just_accept = 0;
	return v;
}

const char *sys_io_hud_action(void)
{
	return s_hud_act;
}

void sys_io_move_6dof(float ax, float ay, float az, float pitch, float yaw, float roll, float speed, float delta)
{
	if (s_hitstop_ms > 0) {
		return;
	}
	if (s_vehl_mode == 0) {
		sys_io_move_planar(ax, az, speed + s_vehl_thrust, delta);
		gs_draw_look_delta(s_vehl_steer * delta, 0.0f);
		return;
	}
	if (s_vehl_mode == 1) {
		sys_io_slide(ax * speed, ay * s_vehl_thrust, az * speed, delta, 255);
		return;
	}
	sys_io_slide(ax * speed, ay * speed, az * speed, delta, 255);
	gs_draw_look(yaw, pitch, roll);
}

void sys_io_set_steer(float steer)
{
	s_vehl_steer = steer;
}

void sys_io_set_thrust(float thrust)
{
	s_vehl_thrust = thrust;
}

void sys_io_set_eye_height(float h)
{
	s_eye_h = h;
	gs_draw_set_eye_height(h);
}

int sys_io_load_particles(int pack)
{
	if (pack < 0) {
		pack = pack_io_current();
	}
	if (pack > 0 && !pack_io_is_loaded(pack)) {
		if (!pack_io_can_fit(pack) || !pack_io_instantiate(pack)) {
			return 0;
		}
	}
	s_part_n = 0;
	s_live_n = 0;
	load_sys_named(pack, "PART", apply_part);
	return s_part_loaded;
}

int sys_io_unload_particles(void)
{
	s_part_n = 0;
	s_live_n = 0;
	s_part_loaded = 0;
	return 1;
}

int sys_io_particles_loaded(void)
{
	return s_part_loaded;
}

void sys_io_path_follow(float speed, float delta)
{
	if (s_path_n < 2) {
		if (s_nav_n >= 2) {
			sys_io_nav_follow(speed, delta);
		}
		return;
	}
	s_path_off += speed * delta;
	float maxo = (float)(s_path_n - 1);
	if (s_path_off < 0.0f) {
		s_path_off = 0.0f;
	}
	if (s_path_off > maxo) {
		s_path_off = maxo;
	}
	int a = (int)s_path_off;
	int b = a + 1;
	if (b >= s_path_n) {
		b = s_path_n - 1;
	}
	const float u = s_path_off - (float)a;
	const float x = s_path_x[a] + (s_path_x[b] - s_path_x[a]) * u;
	const float z = s_path_z[a] + (s_path_z[b] - s_path_z[a]) * u;
	sys_io_follow_node(x, z, speed, delta);
}

void sys_io_set_path_offset(float t)
{
	s_path_off = t;
}

int sys_io_navmesh_next(int from, int to)
{
	if (s_navm_nt < 1) {
		return sys_io_nav_next(from, to);
	}
	int n = from + 1;
	if (n >= s_navm_nt) {
		n = 0;
	}
	const int ia = (int)s_navm_a[n];
	if (ia >= 0 && ia < s_navm_nv) {
		sys_io_follow_node(s_navm_x[ia], s_navm_z[ia], 1.0f, 0.016f);
	}
	return n;
}

int sys_io_prefetch(int pack)
{
	return pack_io_prefetch(pack);
}
