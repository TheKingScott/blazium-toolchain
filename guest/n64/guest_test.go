package guest

import (
	"os"
	"path/filepath"
	"runtime"
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

func TestRuntimeNamesHasScriptVMAndDFS(t *testing.T) {
	var vm, hdr, dfs, rdpq bool
	for _, name := range RuntimeNames {
		switch name {
		case "script_vm.cpp":
			vm = true
		case "script_vm.h":
			hdr = true
		case "dfs_io.cpp":
			dfs = true
		case "rdpq_draw.cpp":
			rdpq = true
		}
	}
	if !vm || !hdr || !dfs || !rdpq {
		t.Fatalf("RuntimeNames missing core files: %v", RuntimeNames)
	}
}

func TestMakefileUsesN64MK(t *testing.T) {
	mk, err := files.ReadFile("runtime/Makefile")
	if err != nil {
		t.Fatal(err)
	}
	s := string(mk)
	if !strings.Contains(s, "n64.mk") || !strings.Contains(s, "LIBDRAGON_PREVIEW") {
		t.Fatal("Makefile must include n64.mk with LIBDRAGON_PREVIEW")
	}
	if !strings.Contains(s, "extra/*.cpp") {
		t.Fatal("Makefile must glob extra/*.cpp")
	}
	low := strings.ToLower(s)
	if strings.Contains(low, "-lt3d") || strings.Contains(low, "-lultra") || strings.Contains(low, "t3d.h") {
		t.Fatal("Makefile must not link a P8 mesh lib or official SDK")
	}
	if strings.Contains(s, "dlopen") || strings.Contains(s, "dlfcn.h") || strings.Contains(s, ".dso") || strings.Contains(s, "n64dso") {
		t.Fatal("default guest Makefile must not build DSO overlays")
	}
}

func TestGuestNoForbiddenSDK(t *testing.T) {
	main, err := files.ReadFile("runtime/main.cpp")
	if err != nil {
		t.Fatal(err)
	}
	src := string(main)
	if strings.Contains(src, "ultra64.h") || strings.Contains(src, "t3d.h") {
		t.Fatal("main.cpp must not include official SDK or Tiny3D")
	}
	if !strings.Contains(src, "BLAZIUM_N64_COOK_ABI") {
		t.Fatal("main.cpp missing cook ABI")
	}
	if !strings.Contains(src, "user_init") || !strings.Contains(src, "user_tick") || !strings.Contains(src, "user_pad") {
		t.Fatal("main.cpp must call user_init/user_tick/user_pad")
	}
	if !strings.Contains(src, "RESOLUTION_320x240") {
		t.Fatal("main.cpp must init 320x240")
	}
	if !strings.Contains(src, "RESOLUTION_640x480") {
		t.Fatal("main.cpp must offer 640x480 behind BLAZIUM_N64_DISPLAY_640")
	}
	if !strings.Contains(src, "BLAZIUM_N64_DISPLAY_640") {
		t.Fatal("main.cpp must gate 640x480 on BLAZIUM_N64_DISPLAY_640")
	}
	if strings.Contains(src, "dlopen") || strings.Contains(src, "dlfcn.h") || strings.Contains(src, ".dso") || strings.Contains(src, "n64dso") {
		t.Fatal("main.cpp must not load DSO overlays")
	}
}

func TestGuestIOParity(t *testing.T) {
	pad, err := files.ReadFile("runtime/pad_io.h")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(pad), "pad_io_pressed") || !strings.Contains(string(pad), "pad_io_stick") || !strings.Contains(string(pad), "pad_io_set_deadzone") {
		t.Fatal("pad_io.h must expose Input action/stick/deadzone")
	}
	padSrc, err := files.ReadFile("runtime/pad_io.cpp")
	if err != nil {
		t.Fatal(err)
	}
	psrc := string(padSrc)
	if !strings.Contains(psrc, "cooked_inp") || !strings.Contains(psrc, "INP6") {
		t.Fatal("pad_io.cpp must read cooked INP6")
	}
	if strings.Contains(psrc, "TIM") || strings.Contains(psrc, "GTEX") {
		t.Fatal("pad_io.cpp must not use TIM/GTEX")
	}
	if !strings.Contains(psrc, "BLAZIUM_N64_RUMBLE") || !strings.Contains(psrc, "joypad_set_rumble_active") {
		t.Fatal("pad_io.cpp must gate rumble on BLAZIUM_N64_RUMBLE")
	}
	if !strings.Contains(psrc, "(void)on") {
		t.Fatal("pad_io.cpp must keep rumble stub when flag is off")
	}
	if strings.Contains(psrc, "dlopen") || strings.Contains(psrc, "dlfcn.h") || strings.Contains(psrc, ".dso") || strings.Contains(psrc, "n64dso") {
		t.Fatal("pad_io.cpp must not load DSO overlays")
	}
	pack, err := files.ReadFile("runtime/pack_io.cpp")
	if err != nil {
		t.Fatal(err)
	}
	ps := string(pack)
	if !strings.Contains(ps, "rom://") || !strings.Contains(ps, "user://") {
		t.Fatal("pack_io.cpp must map rom:// and user://")
	}
	if !strings.Contains(ps, "eepfs_wipe") || !strings.Contains(ps, "eepfs_write") || !strings.Contains(ps, "eepfs_read") {
		t.Fatal("pack_io.cpp must format EEPROM and read/write user://")
	}
	if !strings.Contains(ps, "SAVE") || !strings.Contains(ps, "eeprom_wait_idle") {
		t.Fatal("pack_io.cpp must seed a SAVE slot and wait for EEPROM idle")
	}
	if !strings.Contains(ps, "pack_io_poke") || !strings.Contains(ps, "MESH%02d") || !strings.Contains(ps, "STREAM") {
		t.Fatalf("%s", "pack_io.cpp must poke/peek and load MESH%02d / STREAM")
	}
	if !strings.Contains(ps, "fread") || !strings.Contains(ps, "rom://PACK") {
		t.Fatal("pack_io.cpp must fread extra packs from rom://PACK")
	}
	if !strings.Contains(ps, "N64_LAYERS") || !strings.Contains(ps, "pack_io_can_fit") || !strings.Contains(ps, "N64_EXTRA_RDRAM_CAP") {
		t.Fatal("pack_io.cpp must RAM-gate layers")
	}
	if !strings.Contains(ps, "rebind") || !strings.Contains(ps, "pack_io_instantiate") {
		t.Fatal("pack_io.cpp must rebind MESH/NODE/NTEX and instantiate")
	}
	if !strings.Contains(ps, "BLAZIUM_N64_RDRAM_4") {
		t.Fatal("pack_io.cpp must mention BLAZIUM_N64_RDRAM_4")
	}
	if !strings.Contains(ps, "2 * 1024 * 1024") {
		t.Fatal("pack_io.cpp must keep the 2 MiB extra-pack default")
	}
	dfs, err := files.ReadFile("runtime/dfs_io.cpp")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(dfs), "rom://") || !strings.Contains(string(dfs), "dfs_init") {
		t.Fatal("dfs_io.cpp must init DragonFS rom://")
	}
	sfx, err := files.ReadFile("runtime/sfx_io.cpp")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(sfx), "mixer_init") || !strings.Contains(string(sfx), "MUSIC00") || !strings.Contains(string(sfx), "sfx_io_audible") {
		t.Fatal("sfx_io.cpp must use mixer and MUSIC00")
	}
	if !strings.Contains(string(sfx), "wav64_open") || !strings.Contains(string(sfx), "wav64_play") || !strings.Contains(string(sfx), "mixer_try_play") {
		t.Fatal("sfx_io.cpp must play wav64 through the mixer")
	}
	if strings.Contains(string(sfx), "VAG") || strings.Contains(string(sfx), "libultra") {
		t.Fatal("sfx_io.cpp must not use VAG or libultra audio")
	}
}

