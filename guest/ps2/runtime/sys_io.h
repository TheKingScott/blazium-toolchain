// MIT. Load CAM/HIT/HUD/TILE/NAV/ANIM/SPRT/FMV ABI 1 sidecars from host: then cdrom0:.

#ifndef BLAZIUM_PS2_SYS_IO_H
#define BLAZIUM_PS2_SYS_IO_H

#ifdef __cplusplus
extern "C" {
#endif

int sys_io_init(void);
int sys_io_load_pack(int pack);
int sys_io_loaded(void);
int sys_io_cam_count(void);
int sys_io_hit_count(void);
int sys_io_hud_count(void);
int sys_io_tile_count(void);
int sys_io_nav_count(void);
int sys_io_anim_count(void);
int sys_io_sprite_count(void);
int sys_io_fmv_count(void);
const char *sys_io_fmv_error(void);

void sys_io_say(const char *line);
int sys_io_say_done(void);
void sys_io_set_hud_text(int slot, const char *text);
void sys_io_nav_follow(float speed, float delta);
int sys_io_nav_next(int from, int to);
int sys_io_set_hp(int hp);
int sys_io_get_hp(void);
void sys_io_set_frame(int i);
void sys_io_seek_anim(float sec);
void sys_io_set_anim_speed(float s);
void sys_io_play_anim(int name_or_index);
void sys_io_stop_anim(void);
int sys_io_play_fmv(void);
void sys_io_tick(float delta);
int sys_io_hit_hazard(float x, float z);
void sys_io_look(float yaw, float pitch, float roll);
void sys_io_look_stick(float delta);
void sys_io_orbit(float yaw, float pitch, float dist);
void sys_io_attach(float ox, float oy, float oz);
void sys_io_shake(float amp, float ms);
void sys_io_next_cam(void);
void sys_io_prev_cam(void);
int sys_io_set_cam(int i);
int sys_io_set_cam_name(const char *name);
int sys_io_set_default_cam(int i);
int sys_io_default_cam(void);
int sys_io_make_current(const char *name);
int sys_io_cam_index(void);
int sys_io_tween_start(float from, float to, float sec, int kind);
void sys_io_kill_tweens(void);
int sys_io_tween_count(void);
int sys_io_tween_done(int id);
int sys_io_timer_start(float sec);
int sys_io_timer_done(int id);
void sys_io_slide(float vx, float vy, float vz, float delta, int mask);
void sys_io_move_planar(float ax, float ay, float speed, float delta);
void sys_io_follow_node(float tx, float tz, float speed, float delta);
int sys_io_on_floor(void);
int sys_io_on_wall(void);
int sys_io_on_ceiling(void);
void sys_io_overlap_refresh(void);
int sys_io_overlaps(void);
int sys_io_overlaps_entered(void);
int sys_io_has_overlapping(void);
int sys_io_hitbox_kind(void);
int sys_io_hitbox_kit(void);
int sys_io_entered_kit_count(void);
int sys_io_entered_kit_at(int i);
int sys_io_left_kit_count(void);
int sys_io_left_kit_at(int i);
int sys_io_disable_entered_kit(int kit);
int sys_io_set_hitbox_enabled(int node, int on);
int sys_io_set_hitbox_layer(int node, int layer);
int sys_io_get_hitbox_layer(int node);
int sys_io_raycast(float ox, float oy, float oz, float dx, float dy, float dz, float dist, int mask);
void sys_io_ray_point(float *x, float *y, float *z);
int sys_io_tile_solid_at(float x, float y);
int sys_io_tile_at(float x, float y);
int sys_io_set_cell(int x, int y, int flags);
int sys_io_erase_cell(int x, int y);
void sys_io_set_fade(float alpha, float r, float g, float b);
void sys_io_scene_fade(float sec);
void sys_io_set_fade_pack(int pack);
int sys_io_hurt(int amount, float vx, float vy, float vz);
void sys_io_set_hitstop(float ms);
void sys_io_set_invuln(float ms);
int sys_io_is_invuln(void);
void sys_io_knockback(float vx, float vy, float vz);
void sys_io_set_flip(int flip);
int sys_io_load_sprites(int pack);
int sys_io_unload_sprites(void);
int sys_io_sprites_loaded(void);
int sys_io_load_anims(int pack);
int sys_io_unload_anims(void);
int sys_io_anims_loaded(void);
int sys_io_load_hits(int pack);
int sys_io_unload_hits(void);
int sys_io_hits_loaded(void);
int sys_io_load_tiles(int pack);
int sys_io_unload_tiles(void);
int sys_io_tiles_loaded(void);
int sys_io_load_hud(int pack);
int sys_io_unload_hud(void);
int sys_io_hud_loaded(void);
void sys_io_spawn_ofs(float x, float y, float z);
void sys_io_get_pos(float *x, float *y, float *z);
int sys_io_paused(void);
void sys_io_set_paused(int on);
int sys_io_btn_count(void);
int sys_io_hud_just_accept(void);
const char *sys_io_hud_action(void);
void sys_io_move_6dof(float ax, float ay, float az, float pitch, float yaw, float roll, float speed, float delta);
void sys_io_set_steer(float steer);
void sys_io_set_thrust(float thrust);
void sys_io_set_eye_height(float h);
int sys_io_load_particles(int pack);
int sys_io_unload_particles(void);
int sys_io_particles_loaded(void);
void sys_io_path_follow(float speed, float delta);
void sys_io_set_path_offset(float t);
int sys_io_navmesh_next(int from, int to);
int sys_io_prefetch(int pack);

// HUD rows by slot (GDIR scripts: Label.text / visible / position, Button focus).
void sys_io_hud_set_hidden(int slot, int hidden);
int sys_io_hud_hidden(int slot);
void sys_io_hud_set_pos(int slot, int x, int y);
void sys_io_hud_get_pos(int slot, int *x, int *y);
void sys_io_hud_set_color(int slot, int r, int g, int b);
const char *sys_io_hud_text(int slot);
int sys_io_hud_focus(void);
void sys_io_hud_set_focus(int slot);

#ifdef __cplusplus
}
#endif

#endif
