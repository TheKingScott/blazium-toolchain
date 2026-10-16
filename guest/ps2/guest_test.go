package guest

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestInstallWritesRuntime(t *testing.T) {
	dir := t.TempDir()
	if err := Install(dir); err != nil {
		t.Fatal(err)
	}
	if CookABI != 1 {
		t.Fatalf("CookABI %d", CookABI)
	}
	for _, name := range RuntimeNames {
		if _, err := os.Stat(filepath.Join(dir, name)); err != nil {
			t.Fatalf("missing %s: %v", name, err)
		}
	}
}

func TestEmbeddedRuntimeComplete(t *testing.T) {
	for _, name := range RuntimeNames {
		if _, err := files.ReadFile("runtime/" + name); err != nil {
			t.Fatalf("embed missing %s: %v", name, err)
		}
	}
}

func TestRuntimeNamesHasVU1(t *testing.T) {
	var cpp, hdr, vsm bool
	for _, name := range RuntimeNames {
		switch name {
		case "vu1_draw.cpp":
			cpp = true
		case "vu1_draw.h":
			hdr = true
		case "draw_3D.vsm":
			vsm = true
		}
	}
	if !cpp || !hdr || !vsm {
		t.Fatalf("RuntimeNames missing VU1 files: %v", RuntimeNames)
	}
}

func TestGuestPack1AndDisp(t *testing.T) {
	pack, err := files.ReadFile("runtime/pack_io.cpp")
	if err != nil {
		t.Fatal(err)
	}
	src := string(pack)
	if (!strings.Contains(src, "MESH01") && !strings.Contains(src, "MESH%02d")) || !strings.Contains(src, "pack_io_swap") {
		t.Fatalf("%s", "pack_io.cpp must load MESH%02d / MESH01 and expose pack_io_swap")
	}
	if !strings.Contains(src, "STREAM") {
		t.Fatal("pack_io.cpp must read STREAM.bin for extra packs")
	}
	if !strings.Contains(src, "pack_io_find_path") || !strings.Contains(src, "s_paths") {
		t.Fatal("pack_io.cpp must map STREAM res:// paths")
	}
	if strings.Contains(src, "s_res[4]") || strings.Contains(src, "s_res_n >= 4") {
		t.Fatal("pack_io.cpp must not use a hard 4-resident cap")
	}
	if !strings.Contains(src, "PS2_LAYERS") || !strings.Contains(src, "s_cost_ee") || !strings.Contains(src, "pack_io_can_fit") {
		t.Fatal("pack_io.cpp must RAM-gate 16 layers with STREAM ee/gs costs")
	}
	if !strings.Contains(src, "PS2_EXTRA_EE_CAP") || !strings.Contains(src, "pack_io_prefetch") {
		t.Fatal("pack_io.cpp must cap extra packs at 2 MiB and prefetch host then cdrom0")
	}
	main, err := files.ReadFile("runtime/main.cpp")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(main), "DISP.bin") {
		t.Fatal("main.cpp must read DISP.bin for region/width")
	}
	if !strings.Contains(string(main), "GS_ZBUF_32") {
		t.Fatal("main.cpp must use a 32-bit Z buffer so nearer walls occlude far ones")
	}
	sfx, err := files.ReadFile("runtime/sfx_io.cpp")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(sfx), "sfx_io_audible") || !strings.Contains(string(sfx), "rSdInit") || !strings.Contains(string(sfx), "sce_SDR_DEV") {
		t.Fatal("sfx_io.cpp must use sdrdrv RPC (rSdInit / sce_SDR_DEV) and expose sfx_io_audible")
	}
	if strings.Contains(string(sfx), "#include <libsd.h>") || strings.Contains(string(sfx), "#include <libsdr.h>") || strings.Contains(string(sfx), "sceSdInit(") || strings.Contains(string(sfx), "sceSdRemote(") {
		t.Fatal("sfx_io.cpp must not include libsd.h/libsdr.h or call sceSdInit/sceSdRemote")
	}
	if !strings.Contains(string(sfx), "sdrdrv") && !strings.Contains(string(sfx), "SDRDRV") {
		t.Fatal("sfx_io.cpp must load sdrdrv IRX for the libsdr RPC server")
	}
	if !strings.Contains(string(sfx), "MUSIC00") || !strings.Contains(string(sfx), "sfx_io_music_play") {
		t.Fatal("sfx_io.cpp must load MUSIC00 and expose sfx_io_music_play")
	}
	if !strings.Contains(string(sfx), "SFX%02d") || !strings.Contains(string(sfx), "sfx_io_load") || !strings.Contains(string(sfx), "sfx_io_unload") {
		t.Fatalf("%s", "sfx_io.cpp must load SFX%02d.bin and expose sfx_io_load/unload")
	}
	if !strings.Contains(string(sfx), "ADPCM_LOOP") || !strings.Contains(string(sfx), "0x10000") {
		t.Fatal("sfx_io.cpp must loop music VAG on SPU addr 0x10000")
	}
}