func TestGuestScriptKitParity(t *testing.T) {
	vm, err := files.ReadFile("runtime/script_vm.cpp")
	if err != nil {
		t.Fatal(err)
	}
	src := string(vm)
	for _, want := range []string{
		"OP_INPUT_TRANSLATE", "OP_USER_SAVE", "OP_KIT_TICK", "Checkpoint",
		"s_kit_player", "pad_io_stick", "sys_io_spawn_ofs", "do_checkpoint",
		"Spawn", "Portal", "bind_kit_from_node", "s_kit_spawn", "s_kit_portal",
		"NAT_SET_CHECKPOINT", "NAT_RESPAWN", "s_grav_vy", "s_kit_follow",
		"OP_SAY", "OP_PLAY_FMV", "OP_PLAY_MUSIC", "OP_MUSIC_VOL",
		"OP_LOOK_STICK", "OP_SHAKE_CAMERA", "OP_TWEEN", "OP_TIMER",
		"OP_SLIDE", "sys_io_slide", "OP_OVERLAP", "OP_RAYCAST", "OP_TILE_AT",
		"OP_SET_FADE", "OP_SCENE_FADE", "OP_CHANGE_SCENE", "OP_INSTANTIATE",
		"OP_LOAD_SCENE", "NAT_CAN_INSTANTIATE", "NAT_SET_CAM", "sys_io_set_cam",
		"NAT_LOAD_SPRITES", "NAT_MOVE_PLANAR", "NAT_HURT", "NAT_POKE",
		"NAT_GET_PRESSURE", "NAT_MOVE_6DOF", "NAT_LOAD_PARTICLES", "NAT_PATH_FOLLOW",
		"OP_JMP", "OP_CALL_NATIVE", "pack_io_find_path", "OP_CALL_ROTATE_Y",
		"SCRP", "pack_io_instantiate", "script_vm_rebind_node",
	} {
		if !strings.Contains(src, want) {
			t.Fatalf("script_vm.cpp missing %s", want)
		}
	}
}

