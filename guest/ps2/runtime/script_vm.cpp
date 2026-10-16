// MIT. Interpret cooked SCRP ABI 1 (rotate_y + kit name/id bind). No Godot/Luau VM.

#include "script_vm.h"
#include "iso_path.h"

#include "gdir_vm.h"
#include "gs_draw.h"
#include "pack_io.h"
#include "pad_io.h"
#include "sfx_io.h"
#include "sys_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef BLAZIUM_PS2_COOK_ABI
#define BLAZIUM_PS2_COOK_ABI 1
#endif

enum {
	OP_END = 0,
	OP_LOADK_F = 1,
	OP_LOAD_DELTA = 2,
	OP_MUL = 3,
	OP_CALL_ROTATE_Y = 4,
	OP_READY = 5,
	OP_PROCESS = 6,
	OP_HAS_FEATURE_PS2 = 7,
	OP_SWAP_PACK = 8,
	OP_PLAY_SFX = 9,
	OP_SET_RUMBLE = 10,
	OP_STOP_RUMBLE = 11,
	OP_INPUT_TRANSLATE = 12,
	OP_JUST_ACCEPT_SFX = 13,
	OP_USER_SAVE = 14,
	OP_USER_LOAD = 15,
	OP_MEMCARD_PRESENT = 16,
	OP_KIT_TICK = 17,
	OP_SAY = 18,
	OP_NAV_FOLLOW = 19,
	OP_SAVE_SLOT = 20,
	OP_PLAY_FMV = 21,
	OP_SYS_TICK = 22,
	OP_PLAY_MUSIC = 23,
	OP_STOP_MUSIC = 24,
	OP_MUSIC_VOL = 25,
	OP_LOOK_STICK = 26,
	OP_LOOK_CAMERA = 27,
	OP_ORBIT_CAMERA = 28,
	OP_SHAKE_CAMERA = 29,
	OP_NEXT_CAMERA = 30,
	OP_ATTACH_CAMERA = 31,
	OP_TWEEN = 32,
	OP_TIMER = 33,
	OP_KILL_TWEENS = 34,
	OP_SLIDE = 35,
	OP_OVERLAP = 36,
	OP_RAYCAST = 37,
	OP_TILE_AT = 38,
	OP_SET_FADE = 39,
	OP_SCENE_FADE = 40,
	OP_CHANGE_SCENE = 41,
	OP_INSTANTIATE = 42,
	OP_UNLOAD_PACK = 43,
	OP_SAY_TEXT = 44,
	OP_SEEK_ANIM = 45,
	OP_JMP = 46,
	OP_JZ = 47,
	OP_CALL_NATIVE = 48,
	OP_LOADK_S = 49,
	OP_LOAD_SCENE = 50,
	NAT_EE_USED = 51,
	NAT_EE_FREE = 52,
	NAT_EE_LIMIT = 53,
	NAT_GS_USED = 54,
	NAT_GS_FREE = 55,
	NAT_GS_LIMIT = 56,
	NAT_PACK_COST_EE = 57,
	NAT_PACK_COST_GS = 58,
	NAT_CAN_LOAD = 59,
	NAT_CAN_INSTANTIATE = 60,
	NAT_IS_LOADED = 61,
	NAT_LOADED_COUNT = 62,
	NAT_SET_CAM = 63,
	NAT_SET_DEFAULT_CAM = 64,
	NAT_GET_CAM = 65,
	NAT_GET_CAM_COUNT = 66,
	NAT_MAKE_CURRENT = 67,
	NAT_PREV_CAM = 68,
	NAT_GET_DEFAULT_CAM = 69,
	NAT_LOAD_SPRITES = 70,
	NAT_UNLOAD_SPRITES = 71,
	NAT_CAN_LOAD_SPRITES = 72,
	NAT_IS_SPRITES_LOADED = 73,
	NAT_LOAD_ANIMS = 74,
	NAT_UNLOAD_ANIMS = 75,
	NAT_IS_ANIMS_LOADED = 76,
	NAT_LOAD_HITS = 77,
	NAT_UNLOAD_HITS = 78,
	NAT_IS_HITS_LOADED = 79,
	NAT_LOAD_TILES = 80,
	NAT_MOVE_PLANAR = 81,
	NAT_FOLLOW_NODE = 82,
	NAT_HURT = 83,
	NAT_SET_HITSTOP = 84,
	NAT_SET_INVULN = 85,
	NAT_IS_INVULN = 86,
	NAT_KNOCKBACK = 87,
	NAT_SET_HP = 88,
	NAT_GET_HP = 89,
	NAT_HITBOX_ON = 90,
	NAT_HITBOX_LAYER = 91,
	NAT_GET_PRESSURE = 92,
	NAT_SET_DEADZONE = 93,
	NAT_GET_STICK = 94,
	NAT_POKE = 95,
	NAT_PEEK = 96,
	NAT_SPAWN = 97,
	NAT_OVERLAPS_ENTERED = 98,
	NAT_SET_FLIP = 99,
	NAT_MOVE_6DOF = 100,
	NAT_LOAD_PARTICLES = 101,
	NAT_UNLOAD_PARTICLES = 102,
	NAT_CAN_LOAD_PARTICLES = 103,
	NAT_IS_PARTICLES_LOADED = 104,
	NAT_PATH_FOLLOW = 105,
	NAT_SET_PATH_OFFSET = 106,
	NAT_SET_THRUST = 107,
	NAT_SET_STEER = 108,
	NAT_SET_EYE_HEIGHT = 109,
	NAT_PREFETCH_SCENE = 110,
	NAT_NAVMESH = 111,
	NAT_TWEEN_PLAY = 112,
	NAT_TWEEN_KILL = 113,
	NAT_UNLOAD_TILES = 114,
	NAT_LOAD_HUD = 115,
	NAT_UNLOAD_HUD = 116,
	NAT_CAN_LOAD_HUD = 117,
	NAT_IS_HUD_LOADED = 118,
	NAT_LOAD_MUSIC = 119,
	NAT_UNLOAD_MUSIC = 120,
	NAT_CAN_LOAD_MUSIC = 121,
	NAT_IS_MUSIC_LOADED = 122,
	NAT_PLAY_ANIM = 123,
	NAT_STOP_ANIM = 124,
	NAT_SET_CHECKPOINT = 125,
	NAT_RESPAWN = 126
};

static char s_str[8][128];
static int s_str_n;
static int s_last_str;

static const unsigned char *s_tape;
static unsigned s_tape_n;
static unsigned s_process_off;
static int s_ready;
static int s_kit_spawn;
static int s_kit_player;
static int s_kit_portal;
static int s_kit_checkpoint;
static int s_kit_hazard;
static int s_kit_pickup;
static int s_kit_music;
static int s_kit_music_on;
static int s_kit_unload;
static int s_kit_load;
static int s_kit_talk;
static int s_kit_spawner;
static int s_kit_save;
static int s_kit_camvol;
static int s_kit_follow;
static float s_follow_speed;
static float s_spawn_x;
static float s_spawn_y;
static float s_spawn_z;
static int s_spawn_ok;
static float s_chk_x;
static float s_chk_y;
static float s_chk_z;
static int s_chk_pack;
static int s_chk_ok;
static float s_grav_vy;
static char s_kit_spawn_path[65];
static char s_kit_portal_path[65];
static char s_kit_load_path[65];
static char s_kit_unload_path[65];
static char s_kit_talk_line[65];
static int s_kit_cam_index;
static int s_kit_save_slot;
static int s_kit_hurt_amt;
static int s_kit_load_handle;
static int s_did_slide;
static float s_yaw;

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