func TestGuestInputAndUserIO(t *testing.T) {
	pad, err := files.ReadFile("runtime/pad_io.h")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(pad), "pad_io_pressed") || !strings.Contains(string(pad), "pad_io_stick") {
		t.Fatal("pad_io.h must expose Input action/stick queries")
	}
	if !strings.Contains(string(pad), "pad_io_set_deadzone") {
		t.Fatal("pad_io.h must expose stick deadzone")
	}
	pack, err := files.ReadFile("runtime/pack_io.h")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(pack), "pack_io_user_save") || !strings.Contains(string(pack), "pack_io_user_error") {
		t.Fatal("pack_io.h must map user:// with a named error")
	}
	if !strings.Contains(string(pack), "pack_io_poke") || !strings.Contains(string(pack), "pack_io_peek") {
		t.Fatal("pack_io.h must expose 24KiB poke/peek")
	}
	vm, err := files.ReadFile("runtime/script_vm.cpp")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(vm), "OP_INPUT_TRANSLATE") || !strings.Contains(string(vm), "OP_USER_SAVE") {
		t.Fatal("script_vm must interpret Input translate and user:// save")
	}
	if !strings.Contains(string(vm), "OP_KIT_TICK") || !strings.Contains(string(vm), "Checkpoint") {
		t.Fatal("script_vm must interpret kit tick and Checkpoint")
	}
	if !strings.Contains(string(vm), "sys_io_overlap_refresh") || !strings.Contains(string(vm), "sys_io_entered_kit") {
		t.Fatal("script_vm must fire kit on HIT enter (overlap kit, not Cross-only)")
	}
	if !strings.Contains(string(vm), "s_kit_player") || !strings.Contains(string(vm), "pad_io_stick") {
		t.Fatal("script_vm must auto-slide CharacterBody kit 4 from stick")
	}
	if !strings.Contains(string(vm), "sys_io_spawn_ofs") || !strings.Contains(string(vm), "do_checkpoint") {
		t.Fatal("script_vm must teleport spawn_ofs and store checkpoint xyz")
	}
	if !strings.Contains(string(vm), "NAT_SET_CHECKPOINT") || !strings.Contains(string(vm), "NAT_RESPAWN") {
		t.Fatal("script_vm must interpret set_checkpoint / respawn natives")
	}
	if !strings.Contains(string(vm), "s_grav_vy") || !strings.Contains(string(vm), "s_kit_follow") {
		t.Fatal("script_vm must apply gravity and kit 14 follow")
	}
	if !strings.Contains(string(vm), "OP_SAY") || !strings.Contains(string(vm), "OP_PLAY_FMV") {
		t.Fatal("script_vm must interpret say and play_fmv")
	}
	if !strings.Contains(string(vm), "OP_PLAY_MUSIC") || !strings.Contains(string(vm), "OP_MUSIC_VOL") {
		t.Fatal("script_vm must interpret load_music / set_music_volume")
	}
	if !strings.Contains(string(vm), "OP_LOOK_STICK") || !strings.Contains(string(vm), "OP_SHAKE_CAMERA") {
		t.Fatal("script_vm must interpret look_camera / shake_camera")
	}
	if !strings.Contains(string(vm), "OP_TWEEN") || !strings.Contains(string(vm), "OP_TIMER") {
		t.Fatal("script_vm must interpret create_tween / create_timer")
	}
	if !strings.Contains(string(vm), "OP_SLIDE") || !strings.Contains(string(vm), "sys_io_slide") {
		t.Fatal("script_vm must interpret move_and_slide via sys_io_slide")
	}
	if !strings.Contains(string(vm), "OP_OVERLAP") || !strings.Contains(string(vm), "OP_RAYCAST") || !strings.Contains(string(vm), "OP_TILE_AT") {
		t.Fatal("script_vm must interpret overlaps / raycast / tile_solid_at")
	}
	if !strings.Contains(string(vm), "OP_SET_FADE") || !strings.Contains(string(vm), "OP_SCENE_FADE") {
		t.Fatal("script_vm must interpret set_fade / change_scene_fade")
	}
	if !strings.Contains(string(vm), "OP_CHANGE_SCENE") || !strings.Contains(string(vm), "OP_INSTANTIATE") {
		t.Fatal("script_vm must interpret change_scene / instantiate")
	}
	if !strings.Contains(string(vm), "OP_LOAD_SCENE") || !strings.Contains(string(vm), "NAT_CAN_INSTANTIATE") {
		t.Fatal("script_vm must interpret additive load_scene and can_instantiate natives")
	}
	if !strings.Contains(string(vm), "NAT_SET_CAM") || !strings.Contains(string(vm), "sys_io_set_cam") {
		t.Fatal("script_vm must interpret set_camera natives that push")
	}
	if !strings.Contains(string(vm), "NAT_LOAD_SPRITES") || !strings.Contains(string(vm), "NAT_MOVE_PLANAR") {
		t.Fatal("script_vm must interpret load_sprites / move_planar")
	}
	if !strings.Contains(string(vm), "NAT_HURT") || !strings.Contains(string(vm), "NAT_POKE") || !strings.Contains(string(vm), "NAT_GET_PRESSURE") {
		t.Fatal("script_vm must interpret hurt / poke / get_pressure")
	}
	if !strings.Contains(string(vm), "NAT_MOVE_6DOF") || !strings.Contains(string(vm), "NAT_LOAD_PARTICLES") || !strings.Contains(string(vm), "NAT_PATH_FOLLOW") {
		t.Fatal("script_vm must interpret move_6dof / load_particles / path_follow")
	}
	if !strings.Contains(string(vm), "OP_JMP") || !strings.Contains(string(vm), "OP_CALL_NATIVE") {
		t.Fatal("script_vm must interpret JMP / CALL_NATIVE")
	}
	if !strings.Contains(string(vm), "pack_io_find_path") {
		t.Fatal("script_vm must resolve STREAM paths")
	}
}