func TestGuestDrawGoldFallback(t *testing.T) {
	rdpq, err := files.ReadFile("runtime/rdpq_draw.cpp")
	if err != nil {
		t.Fatal(err)
	}
	src := string(rdpq)
	if strings.Contains(src, "t3d.h") || strings.Contains(src, "#include <t3d") {
		t.Fatal("rdpq_draw must not include a P8 mesh header")
	}
	if !strings.Contains(src, "40, 160, 90") {
		t.Fatal("rdpq gold fallback must emit two colors")
	}
	if !strings.Contains(src, "rdpq_draw_set_camera") || !strings.Contains(src, "rdpq_draw_set_fade") {
		t.Fatal("rdpq_draw must expose camera/fade")
	}
	if !strings.Contains(src, "rdpq_draw_rebind") || !strings.Contains(src, "MESH%02d") {
		t.Fatalf("%s", "rdpq_draw must rebind MESH%02d after pack swap")
	}
}

func TestGuestIndustrialKit(t *testing.T) {
	sys, err := files.ReadFile("runtime/sys_io.cpp")
	if err != nil {
		t.Fatal(err)
	}
	ss := string(sys)
	for _, want := range []string{"CAMN", "HITN", "aabb", "load_anims", "SPRN", "TILN"} {
		if !strings.Contains(ss, want) {
			t.Fatalf("sys_io.cpp missing %s", want)
		}
	}
	if strings.Contains(ss, "rdpq_draw_set_camera(0, 0, 0, 0, 0, 55.0f)") {
		t.Fatal("sys_io_tick must not force a dummy camera every frame")
	}
	if strings.Contains(ss, "return 0;\n}\n\nint sys_io_raycast") {
		/* overlap stub was a lone return 0; real AABB must mention mn/mx */
	}
	if !strings.Contains(ss, "mn[0]") || !strings.Contains(ss, "mx[0]") {
		t.Fatal("sys_io.cpp must test AABB min/max")
	}
	pack, err := files.ReadFile("runtime/pack_io.cpp")
	if err != nil {
		t.Fatal(err)
	}
	ps := string(pack)
	if !strings.Contains(ps, "4096") {
		t.Fatal("pack_io.cpp EEPROM must be 4096")
	}
	if strings.Contains(ps, ".size = 256") {
		t.Fatal("pack_io.cpp must not keep the 256-byte eepfs slot")
	}
	pad, err := files.ReadFile("runtime/pad_io.cpp")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(pad), "c_left") && !strings.Contains(string(pad), "c_up") {
		t.Fatal("pad_io.cpp must map C-buttons")
	}
	if !strings.Contains(string(pad), "BTN_CLEFT") {
		t.Fatal("pad_io.cpp must define C-button bits")
	}
	vm, err := files.ReadFile("runtime/script_vm.cpp")
	if err != nil {
		t.Fatal(err)
	}
	vs := string(vm)
	if !strings.Contains(vs, "OP_ATTACH_CAMERA") || !strings.Contains(vs, "sys_io_attach_camera") {
		t.Fatal("script_vm.cpp must implement OP_ATTACH_CAMERA")
	}
	if !strings.Contains(vs, "NAT_PLAY_ANIM") || !strings.Contains(vs, "load_anims") {
		t.Fatal("script_vm.cpp must wire ANIM natives after 71")
	}
	mk, err := files.ReadFile("runtime/Makefile")
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(mk), "dlopen") {
		t.Fatal("Makefile still no dlopen")
	}
}