static int resolve_pack(void)
{
	if (s_last_str >= 0 && s_last_str < 8 && s_str[s_last_str][0]) {
		const int found = pack_io_find_path(s_str[s_last_str]);
		if (found >= 0) {
			return found;
		}
	}
	return pack_io_current();
}

static int name_is_ci(const char *n, const char *want)
{
	if (!n || !want) {
		return 0;
	}
	unsigned i = 0;
	while (n[i] && want[i]) {
		char a = n[i];
		char b = want[i];
		if (a >= 'A' && a <= 'Z') {
			a = (char)(a - 'A' + 'a');
		}
		if (b >= 'A' && b <= 'Z') {
			b = (char)(b - 'A' + 'a');
		}
		if (a != b) {
			return 0;
		}
		i++;
	}
	return n[i] == 0 && want[i] == 0;
}

static void apply_kit(unsigned char kit, const char *name)
{
	if (kit == 1 || name_is_ci(name, "Spawn")) {
		s_kit_spawn = 1;
	}
	if (kit == 2 || name_is_ci(name, "Checkpoint")) {
		s_kit_checkpoint = 1;
	}
	if (kit == 3 || name_is_ci(name, "Portal")) {
		s_kit_portal = 1;
	}
	if (kit == 4 || name_is_ci(name, "Player") || name_is_ci(name, "Hero")) {
		s_kit_player = 1;
	}
	if (kit == 5 || name_is_ci(name, "Hazard")) {
		s_kit_hazard = 1;
	}
	if (kit == 6 || name_is_ci(name, "Pickup")) {
		s_kit_pickup = 1;
	}
	if (kit == 7 || name_is_ci(name, "Music") || name_is_ci(name, "MusicZone")) {
		s_kit_music = 1;
	}
	if (kit == 8 || name_is_ci(name, "Unload") || name_is_ci(name, "UnloadVolume")) {
		s_kit_unload = 1;
	}
	if (kit == 9 || name_is_ci(name, "Load") || name_is_ci(name, "LoadZone")) {
		s_kit_load = 1;
	}
	if (kit == 10 || name_is_ci(name, "Talk") || name_is_ci(name, "TalkZone")) {
		s_kit_talk = 1;
	}
	if (kit == 11 || name_is_ci(name, "Spawner") || name_is_ci(name, "Instance")) {
		s_kit_spawner = 1;
	}
	if (kit == 12 || name_is_ci(name, "Save") || name_is_ci(name, "SaveZone") || name_is_ci(name, "SavePoint")) {
		s_kit_save = 1;
	}
	if (kit == 13 || name_is_ci(name, "CameraVolume") || name_is_ci(name, "CamVol")) {
		s_kit_camvol = 1;
	}
	if (kit == 14 || name_is_ci(name, "Follow") || name_is_ci(name, "KitFollow") || name_is_ci(name, "CamFollow")) {
		s_kit_follow = 1;
	}
}

static void bind_nodes(const unsigned char *blob, unsigned sz)
{
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
		char name[33];
		memcpy(name, n + 42, 32);
		name[32] = 0;
		apply_kit(n[5], name);
		if (n[5] == 1 || name_is_ci(name, "Spawn") || name_is_ci(name, "KitSpawn")) {
			s_spawn_x = rf32(n + 6);
			s_spawn_y = rf32(n + 10);
			s_spawn_z = rf32(n + 14);
			s_spawn_ok = 1;
		}
		if (rec >= 138) {
			if (n[5] == 11 || name_is_ci(name, "Spawner") || name_is_ci(name, "Instance")) {
				memcpy(s_kit_spawn_path, n + 74, 64);
				s_kit_spawn_path[64] = 0;
			}
			if (n[5] == 3 || name_is_ci(name, "Portal")) {
				memcpy(s_kit_portal_path, n + 74, 64);
				s_kit_portal_path[64] = 0;
			}
			if (n[5] == 9 || name_is_ci(name, "Load") || name_is_ci(name, "LoadZone")) {
				memcpy(s_kit_load_path, n + 74, 64);
				s_kit_load_path[64] = 0;
			}
			if (n[5] == 8 || name_is_ci(name, "Unload") || name_is_ci(name, "UnloadZone") || name_is_ci(name, "UnloadVolume")) {
				memcpy(s_kit_unload_path, n + 74, 64);
				s_kit_unload_path[64] = 0;
			}
			if (n[5] == 10 || name_is_ci(name, "Talk") || name_is_ci(name, "TalkZone")) {
				memcpy(s_kit_talk_line, n + 74, 64);
				s_kit_talk_line[64] = 0;
			}
			if (n[5] == 13 || name_is_ci(name, "CameraVolume") || name_is_ci(name, "CamVol")) {
				s_kit_cam_index = atoi((const char *)(n + 74));
			}
			if (n[5] == 12 || name_is_ci(name, "Save") || name_is_ci(name, "SaveZone") || name_is_ci(name, "SavePoint")) {
				s_kit_save_slot = atoi((const char *)(n + 74));
			}
			if (n[5] == 5 || name_is_ci(name, "Hazard")) {
				s_kit_hurt_amt = atoi((const char *)(n + 74));
			}
			if (n[5] == 14 || name_is_ci(name, "Follow") || name_is_ci(name, "KitFollow") || name_is_ci(name, "CamFollow")) {
				const float spd = (float)atof((const char *)(n + 74));
				s_follow_speed = spd > 0.05f ? spd : 4.0f;
			}
		}
	}
}

static void try_bind_node_pack(int pack)
{
	char host[40];
	char iso_bs[48];
	char iso[48];
	sprintf(host, "host:NODE%02d.bin", pack);
	sprintf(iso_bs, "cdrom0:\\NODE%02d.BIN;1", pack);
	sprintf(iso, "cdrom0:NODE%02d.BIN;1", pack);
	const char *paths[4];
	paths[0] = host;
	paths[1] = iso_bs;
	paths[2] = iso;
	paths[3] = NULL;
	for (int i = 0; paths[i]; i++) {
		FILE *f = iso_fopen(paths[i], "rb");
		if (!f) {
			continue;
		}
		if (fseek(f, 0, SEEK_END) != 0) {
			fclose(f);
			continue;
		}
		long n = ftell(f);
		if (n < 8) {
			fclose(f);
			continue;
		}
		rewind(f);
		unsigned char *buf = (unsigned char *)malloc((size_t)n);
		if (!buf) {
			fclose(f);
			continue;
		}
		if (fread(buf, 1, (size_t)n, f) == (size_t)n) {
			bind_nodes(buf, (unsigned)n);
		}
		free(buf);
		fclose(f);
		return;
	}
}