func TestGuestCameraLookAndVu1Fallback(t *testing.T) {
	gs, err := files.ReadFile("runtime/gs_draw.cpp")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(gs), "gs_draw_set_camera") || !strings.Contains(string(gs), "euler_yxz_negz") {
		t.Fatal("gs_draw.cpp must apply Camera3D look-at from YXZ euler")
	}
	if !strings.Contains(string(gs), "gs_draw_set_ortho") || !strings.Contains(string(gs), "ortho_proj") {
		t.Fatal("gs_draw.cpp must support Camera2D ortho")
	}
	if !strings.Contains(string(gs), "gs_draw_look") || !strings.Contains(string(gs), "gs_draw_orbit_sph") || !strings.Contains(string(gs), "gs_draw_shake") {
		t.Fatal("gs_draw.cpp must expose look/orbit/shake")
	}
	if !strings.Contains(string(gs), "g_fov") {
		t.Fatal("gs_draw.cpp must use cooked FOV instead of a hardcoded 55")
	}
	sys, err := files.ReadFile("runtime/sys_io.cpp")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(sys), "gs_draw_set_camera") {
		t.Fatal("sys_io.cpp must feed CAM00 euler+fov into gs_draw_set_camera")
	}
	if !strings.Contains(string(sys), "sys_io_look") || !strings.Contains(string(sys), "sys_io_next_cam") {
		t.Fatal("sys_io.cpp must expose look_camera / next_camera")
	}
	if !strings.Contains(string(sys), "sys_io_tween_start") || !strings.Contains(string(sys), "sys_io_timer_start") {
		t.Fatal("sys_io.cpp must expose linear tweens and timers")
	}
	if !strings.Contains(string(sys), "sys_io_slide") || !strings.Contains(string(sys), "enters_hit") {
		t.Fatal("sys_io.cpp must slide against HIT00 AABBs")
	}
	if !strings.Contains(string(sys), "sys_io_overlaps") || !strings.Contains(string(sys), "sys_io_raycast") || !strings.Contains(string(sys), "sys_io_tile_solid_at") {
		t.Fatal("sys_io.cpp must query HIT overlaps, raycast, and TILE cells")
	}
	if !strings.Contains(string(sys), "sys_io_set_fade") || !strings.Contains(string(sys), "sys_io_scene_fade") {
		t.Fatal("sys_io.cpp must expose set_fade / change_scene_fade")
	}
	if !strings.Contains(string(sys), "sys_io_set_fade_pack") || !strings.Contains(string(sys), "pack_io_swap") {
		t.Fatal("sys_io.cpp must fade then swap packs")
	}
	if !strings.Contains(string(sys), "sys_io_set_cam") || !strings.Contains(string(sys), "sys_io_set_default_cam") {
		t.Fatal("sys_io.cpp must expose set_camera / set_default_camera")
	}
	if !strings.Contains(string(sys), "sys_io_move_planar") || !strings.Contains(string(sys), "sys_io_hurt") {
		t.Fatal("sys_io.cpp must expose move_planar / hurt")
	}
	if !strings.Contains(string(sys), "SPRT") || !strings.Contains(string(sys), "apply_sprt") {
		t.Fatal("sys_io.cpp must load SPRT sidecars")
	}
	if !strings.Contains(string(sys), "sys_io_move_6dof") || !strings.Contains(string(sys), "sys_io_path_follow") {
		t.Fatal("sys_io.cpp must expose move_6dof / path_follow")
	}
	if !strings.Contains(string(sys), "apply_part") || !strings.Contains(string(sys), "PART") {
		t.Fatal("sys_io.cpp must load PART sidecars")
	}
	if !strings.Contains(string(sys), "apply_navm") || !strings.Contains(string(sys), "NAVM") {
		t.Fatal("sys_io.cpp must load NAVM sidecars")
	}
	if !strings.Contains(string(sys), "gs_draw_primary_mesh_node") {
		t.Fatal("sys_io.cpp must apply spawn ofs to the MESH vertex node, not only node 0")
	}
	vu1, err := files.ReadFile("runtime/vu1_draw.cpp")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(vu1), "frame->width") || !strings.Contains(string(vu1), "gs_draw_fb_origin") {
		t.Fatal("vu1_draw.cpp must clear using DISP framebuffer size")
	}
	if !strings.Contains(string(vu1), "gs_draw_overlay") {
		t.Fatal("vu1_draw.cpp must draw HUD/fade on the VU1 path")
	}
	if strings.Contains(string(vu1), "2048.0f - 320.0f") {
		t.Fatal("vu1_draw.cpp must not hardcode 320x224 clear")
	}
	if strings.Contains(string(vu1), "packet2_create(1,") {
		t.Fatal("vu1_draw.cpp must not allocate 1-qword packets (overflows into malloc)")
	}
	if !strings.Contains(string(gs), "gs_draw_set_fade") || !strings.Contains(string(gs), "draw_rect_filled") {
		t.Fatal("gs_draw.cpp must overlay set_fade as a blended GS rect")
	}
	if !strings.Contains(string(gs), "gs_draw_layer_add") || !strings.Contains(string(gs), "g_ly_n") {
		t.Fatal("gs_draw.cpp must draw resident layers after pack 0")
	}
	if !strings.Contains(string(gs), "csm1_index") || !strings.Contains(string(gs), "expand_ct16") {
		t.Fatal("gs_draw.cpp must expand T8 to CT16 when VRAM allows and keep CSM1 CLUT fallback")
	}
	if !strings.Contains(string(gs), "nz = -z / w") {
		t.Fatal("gs_draw.cpp must invert NDC z so GS GEQUAL keeps nearer surfaces")
	}
	if !strings.Contains(string(gs), "apply_gs_z") || !strings.Contains(string(gs), "16777215.0f") || !strings.Contains(string(gs), "GIF_REG_XYZF2") {
		t.Fatal("gs_draw.cpp must write reverse-Z as XYZF2 (24-bit Z, then the fog factor)")
	}
	if !strings.Contains(string(gs), "'I' && h[1] == 'N' && h[2] == 'S' && h[3] == 'T'") || !strings.Contains(string(gs), "draw_instances") {
		t.Fatal("gs_draw.cpp must read MESH 'INST' and draw each instance with its delta")
	}
	if !strings.Contains(string(gs), "gs_draw_set_fog") || !strings.Contains(string(gs), "GS_REG_FOGCOL") {
		t.Fatal("gs_draw.cpp must set GS fog (FOGCOL, PRIM FGE) for PS2.set_fog")
	}
	if !strings.Contains(string(gs), "VU1Scene_CodeStart") || !strings.Contains(string(gs), "vu_take") {
		t.Fatal("gs_draw.cpp must send unclipped chunks to VU1 (vu1_scene.vsm)")
	}
	if !strings.Contains(string(gs), "draw_zbuffer") {
		t.Fatal("gs_draw.cpp must bind the Z buffer every frame and after packet splits")
	}
	if !strings.Contains(string(gs), "cam_cannot_see_aabb") || !strings.Contains(string(gs), "frame_aabb()") {
		t.Fatal("gs_draw.cpp must re-frame the AABB when follow parks the eye inside the mesh")
	}
	if !strings.Contains(string(gs), "gs_xfer_bytes") || !strings.Contains(string(gs), "dup_align_xfer") {
		t.Fatal("gs_draw.cpp must pad short GTEX payloads to the GS BITBLT size")
	}
	if !strings.Contains(string(gs), "aabb_is_tiny()") || !strings.Contains(string(gs), "span * 6.0f + 24.0f") {
		t.Fatal("gs_draw.cpp must re-frame tiny meshes and cameras that drift too far from the AABB")
	}
	if !strings.Contains(string(gs), "span * 0.9f + 4.5f") {
		t.Fatal("gs_draw.cpp must stand farther from tiny AABB meshes so they stay framed")
	}
	if !strings.Contains(string(gs), "face_tint") {
		t.Fatal("gs_draw.cpp must draw tiny AABB meshes untextured with face tints")
	}
	if !strings.Contains(string(gs), "gs_draw_primary_mesh_node") {
		t.Fatal("gs_draw.cpp must expose the MESH vertex node for spawn ofs")
	}
	if strings.Contains(string(gs), "have_tex") && strings.Contains(string(gs), "draw_rect_textured(q, 0, &bar)") {
		t.Fatal("gs_draw.cpp must not blit GTEX onto HUD bars (garbles Pause/PS2 text)")
	}
	if !strings.Contains(string(gs), "40, 160, 90") {
		t.Fatal("gs_draw.cpp gold fallback must emit two colors so stills are not a flat field")
	}
	if !strings.Contains(string(gs), "aabb_is_tiny") {
		t.Fatal("gs_draw.cpp must skip Z test on tiny preview AABBs")
	}
	if !strings.Contains(string(gs), "-0.5f * (float)(width") && !strings.Contains(string(gs), "-0.5f * (float)frame->width") {
		t.Fatal("gs_draw.cpp 2D sprites must be center-relative (ps2sdk START_OFFSET)")
	}
	if strings.Contains(string(gs), "256, 1, GS_PSM_16") {
		t.Fatal("gs_draw.cpp must not upload 8-bit CLUT as a linear 256x1 PSM16 strip")
	}
	cmake, err := files.ReadFile("runtime/CMakeLists.txt")
	if err != nil {
		t.Fatal(err)
	}
	cm := string(cmake)
	if !strings.Contains(cm, "CPU GIF fallback") || !strings.Contains(cm, "BLAZIUM_DVP_AS") {
		t.Fatal("CMakeLists must enable VU1 when dvp-as exists and name the CPU GIF fallback")
	}
	if strings.Contains(cm, "-lgskit") || strings.Contains(strings.ToLower(cm), "libgskit") {
		t.Fatal("CMakeLists must not link gsKit")
	}
	mk, err := files.ReadFile("runtime/Makefile")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(mk), "extra/*.cpp") {
		t.Fatal("Makefile must glob extra/*.cpp")
	}
	hooks, err := files.ReadFile("runtime/guest_hooks.h")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(hooks), "user_init") || !strings.Contains(string(hooks), "user_tick") || !strings.Contains(string(hooks), "user_pad") {
		t.Fatal("guest_hooks.h must declare user_init/user_tick/user_pad")
	}
	main2, err := files.ReadFile("runtime/main.cpp")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(main2), "user_init") || !strings.Contains(string(main2), "user_tick") || !strings.Contains(string(main2), "user_pad") {
		t.Fatal("main.cpp must call user_init/user_tick/user_pad")
	}
}