func TestGuestGenreSystems(t *testing.T) {
	sys, err := files.ReadFile("runtime/sys_io.cpp")
	if err != nil {
		t.Fatal(err)
	}
	ss := string(sys)
	for _, want := range []string{"PRTN", "PTHN", "NAVM", "billboard", "follow_node", "sys_io_tween_start", "sys_io_path_follow", "sys_io_move_6dof"} {
		if !strings.Contains(ss, want) {
			t.Fatalf("sys_io.cpp missing %s", want)
		}
	}
	if strings.Contains(ss, "void sys_io_tween_start(int id, float dur)\n{\n\t(void)id;\n\t(void)dur;\n}") {
		t.Fatal("sys_io_tween_start must not stay a (void) stub")
	}
	if strings.Contains(ss, "void sys_io_path_follow(int path)\n{\n\t(void)path;\n}") {
		t.Fatal("sys_io_path_follow must not stay a (void) stub")
	}
	rdpq, err := files.ReadFile("runtime/rdpq_draw.cpp")
	if err != nil {
		t.Fatal(err)
	}
	rs := string(rdpq)
	if !strings.Contains(rs, "g_look_pitch") {
		t.Fatal("rdpq_draw_look must use pitch")
	}
	mk, err := files.ReadFile("runtime/Makefile")
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(mk), "dlopen") {
		t.Fatal("Makefile still no dlopen")
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

func TestGDIRSharedMatchesPS2(t *testing.T) {
	_, thisFile, _, ok := runtime.Caller(0)
	if !ok {
		t.Skip("no caller info")
	}
	ps2 := filepath.Join(filepath.Dir(thisFile), "..", "ps2", "runtime")
	for _, name := range GDIRShared {
		n64, err := gdirFiles.ReadFile("gdir/runtime/" + name)
		if err != nil {
			t.Fatalf("embed missing %s: %v", name, err)
		}
		want, err := os.ReadFile(filepath.Join(ps2, name))
		if err != nil {
			t.Fatal(err)
		}
		if string(n64) != string(want) {
			t.Errorf("guest/n64/gdir/runtime/%s differs from guest/ps2/runtime/%s (copy it across)", name, name)
		}
	}
}

func TestInstallGDIRWritesMakefile(t *testing.T) {
	dir := t.TempDir()
	if err := InstallGDIR(dir); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"Makefile", "main.cpp", "t3d_draw.cpp", "gdir_vm.cpp", "iso_path.h"} {
		if _, err := os.Stat(filepath.Join(dir, name)); err != nil {
			t.Fatalf("missing %s: %v", name, err)
		}
	}
}