static void rebind_current_pack(void)
{
	const unsigned char *node = 0;
	unsigned sz = 0;
	pack_io_current_node(&node, &sz);
	if (node && sz) {
		bind_nodes(node, sz);
	}
}

static int hint_is_path(const char *s)
{
	if (!s || !s[0]) {
		return 0;
	}
	if (s[0] == 'r' && s[1] == 'e' && s[2] == 's') {
		return 1;
	}
	return strstr(s, ".tscn") != 0 || strstr(s, ".bin") != 0;
}

static int pack_from_hint(const char *path, int fallback)
{
	if (hint_is_path(path)) {
		const int found = pack_io_find_path(path);
		if (found >= 0) {
			return found;
		}
	}
	return fallback;
}

static void fire_kit(unsigned char kit);

static void do_checkpoint(void)
{
	sys_io_get_pos(&s_chk_x, &s_chk_y, &s_chk_z);
	s_chk_pack = pack_io_current();
	s_chk_ok = 1;
}

static void do_respawn(void)
{
	if (s_chk_ok) {
		sys_io_spawn_ofs(s_chk_x, s_chk_y, s_chk_z);
		if (s_chk_pack >= 0 && s_chk_pack != pack_io_current()) {
			if (pack_io_is_loaded(s_chk_pack) || pack_io_can_fit(s_chk_pack)) {
				pack_io_swap(s_chk_pack);
			}
		}
	} else if (s_spawn_ok) {
		sys_io_spawn_ofs(s_spawn_x, s_spawn_y, s_spawn_z);
	}
	s_grav_vy = 0.0f;
	if (sys_io_get_hp() <= 0) {
		sys_io_set_hp(3);
	}
}

static int act_is(const char *act, const char *want)
{
	if (!act || !want) {
		return 0;
	}
	unsigned i = 0;
	while (want[i]) {
		char a = act[i];
		char b = want[i];
		if (a >= 'A' && a <= 'Z') {
			a = (char)(a - 'A' + 'a');
		}
		if (a != b) {
			return 0;
		}
		i++;
	}
	return act[i] == 0 || act[i] == ':' || act[i] == '|';
}

static void run_hud_action(const char *act)
{
	if (!act || !act[0]) {
		return;
	}
	if (act_is(act, "pause")) {
		sys_io_set_paused(1);
		return;
	}
	if (act_is(act, "resume")) {
		sys_io_set_paused(0);
		return;
	}
	if (act_is(act, "save")) {
		(void)pack_io_user_save_slot(0, pack_io_poke_data(), pack_io_poke_size());
		return;
	}
	if (act_is(act, "load")) {
		fire_kit(9);
		return;
	}
	if (act_is(act, "respawn")) {
		do_respawn();
		return;
	}
	if (act_is(act, "say")) {
		const char *line = act;
		while (*line && *line != ':') {
			line++;
		}
		if (*line == ':' && line[1]) {
			sys_io_say(line + 1);
		} else {
			sys_io_say("PS2");
		}
		return;
	}
	if (act_is(act, "portal")) {
		fire_kit(3);
	}
}

static void fire_kit(unsigned char kit)
{
	if (kit == 1 && s_spawn_ok) {
		sys_io_spawn_ofs(s_spawn_x, s_spawn_y, s_spawn_z);
	}
	if (kit == 2) {
		do_checkpoint();
	}
	if (kit == 12) {
		if (!pack_io_user_save_slot(s_kit_save_slot, pack_io_poke_data(), pack_io_poke_size())) {
			(void)pack_io_user_error();
		}
	}
	if (kit == 3) {
		int pack = pack_from_hint(s_kit_portal_path, pack_io_current() + 1);
		if (pack < 0) {
			pack = 1;
		}
		if (pack_io_is_loaded(pack) || pack_io_can_fit(pack)) {
			sys_io_set_fade_pack(pack);
			sys_io_scene_fade(0.2f);
		}
	}
	if (kit == 5) {
		const int amt = s_kit_hurt_amt > 0 ? s_kit_hurt_amt : 1;
		(void)sys_io_hurt(amt, 0.0f, 0.0f, 0.0f);
		pad_io_set_rumble(1, 64);
		sfx_io_play();
		if (sys_io_get_hp() <= 0) {
			do_respawn();
		}
	}
	if (kit == 6) {
		sfx_io_play();
		sys_io_disable_entered_kit(6);
	}
	if (kit == 7) {
		const int pack = pack_io_current();
		if (pack <= 0 || pack_io_is_loaded(pack) || pack_io_can_fit(pack)) {
			if (sfx_io_music_load(pack)) {
				sfx_io_music_play();
				s_kit_music_on = 1;
			}
		}
	}
	if (kit == 8) {
		int pack = pack_from_hint(s_kit_unload_path, s_kit_load_handle);
		if (pack <= 0) {
			pack = pack_io_current();
		}
		if (pack <= 0) {
			pack = pack_io_max();
		}
		if (pack > 0) {
			pack_io_unload(pack);
			sfx_io_unload();
		}
	}
	if (kit == 9) {
		int pack = pack_from_hint(s_kit_load_path, pack_io_current() + 1);
		if (pack < 0) {
			pack = 1;
		}
		if (pack_io_is_loaded(pack) || pack_io_can_fit(pack)) {
			if (pack_io_instantiate(pack)) {
				s_kit_load_handle = pack;
				if (pack <= 0 || pack_io_is_loaded(pack) || pack_io_can_fit(pack)) {
					(void)sfx_io_load(pack);
				}
			}
		}
	}
	if (kit == 10) {
		if (s_kit_talk_line[0]) {
			sys_io_say(s_kit_talk_line);
		} else {
			sys_io_say("PS2");
		}
		sfx_io_play();
	}
	if (kit == 11) {
		int pack = pack_from_hint(s_kit_spawn_path, pack_io_current() + 1);
		if (pack < 0) {
			pack = 1;
		}
		if (pack_io_is_loaded(pack) || pack_io_can_fit(pack)) {
			(void)pack_io_instantiate(pack > 0 ? pack : 1);
		}
	}
	if (kit == 13) {
		(void)sys_io_set_cam(s_kit_cam_index);
	}
}

static void kit_tick(void)
{
	if (sys_io_paused()) {
		return;
	}
	sys_io_overlap_refresh();
	const int n = sys_io_entered_kit_count();
	for (int i = 0; i < n; i++) {
		fire_kit((unsigned char)sys_io_entered_kit_at(i));
	}
	const int left = sys_io_left_kit_count();
	for (int i = 0; i < left; i++) {
		if (sys_io_left_kit_at(i) == 7) {
			sfx_io_music_unload();
			s_kit_music_on = 0;
		}
	}
	if (pad_io_just_pressed(4)) {
		if (s_kit_talk) {
			fire_kit(10);
		}
		if (s_kit_pickup) {
			fire_kit(6);
		}
	}
}