func TestGuestSysIO(t *testing.T) {
	sys, err := files.ReadFile("runtime/sys_io.cpp")
	if err != nil {
		t.Fatal(err)
	}
	src := string(sys)
	if !strings.Contains(src, "CAM00") || !strings.Contains(src, "HUD00") || !strings.Contains(src, "NAV00") {
		t.Fatal("sys_io.cpp must load CAM/HUD/NAV sidecars")
	}
	if !strings.Contains(src, "sys_io_load_pack") || !strings.Contains(src, "CAM%02d") {
		t.Fatalf("%s", "sys_io.cpp must reload CAM%02d per pack")
	}
	if !strings.Contains(src, "sys_io_say_done") || !strings.Contains(src, "s_hud_kind") {
		t.Fatal("sys_io.cpp must expose say_done and button HUD kinds")
	}
	if !strings.Contains(src, "s_paused") || !strings.Contains(src, "s_hud_just_accept") {
		t.Fatal("sys_io.cpp must toggle Start pause and HUD accept actions")
	}
	if !strings.Contains(src, "s_hud_act") || !strings.Contains(src, "strchr") {
		t.Fatal("sys_io.cpp must split HUD action|label")
	}
	if !strings.Contains(src, "sys_io_seek_anim") || !strings.Contains(src, "s_ak_t") {
		t.Fatal("sys_io.cpp must interpolate ANIM TRS keys")
	}
	if !strings.Contains(src, "FMV/IPU") && !strings.Contains(src, "license-clean") {
		t.Fatal("sys_io.cpp must name FMV/IPU skip")
	}
}

func TestRuntimeNamesHasScriptVM(t *testing.T) {
	var vm, hdr bool
	for _, name := range RuntimeNames {
		if name == "script_vm.cpp" {
			vm = true
		}
		if name == "script_vm.h" {
			hdr = true
		}
	}
	if !vm || !hdr {
		t.Fatalf("RuntimeNames missing script_vm: %v", RuntimeNames)
	}
}

func TestOverlayReplacesAndAdds(t *testing.T) {
	dir := t.TempDir()
	if err := Install(dir); err != nil {
		t.Fatal(err)
	}
	over := t.TempDir()
	if err := os.MkdirAll(filepath.Join(over, "extra"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(over, "main.cpp"), []byte("// overlay main\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(over, "extra", "hooks.cpp"), []byte("// extra hook\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := Overlay(dir, over); err != nil {
		t.Fatal(err)
	}
	got, err := os.ReadFile(filepath.Join(dir, "main.cpp"))
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != "// overlay main\n" {
		t.Fatalf("main.cpp not replaced: %q", got)
	}
	if _, err := os.Stat(filepath.Join(dir, "extra", "hooks.cpp")); err != nil {
		t.Fatal(err)
	}
}