static void run_range(unsigned from, unsigned to, float delta)
{
	float stack[8];
	int sp = 0;
	unsigned i = from;
	while (i < to && i < s_tape_n) {
		const unsigned char op = s_tape[i++];
		if (op == OP_END) {
			break;
		}
		if (op == OP_READY) {
			continue;
		}
		if (op == OP_PROCESS) {
			break;
		}
		if (op == OP_LOADK_F) {
			if (i + 4 > s_tape_n || sp >= 8) {
				return;
			}
			stack[sp++] = rf32(s_tape + i);
			i += 4;
			continue;
		}
		if (op == OP_LOAD_DELTA) {
			if (sp >= 8) {
				return;
			}
			stack[sp++] = delta;
			continue;
		}
		if (op == OP_MUL) {
			if (sp < 2) {
				return;
			}
			const float b = stack[--sp];
			const float a = stack[--sp];
			stack[sp++] = a * b;
			continue;
		}
		if (op == OP_CALL_ROTATE_Y) {
			if (sp < 1) {
				return;
			}
			s_yaw += stack[--sp];
			gs_draw_set_world_yaw(s_yaw);
			continue;
		}
		if (op == OP_HAS_FEATURE_PS2) {
			if (sp < 8) {
				stack[sp++] = 1.0f;
			}
			continue;
		}
		if (op == OP_SWAP_PACK) {
			if (sp < 1) {
				return;
			}
			pack_io_swap((int)stack[--sp]);
			rebind_current_pack();
			continue;
		}
		if (op == OP_PLAY_SFX) {
			sfx_io_play();
			continue;
		}
		if (op == OP_SET_RUMBLE) {
			if (sp < 2) {
				return;
			}
			const int large = (int)stack[--sp];
			const int small = (int)stack[--sp];
			pad_io_set_rumble(small != 0, large);
			continue;
		}
		if (op == OP_STOP_RUMBLE) {
			pad_io_set_rumble(0, 0);
			continue;
		}
		if (op == OP_INPUT_TRANSLATE) {
			if (sp < 1) {
				return;
			}
			const float rate = stack[--sp];
			float sx = 0.0f;
			float sy = 0.0f;
			pad_io_stick(0, &sx, &sy);
			sys_io_slide(sx * rate, 0.0f, sy * rate, delta, 255);
			s_did_slide = 1;
			continue;
		}
		if (op == OP_JUST_ACCEPT_SFX) {
			if (pad_io_just_pressed(4)) {
				sfx_io_play();
			}
			continue;
		}
		if (op == OP_USER_SAVE) {
			if (!pack_io_user_save(pack_io_poke_data(), pack_io_poke_size())) {
				(void)pack_io_user_error();
			}
			continue;
		}
		if (op == OP_USER_LOAD) {
			unsigned char slot[8];
			(void)pack_io_user_load(slot, 8);
			continue;
		}
		if (op == OP_MEMCARD_PRESENT) {
			if (sp < 8) {
				stack[sp++] = pack_io_user_present() ? 1.0f : 0.0f;
			}
			continue;
		}
		if (op == OP_KIT_TICK) {
			continue;
		}
		if (op == OP_SAY) {
			if (s_last_str >= 0 && s_last_str < 8 && s_str[s_last_str][0]) {
				sys_io_say(s_str[s_last_str]);
			} else {
				sys_io_say("PS2");
			}
			continue;
		}
		if (op == OP_NAV_FOLLOW) {
			sys_io_nav_follow(1.0f, delta);
			continue;
		}
		if (op == OP_SAVE_SLOT) {
			int slot = 0;
			if (sp >= 1) {
				slot = (int)stack[--sp];
			}
			if (!pack_io_user_save_slot(slot, 0, 0)) {
				(void)pack_io_user_error();
			}
			continue;
		}
		if (op == OP_PLAY_FMV) {
			(void)sys_io_play_fmv();
			(void)sys_io_fmv_error();
			continue;
		}
		if (op == OP_SYS_TICK) {
			sys_io_tick(delta);
			continue;
		}
		if (op == OP_PLAY_MUSIC) {
			sfx_io_music_play();
			continue;
		}
		if (op == OP_STOP_MUSIC) {
			sfx_io_music_stop();
			continue;
		}
		if (op == OP_MUSIC_VOL) {
			if (sp < 1) {
				return;
			}
			sfx_io_music_set_vol(stack[--sp]);
			continue;
		}
		if (op == OP_LOOK_STICK) {
			sys_io_look_stick(delta);
			continue;
		}
		if (op == OP_LOOK_CAMERA) {
			if (sp < 3) {
				return;
			}
			const float roll = stack[--sp];
			const float pitch = stack[--sp];
			const float yaw = stack[--sp];
			sys_io_look(yaw, pitch, roll);
			continue;
		}
		if (op == OP_ORBIT_CAMERA) {
			if (sp < 3) {
				return;
			}
			const float dist = stack[--sp];
			const float pitch = stack[--sp];
			const float yaw = stack[--sp];
			sys_io_orbit(yaw, pitch, dist);
			continue;
		}
		if (op == OP_SHAKE_CAMERA) {
			if (sp < 2) {
				return;
			}
			const float ms = stack[--sp];
			const float amp = stack[--sp];
			sys_io_shake(amp, ms);
			continue;
		}
		if (op == OP_NEXT_CAMERA) {
			sys_io_next_cam();
			continue;
		}
		if (op == OP_ATTACH_CAMERA) {
			if (sp < 3) {
				return;
			}
			const float oz = stack[--sp];
			const float oy = stack[--sp];
			const float ox = stack[--sp];
			sys_io_attach(ox, oy, oz);
			continue;
		}
		if (op == OP_TWEEN) {
			if (sp < 4) {
				return;
			}
			const float kind = stack[--sp];
			const float sec = stack[--sp];
			const float to = stack[--sp];
			const float from = stack[--sp];
			(void)sys_io_tween_start(from, to, sec, (int)kind);
			continue;
		}
		if (op == OP_TIMER) {
			if (sp < 1) {
				return;
			}
			(void)sys_io_timer_start(stack[--sp]);
			continue;
		}
		if (op == OP_KILL_TWEENS) {
			sys_io_kill_tweens();
			continue;
		}
		if (op == OP_SLIDE) {
			if (sp < 3) {
				return;
			}
			int mask = 255;
			if (sp >= 4) {
				mask = (int)stack[--sp];
			}
			const float vz = stack[--sp];
			const float vy = stack[--sp];
			const float vx = stack[--sp];
			sys_io_slide(vx, vy, vz, delta, mask);
			s_did_slide = 1;
			continue;
		}
		if (op == OP_OVERLAP) {
			sys_io_overlap_refresh();
			if (sp < 8) {
				stack[sp++] = sys_io_overlaps() ? 1.0f : 0.0f;
			}
			continue;
		}
		if (op == OP_RAYCAST) {
			if (sp < 4) {
				return;
			}
			const float dist = stack[--sp];
			const float dz = stack[--sp];
			const float dy = stack[--sp];
			const float dx = stack[--sp];
			float px = 0.0f, py = 0.0f, pz = 0.0f;
			gs_draw_look_point(&px, &py, &pz);
			const int hit = sys_io_raycast(px, py, pz, dx, dy, dz, dist, 255);
			if (sp < 8) {
				stack[sp++] = hit ? 1.0f : 0.0f;
			}
			continue;
		}
		if (op == OP_TILE_AT) {
			if (sp < 2) {
				return;
			}
			const float y = stack[--sp];
			const float x = stack[--sp];
			if (sp < 8) {
				stack[sp++] = (float)sys_io_tile_at(x, y);
			}
			continue;
		}
		if (op == OP_SET_FADE) {
			if (sp < 4) {
				return;
			}
			const float b = stack[--sp];
			const float g = stack[--sp];
			const float r = stack[--sp];
			const float a = stack[--sp];
			sys_io_set_fade(a, r, g, b);
			continue;
		}
		if (op == OP_SCENE_FADE) {
			if (sp < 1) {
				return;
			}
			if (s_last_str >= 0 && s_last_str < 8 && s_str[s_last_str][0]) {
				sys_io_set_fade_pack(pack_io_find_path(s_str[s_last_str]));
			}
			sys_io_scene_fade(stack[--sp]);
			continue;
		}
		if (op == OP_LOADK_S) {
			if (i >= s_tape_n) {
				return;
			}
			const unsigned n = s_tape[i++];
			if (i + n > s_tape_n) {
				return;
			}
			if (s_str_n >= 8) {
				s_str_n = 0;
			}
			unsigned c = 0;
			while (c < n && c < 127) {
				s_str[s_str_n][c] = (char)s_tape[i + c];
				c++;
			}
			s_str[s_str_n][c] = 0;
			i += n;
			s_last_str = s_str_n;
			if (sp < 8) {
				stack[sp++] = (float)s_str_n;
			}
			s_str_n++;
			continue;
		}
		if (op == OP_CHANGE_SCENE) {
			int pack = -1;
			if (s_last_str >= 0 && s_last_str < 8 && s_str[s_last_str][0]) {
				pack = pack_io_find_path(s_str[s_last_str]);
			}
			if (pack < 0 && sp >= 1) {
				pack = (int)stack[--sp];
			}
			if (pack >= 0) {
				pack_io_swap(pack);
				rebind_current_pack();
			}
			continue;
		}
		if (op == OP_INSTANTIATE || op == OP_LOAD_SCENE) {
			int pack = -1;
			if (s_last_str >= 0 && s_last_str < 8 && s_str[s_last_str][0]) {
				pack = pack_io_find_path(s_str[s_last_str]);
			}
			if (pack < 0 && sp >= 1) {
				pack = (int)stack[--sp];
			}
			if (sp < 8) {
				stack[sp++] = pack_io_instantiate(pack) ? (float)pack : -1.0f;
			} else {
				(void)pack_io_instantiate(pack);
			}
			continue;
		}
		if (op == OP_UNLOAD_PACK) {
			int pack = pack_io_current();
			if (s_last_str >= 0 && s_last_str < 8 && s_str[s_last_str][0]) {
				const int found = pack_io_find_path(s_str[s_last_str]);
				if (found >= 0) {
					pack = found;
				}
			}
			pack_io_unload(pack);
			rebind_current_pack();
			continue;
		}
		if (op == OP_SAY_TEXT) {
			if (s_last_str >= 0 && s_last_str < 8) {
				sys_io_set_hud_text(0, s_str[s_last_str]);
			}
			(void)sys_io_say_done();
			continue;
		}
		if (op == OP_SEEK_ANIM) {
			float t = 0.0f;
			if (sp >= 1) {
				t = stack[--sp];
			}
			sys_io_seek_anim(t);
			continue;
		}
		if (op == OP_JMP) {
			if (i + 2 > s_tape_n) {
				return;
			}
			const short off = (short)ru16(s_tape + i);
			i += 2;
			i = (unsigned)((int)i + off);
			continue;
		}
		if (op == OP_JZ) {
			if (i + 2 > s_tape_n || sp < 1) {
				return;
			}
			const short off = (short)ru16(s_tape + i);
			i += 2;
			if (stack[--sp] == 0.0f) {
				i = (unsigned)((int)i + off);
			}
			continue;
		}
		if (op == OP_CALL_NATIVE) {
			if (i >= s_tape_n) {
				return;
			}
			const unsigned char nid = s_tape[i++];
			if (nid == OP_HAS_FEATURE_PS2 && sp < 8) {
				stack[sp++] = 1.0f;
			} else if (nid == OP_OVERLAP) {
				sys_io_overlap_refresh();
				if (sp < 8) {
					stack[sp++] = sys_io_overlaps() ? 1.0f : 0.0f;
				}
			} else if (nid == OP_TILE_AT && sp >= 2) {
				const float y = stack[--sp];
				const float x = stack[--sp];
				stack[sp++] = (float)sys_io_tile_at(x, y);
			} else if (nid == NAT_EE_USED && sp < 8) {
				stack[sp++] = (float)pack_io_ee_used();
			} else if (nid == NAT_EE_FREE && sp < 8) {
				stack[sp++] = (float)pack_io_ee_free();
			} else if (nid == NAT_EE_LIMIT && sp < 8) {
				stack[sp++] = (float)pack_io_ee_limit();
			} else if (nid == NAT_GS_USED && sp < 8) {
				stack[sp++] = (float)pack_io_gs_used();
			} else if (nid == NAT_GS_FREE && sp < 8) {
				stack[sp++] = (float)pack_io_gs_free();
			} else if (nid == NAT_GS_LIMIT && sp < 8) {
				stack[sp++] = (float)pack_io_gs_limit();
			} else if (nid == NAT_PACK_COST_EE && sp < 8) {
				int pack = -1;
				if (s_last_str >= 0 && s_last_str < 8 && s_str[s_last_str][0]) {
					pack = pack_io_find_path(s_str[s_last_str]);
				}
				stack[sp++] = (float)pack_io_cost_ee(pack);
			} else if (nid == NAT_PACK_COST_GS && sp < 8) {
				int pack = -1;
				if (s_last_str >= 0 && s_last_str < 8 && s_str[s_last_str][0]) {
					pack = pack_io_find_path(s_str[s_last_str]);
				}
				stack[sp++] = (float)pack_io_cost_gs(pack);
			} else if ((nid == NAT_CAN_LOAD || nid == NAT_CAN_INSTANTIATE) && sp < 8) {
				int pack = -1;
				if (s_last_str >= 0 && s_last_str < 8 && s_str[s_last_str][0]) {
					pack = pack_io_find_path(s_str[s_last_str]);
				}
				stack[sp++] = (pack >= 0 && pack_io_can_fit(pack)) ? 1.0f : 0.0f;
			} else if (nid == NAT_IS_LOADED && sp < 8) {
				int pack = -1;
				if (s_last_str >= 0 && s_last_str < 8 && s_str[s_last_str][0]) {
					pack = pack_io_find_path(s_str[s_last_str]);
				}
				stack[sp++] = pack_io_is_loaded(pack) ? 1.0f : 0.0f;
			} else if (nid == NAT_LOADED_COUNT && sp < 8) {
				stack[sp++] = (float)pack_io_loaded_count();
			} else if (nid == NAT_SET_CAM && sp >= 1) {
				stack[sp - 1] = sys_io_set_cam((int)stack[sp - 1]) ? 1.0f : 0.0f;
			} else if (nid == NAT_SET_DEFAULT_CAM && sp >= 1) {
				stack[sp - 1] = sys_io_set_default_cam((int)stack[sp - 1]) ? 1.0f : 0.0f;
			} else if (nid == NAT_GET_CAM && sp < 8) {
				stack[sp++] = (float)sys_io_cam_index();
			} else if (nid == NAT_GET_CAM_COUNT && sp < 8) {
				stack[sp++] = (float)sys_io_cam_count();
			} else if (nid == NAT_MAKE_CURRENT) {
				int ok = 0;
				if (s_last_str >= 0 && s_last_str < 8) {
					ok = sys_io_make_current(s_str[s_last_str]);
				}
				if (sp < 8) {
					stack[sp++] = ok ? 1.0f : 0.0f;
				}
			} else if (nid == NAT_PREV_CAM) {
				sys_io_prev_cam();
				if (sp < 8) {
					stack[sp++] = (float)sys_io_cam_index();
				}
			} else if (nid == NAT_GET_DEFAULT_CAM && sp < 8) {
				stack[sp++] = (float)sys_io_default_cam();
			} else if (nid == NAT_LOAD_SPRITES && sp < 8) {
				int pack = pack_io_current();
				if (s_last_str >= 0 && s_last_str < 8 && s_str[s_last_str][0]) {
					pack = pack_io_find_path(s_str[s_last_str]);
				}
				stack[sp++] = sys_io_load_sprites(pack) ? 1.0f : 0.0f;
			} else if (nid == NAT_UNLOAD_SPRITES && sp < 8) {
				stack[sp++] = sys_io_unload_sprites() ? 1.0f : 0.0f;
			} else if (nid == NAT_CAN_LOAD_SPRITES && sp < 8) {
				int pack = pack_io_current();
				if (s_last_str >= 0 && s_last_str < 8 && s_str[s_last_str][0]) {
					pack = pack_io_find_path(s_str[s_last_str]);
				}
				stack[sp++] = (pack_io_is_loaded(pack) || pack_io_can_fit(pack)) ? 1.0f : 0.0f;
			} else if (nid == NAT_IS_SPRITES_LOADED && sp < 8) {
				stack[sp++] = sys_io_sprites_loaded() ? 1.0f : 0.0f;
			} else if (nid == NAT_LOAD_ANIMS && sp < 8) {
				int pack = pack_io_current();
				if (s_last_str >= 0 && s_last_str < 8 && s_str[s_last_str][0]) {
					pack = pack_io_find_path(s_str[s_last_str]);
				}
				stack[sp++] = sys_io_load_anims(pack) ? 1.0f : 0.0f;
			} else if (nid == NAT_UNLOAD_ANIMS && sp < 8) {
				stack[sp++] = sys_io_unload_anims() ? 1.0f : 0.0f;
			} else if (nid == NAT_IS_ANIMS_LOADED && sp < 8) {
				stack[sp++] = sys_io_anims_loaded() ? 1.0f : 0.0f;
			} else if (nid == NAT_LOAD_HITS && sp < 8) {
				int pack = pack_io_current();
				if (s_last_str >= 0 && s_last_str < 8 && s_str[s_last_str][0]) {
					pack = pack_io_find_path(s_str[s_last_str]);
				}
				stack[sp++] = sys_io_load_hits(pack) ? 1.0f : 0.0f;
			} else if (nid == NAT_UNLOAD_HITS && sp < 8) {
				stack[sp++] = sys_io_unload_hits() ? 1.0f : 0.0f;
			} else if (nid == NAT_IS_HITS_LOADED && sp < 8) {
				stack[sp++] = sys_io_hits_loaded() ? 1.0f : 0.0f;
			} else if (nid == NAT_LOAD_TILES && sp < 8) {
				int pack = pack_io_current();
				if (s_last_str >= 0 && s_last_str < 8 && s_str[s_last_str][0]) {
					pack = pack_io_find_path(s_str[s_last_str]);
				}
				stack[sp++] = sys_io_load_tiles(pack) ? 1.0f : 0.0f;
			} else if (nid == NAT_MOVE_PLANAR && sp >= 3) {
				const float speed = stack[--sp];
				const float ay = stack[--sp];
				const float ax = stack[--sp];
				sys_io_move_planar(ax, ay, speed, delta);
				if (sp < 8) {
					stack[sp++] = 1.0f;
				}
			} else if (nid == NAT_FOLLOW_NODE && sp >= 3) {
				const float speed = stack[--sp];
				const float tz = stack[--sp];
				const float tx = stack[--sp];
				sys_io_follow_node(tx, tz, speed, delta);
				if (sp < 8) {
					stack[sp++] = 1.0f;
				}
			} else if (nid == NAT_HURT && sp >= 4) {
				const float vz = stack[--sp];
				const float vy = stack[--sp];
				const float vx = stack[--sp];
				const int amt = (int)stack[--sp];
				if (sp < 8) {
					stack[sp++] = (float)sys_io_hurt(amt, vx, vy, vz);
				}
			} else if (nid == NAT_SET_HITSTOP && sp >= 1) {
				sys_io_set_hitstop(stack[--sp]);
				if (sp < 8) {
					stack[sp++] = 1.0f;
				}
			} else if (nid == NAT_SET_INVULN && sp >= 1) {
				sys_io_set_invuln(stack[--sp]);
				if (sp < 8) {
					stack[sp++] = 1.0f;
				}
			} else if (nid == NAT_IS_INVULN && sp < 8) {
				stack[sp++] = sys_io_is_invuln() ? 1.0f : 0.0f;
			} else if (nid == NAT_KNOCKBACK && sp >= 3) {
				const float vz = stack[--sp];
				const float vy = stack[--sp];
				const float vx = stack[--sp];
				sys_io_knockback(vx, vy, vz);
				if (sp < 8) {
					stack[sp++] = 1.0f;
				}
			} else if (nid == NAT_SET_HP && sp >= 1) {
				stack[sp - 1] = (float)sys_io_set_hp((int)stack[sp - 1]);
			} else if (nid == NAT_GET_HP && sp < 8) {
				stack[sp++] = (float)sys_io_get_hp();
			} else if (nid == NAT_HITBOX_ON && sp >= 2) {
				const int on = (int)stack[--sp];
				const int node = (int)stack[--sp];
				if (sp < 8) {
					stack[sp++] = sys_io_set_hitbox_enabled(node, on) ? 1.0f : 0.0f;
				}
			} else if (nid == NAT_HITBOX_LAYER && sp >= 2) {
				const int layer = (int)stack[--sp];
				const int node = (int)stack[--sp];
				if (sp < 8) {
					stack[sp++] = sys_io_set_hitbox_layer(node, layer) ? 1.0f : 0.0f;
				}
			} else if (nid == NAT_GET_PRESSURE && sp >= 1) {
				stack[sp - 1] = (float)pad_io_get_pressure((int)stack[sp - 1]);
			} else if (nid == NAT_SET_DEADZONE && sp >= 1) {
				pad_io_set_deadzone(stack[--sp]);
				if (sp < 8) {
					stack[sp++] = 1.0f;
				}
			} else if (nid == NAT_GET_STICK && sp >= 1) {
				float sx = 0.0f, sy = 0.0f;
				pad_io_stick((int)stack[--sp], &sx, &sy);
				if (sp < 8) {
					stack[sp++] = sx;
				}
			} else if (nid == NAT_POKE && sp >= 2) {
				const unsigned char v = (unsigned char)stack[--sp];
				const unsigned off = (unsigned)stack[--sp];
				if (sp < 8) {
					stack[sp++] = pack_io_poke(off, v) ? 1.0f : 0.0f;
				}
			} else if (nid == NAT_PEEK && sp >= 1) {
				stack[sp - 1] = (float)pack_io_peek((unsigned)stack[sp - 1]);
			} else if (nid == NAT_SPAWN) {
				int pack = -1;
				if (s_last_str >= 0 && s_last_str < 8 && s_str[s_last_str][0]) {
					pack = pack_io_find_path(s_str[s_last_str]);
				}
				const int ok = pack_io_instantiate(pack);
				if (sp >= 3) {
					const float z = stack[--sp];
					const float y = stack[--sp];
					const float x = stack[--sp];
					if (ok) {
						sys_io_spawn_ofs(x, y, z);
					}
				}
				if (sp < 8) {
					stack[sp++] = ok ? 1.0f : 0.0f;
				}
			} else if (nid == NAT_OVERLAPS_ENTERED && sp < 8) {
				sys_io_overlap_refresh();
				stack[sp++] = sys_io_overlaps_entered() ? 1.0f : 0.0f;
			} else if (nid == NAT_SET_FLIP && sp >= 1) {
				sys_io_set_flip((int)stack[--sp]);
				if (sp < 8) {
					stack[sp++] = 1.0f;
				}
			} else if (nid == NAT_MOVE_6DOF && sp >= 7) {
				const float speed = stack[--sp];
				const float roll = stack[--sp];
				const float yaw = stack[--sp];
				const float pitch = stack[--sp];
				const float az = stack[--sp];
				const float ay = stack[--sp];
				const float ax = stack[--sp];
				sys_io_move_6dof(ax, ay, az, pitch, yaw, roll, speed, delta);
				if (sp < 8) {
					stack[sp++] = 1.0f;
				}
			} else if (nid == NAT_LOAD_PARTICLES && sp < 8) {
				int pack = pack_io_current();
				if (s_last_str >= 0 && s_last_str < 8 && s_str[s_last_str][0]) {
					pack = pack_io_find_path(s_str[s_last_str]);
				}
				stack[sp++] = sys_io_load_particles(pack) ? 1.0f : 0.0f;
			} else if (nid == NAT_UNLOAD_PARTICLES && sp < 8) {
				stack[sp++] = sys_io_unload_particles() ? 1.0f : 0.0f;
			} else if (nid == NAT_CAN_LOAD_PARTICLES && sp < 8) {
				int pack = pack_io_current();
				if (s_last_str >= 0 && s_last_str < 8 && s_str[s_last_str][0]) {
					pack = pack_io_find_path(s_str[s_last_str]);
				}
				stack[sp++] = (pack_io_is_loaded(pack) || pack_io_can_fit(pack)) ? 1.0f : 0.0f;
			} else if (nid == NAT_IS_PARTICLES_LOADED && sp < 8) {
				stack[sp++] = sys_io_particles_loaded() ? 1.0f : 0.0f;
			} else if (nid == NAT_PATH_FOLLOW && sp >= 1) {
				sys_io_path_follow(stack[--sp], delta);
				if (sp < 8) {
					stack[sp++] = 1.0f;
				}
			} else if (nid == NAT_SET_PATH_OFFSET && sp >= 1) {
				sys_io_set_path_offset(stack[--sp]);
				if (sp < 8) {
					stack[sp++] = 1.0f;
				}
			} else if (nid == NAT_SET_THRUST && sp >= 1) {
				sys_io_set_thrust(stack[--sp]);
				if (sp < 8) {
					stack[sp++] = 1.0f;
				}
			} else if (nid == NAT_SET_STEER && sp >= 1) {
				sys_io_set_steer(stack[--sp]);
				if (sp < 8) {
					stack[sp++] = 1.0f;
				}
			} else if (nid == NAT_SET_EYE_HEIGHT && sp >= 1) {
				sys_io_set_eye_height(stack[--sp]);
				if (sp < 8) {
					stack[sp++] = 1.0f;
				}
			} else if (nid == NAT_PREFETCH_SCENE && sp < 8) {
				int pack = pack_io_current();
				if (s_last_str >= 0 && s_last_str < 8 && s_str[s_last_str][0]) {
					pack = pack_io_find_path(s_str[s_last_str]);
				}
				stack[sp++] = sys_io_prefetch(pack) ? 1.0f : 0.0f;
			} else if (nid == NAT_NAVMESH && sp < 8) {
				stack[sp++] = (float)sys_io_navmesh_next(0, 1);
			} else if (nid == NAT_TWEEN_PLAY && sp < 8) {
				stack[sp++] = (float)sys_io_tween_start(0.0f, 1.0f, 0.5f, 0);
			} else if (nid == NAT_TWEEN_KILL && sp < 8) {
				sys_io_kill_tweens();
				stack[sp++] = 1.0f;
			} else if (nid == NAT_UNLOAD_TILES && sp < 8) {
				stack[sp++] = sys_io_unload_tiles() ? 1.0f : 0.0f;
			} else if (nid == NAT_LOAD_HUD && sp < 8) {
				const int pack = resolve_pack();
				stack[sp++] = sys_io_load_hud(pack) ? 1.0f : 0.0f;
			} else if (nid == NAT_UNLOAD_HUD && sp < 8) {
				stack[sp++] = sys_io_unload_hud() ? 1.0f : 0.0f;
			} else if (nid == NAT_CAN_LOAD_HUD && sp < 8) {
				const int pack = resolve_pack();
				stack[sp++] = (pack_io_is_loaded(pack) || pack_io_can_fit(pack)) ? 1.0f : 0.0f;
			} else if (nid == NAT_IS_HUD_LOADED && sp < 8) {
				stack[sp++] = sys_io_hud_loaded() ? 1.0f : 0.0f;
			} else if (nid == NAT_LOAD_MUSIC && sp < 8) {
				const int pack = resolve_pack();
				if (pack > 0 && !pack_io_is_loaded(pack) && !pack_io_can_fit(pack)) {
					stack[sp++] = 0.0f;
				} else {
					stack[sp++] = sfx_io_music_load(pack) ? 1.0f : 0.0f;
				}
			} else if (nid == NAT_UNLOAD_MUSIC && sp < 8) {
				sfx_io_music_unload();
				stack[sp++] = 1.0f;
			} else if (nid == NAT_CAN_LOAD_MUSIC && sp < 8) {
				const int pack = resolve_pack();
				stack[sp++] = (pack <= 0 || pack_io_is_loaded(pack) || pack_io_can_fit(pack)) ? 1.0f : 0.0f;
			} else if (nid == NAT_IS_MUSIC_LOADED && sp < 8) {
				stack[sp++] = sfx_io_music_loaded() ? 1.0f : 0.0f;
			} else if (nid == NAT_PLAY_ANIM && sp < 8) {
				int idx = 0;
				if (s_last_str >= 0 && s_last_str < 8 && s_str[s_last_str][0]) {
					idx = s_str[s_last_str][0];
				} else if (sp >= 1) {
					idx = (int)stack[--sp];
				}
				sys_io_play_anim(idx);
				stack[sp++] = 1.0f;
			} else if (nid == NAT_STOP_ANIM && sp < 8) {
				sys_io_stop_anim();
				stack[sp++] = 1.0f;
			} else if (nid == NAT_SET_CHECKPOINT && sp < 8) {
				do_checkpoint();
				stack[sp++] = 1.0f;
			} else if (nid == NAT_RESPAWN && sp < 8) {
				do_respawn();
				stack[sp++] = 1.0f;
			} else if (sp < 8) {
				stack[sp++] = (float)sys_io_get_hp();
			}
			continue;
		}
	}
}

int script_vm_init(const unsigned char *scrp, unsigned scrp_sz,
		const unsigned char *node, unsigned node_sz)
{
	s_ready = 0;
	s_tape = 0;
	s_tape_n = 0;
	s_process_off = 0;
	s_kit_spawn = 0;
	s_kit_player = 0;
	s_kit_portal = 0;
	s_kit_checkpoint = 0;
	s_kit_hazard = 0;
	s_kit_pickup = 0;
	s_kit_music = 0;
	s_kit_music_on = 0;
	s_kit_unload = 0;
	s_kit_load = 0;
	s_kit_talk = 0;
	s_kit_spawner = 0;
	s_kit_save = 0;
	s_kit_camvol = 0;
	s_kit_follow = 0;
	s_follow_speed = 4.0f;
	s_spawn_x = 0.0f;
	s_spawn_y = 0.0f;
	s_spawn_z = 0.0f;
	s_spawn_ok = 0;
	s_chk_x = 0.0f;
	s_chk_y = 0.0f;
	s_chk_z = 0.0f;
	s_chk_pack = 0;
	s_chk_ok = 0;
	s_grav_vy = 0.0f;
	s_kit_spawn_path[0] = 0;
	s_kit_portal_path[0] = 0;
	s_kit_load_path[0] = 0;
	s_kit_unload_path[0] = 0;
	s_kit_talk_line[0] = 0;
	s_kit_cam_index = 0;
	s_kit_save_slot = 0;
	s_kit_hurt_amt = 1;
	s_kit_load_handle = -1;
	s_did_slide = 0;
	s_yaw = 0.0f;
	s_str_n = 0;
	s_last_str = -1;
	memset(s_str, 0, sizeof(s_str));
	bind_nodes(node, node_sz);
	try_bind_node_pack(1);
	try_bind_node_pack(2);
	if (s_spawn_ok) {
		sys_io_spawn_ofs(s_spawn_x, s_spawn_y, s_spawn_z);
		s_chk_x = s_spawn_x;
		s_chk_y = s_spawn_y;
		s_chk_z = s_spawn_z;
		s_chk_pack = pack_io_current();
		s_chk_ok = 1;
	}
	if (!scrp || scrp_sz < 12) {
		return 0;
	}
	if (scrp[0] != 'S' || scrp[1] != 'C' || scrp[2] != 'R' || scrp[3] != 'P') {
		return 0;
	}
	if (ru16(scrp + 4) != BLAZIUM_PS2_COOK_ABI) {
		return 0;
	}
	const unsigned tape_n = ru32(scrp + 8);
	if (12 + tape_n > scrp_sz) {
		return 0;
	}
	/* Flag bit 0: a GDIR program (compiled scripts) follows the tape and
	 * drives the scene; the keyword tape is then only a summary. */
	if ((ru16(scrp + 6) & 1) && gdir_vm_init(scrp + 12 + tape_n, scrp_sz - 12 - tape_n)) {
		s_tape = 0;
		s_tape_n = 0;
		s_ready = 1;
		return 1;
	}
	s_tape = scrp + 12;
	s_tape_n = tape_n;
	s_process_off = 0;
	for (unsigned i = 0; i < s_tape_n; i++) {
		if (s_tape[i] == OP_PROCESS) {
			s_process_off = i + 1;
			break;
		}
		if (s_tape[i] == OP_LOADK_F) {
			i += 4;
		} else if (s_tape[i] == OP_JMP || s_tape[i] == OP_JZ) {
			i += 2;
		} else if (s_tape[i] == OP_CALL_NATIVE) {
			i += 1;
		} else if (s_tape[i] == OP_LOADK_S && i + 1 < s_tape_n) {
			i += 1 + s_tape[i + 1];
		}
	}
	run_range(0, s_tape_n, 0.0f);
	s_ready = 1;
	return 1;
}

void script_vm_process(float delta)
{
	s_did_slide = 0;
	if (sys_io_hud_just_accept()) {
		run_hud_action(sys_io_hud_action());
	}
	if (sys_io_paused()) {
		if (s_ready && s_tape) {
			sfx_io_tick(delta);
		}
		return;
	}
	if (s_ready && s_tape) {
		run_range(s_process_off, s_tape_n, delta);
	}
	kit_tick();
	if (s_kit_player) {
		s_grav_vy -= 18.0f * delta;
		if (s_grav_vy < -24.0f) {
			s_grav_vy = -24.0f;
		}
		if (sys_io_on_floor() && s_grav_vy < 0.0f) {
			s_grav_vy = 0.0f;
		}
		if (pad_io_just_pressed(4) && sys_io_on_floor() && sys_io_btn_count() == 0) {
			s_grav_vy = 8.0f;
		}
		float sx = 0.0f;
		float sy = 0.0f;
		if (!s_did_slide) {
			pad_io_stick(0, &sx, &sy);
		}
		sys_io_slide(sx * 4.0f, s_grav_vy, sy * 4.0f, delta, 255);
	}
	if (s_kit_follow) {
		float px = 0.0f;
		float py = 0.0f;
		float pz = 0.0f;
		sys_io_get_pos(&px, &py, &pz);
		(void)py;
		sys_io_attach(0.0f, 1.6f, s_follow_speed);
		sys_io_follow_node(px, pz, s_follow_speed, delta);
	}
	if (s_ready && s_tape) {
		sfx_io_tick(delta);
	}
}

int script_vm_ready(void)
{
	return s_ready;
}

int script_vm_kit_spawn(void)
{
	return s_kit_spawn;
}

int script_vm_kit_player(void)
{
	return s_kit_player;
}

int script_vm_kit_portal(void)
{
	return s_kit_portal;
}

int script_vm_kit_checkpoint(void)
{
	return s_kit_checkpoint;
}

int script_vm_kit_hazard(void)
{
	return s_kit_hazard;
}

int script_vm_kit_pickup(void)
{
	return s_kit_pickup;
}

int script_vm_kit_load(void)
{
	return s_kit_load;
}

int script_vm_kit_save(void)
{
	return s_kit_save;
}
