package ps2

import (
	"context"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	guest "github.com/blazium-games/blazium-toolchain/guest/ps2"
	"github.com/blazium-games/blazium-toolchain/internal/platforms"
)

func plantCompileTools(t *testing.T, prefix string) {
	t.Helper()
	gcc := filepath.Join(prefix, "ps2", "ps2dev", "ee", "bin", "mips64r5900el-ps2-elf-gcc")
	if err := os.MkdirAll(filepath.Dir(gcc), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(gcc, []byte("gcc"), 0o644); err != nil {
		t.Fatal(err)
	}
	sdk := filepath.Join(prefix, "ps2", "ps2sdk")
	if err := os.MkdirAll(filepath.Join(sdk, "ee", "include"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(sdk, "samples"), 0o755); err != nil {
		t.Fatal(err)
	}
	cc1 := filepath.Join(prefix, "ps2", "ps2dev", "ee", "libexec", "gcc", "cc1")
	if runtime.GOOS == "windows" {
		cc1 += ".exe"
	}
	if err := os.MkdirAll(filepath.Dir(cc1), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(cc1, []byte("cc1"), 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestSetupWritesState(t *testing.T) {
	dir := t.TempDir()
	plantCompileTools(t, dir)
	t.Setenv("EE_GCC", "")
	t.Setenv("PS2SDK", "")
	t.Setenv("PS2DEV", "")
	tool := New()
	err := tool.Setup(context.Background(), platforms.SetupOptions{
		CommonOptions: platforms.CommonOptions{Prefix: dir, Stdout: os.Stdout},
		Profile:       "compile",
		Offline:       true,
	})
	if err != nil {
		t.Fatal(err)
	}
	env, err := tool.Env(platforms.CommonOptions{Prefix: dir})
	if err != nil {
		t.Fatal(err)
	}
	if !compileReady(env) {
		t.Fatalf("not ready: %+v", env)
	}
	if _, err := os.Stat(filepath.Join(dir, "ps2", "guest", "runtime", "main.cpp")); err != nil {
		t.Fatal(err)
	}
	if guest.CookABI != 1 {
		t.Fatalf("abi %d", guest.CookABI)
	}
}

func TestSetupRejectsBadProfile(t *testing.T) {
	tool := New()
	err := tool.Setup(context.Background(), platforms.SetupOptions{
		CommonOptions: platforms.CommonOptions{Prefix: t.TempDir()},
		Profile:       "dreamcast",
	})
	if err == nil {
		t.Fatal("expected error")
	}
}

func TestInfoSupported(t *testing.T) {
	info := New().Info()
	if info.ID != "ps2" || info.Status != platforms.StatusSupported {
		t.Fatalf("%+v", info)
	}
	var exp, elfInfo, chd bool
	for _, c := range info.Commands {
		if c == "export-guest" {
			exp = true
		}
		if c == "elf-info" {
			elfInfo = true
		}
		if c == "chd" {
			chd = true
		}
	}
	if !exp || !elfInfo || !chd {
		t.Fatalf("commands %v", info.Commands)
	}
	if !strings.Contains(info.Description, "Windows") || !strings.Contains(info.Description, "Linux") {
		t.Fatalf("description must name Windows/Linux: %s", info.Description)
	}
	if !strings.Contains(info.Description, "bundled") {
		t.Fatalf("description must say guest is bundled: %s", info.Description)
	}
}

func TestCHDRequiresIsoAndOut(t *testing.T) {
	err := WriteCHD(context.Background(), "", "", nil, nil)
	if err == nil {
		t.Fatal("expected usage error")
	}
	cue := filepath.Join(t.TempDir(), "g.cue")
	if err := os.WriteFile(cue, []byte("FILE"), 0o644); err != nil {
		t.Fatal(err)
	}
	err = WriteCHD(context.Background(), cue, filepath.Join(t.TempDir(), "g.chd"), nil, nil)
	if err == nil {
		t.Fatal("expected cue refusal")
	}
}

func TestISORequiresDirAndOut(t *testing.T) {
	err := New().ISO(context.Background(), platforms.ISOOptions{
		CommonOptions: platforms.CommonOptions{Prefix: t.TempDir()},
	})
	if err == nil {
		t.Fatal("expected usage error")
	}
}

func TestISOWritesSYSTEMCNF(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "GAME.ELF"), []byte{0x7f, 'E', 'L', 'F', 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}, 0o644); err != nil {
		t.Fatal(err)
	}
	out := filepath.Join(t.TempDir(), "g.iso")
	err := New().ISO(context.Background(), platforms.ISOOptions{
		CommonOptions: platforms.CommonOptions{Prefix: t.TempDir(), Stdout: os.Stdout},
		Dir:           dir,
		Out:           out,
	})
	if err != nil {
		t.Fatal(err)
	}
	cnf, err := os.ReadFile(filepath.Join(dir, "SYSTEM.CNF"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(cnf), "BOOT2") || !strings.Contains(string(cnf), "cdrom0:") {
		t.Fatalf("SYSTEM.CNF missing BOOT2: %s", cnf)
	}
}

func TestVerifyMipsELF(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "x.elf")
	hdr := make([]byte, 20)
	copy(hdr, []byte{0x7f, 'E', 'L', 'F'})
	hdr[18] = 8
	if err := os.WriteFile(p, hdr, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := verifyMipsELF(p); err != nil {
		t.Fatal(err)
	}
}

func TestStageCookEmbedForwardsGtex(t *testing.T) {
	dir := t.TempDir()
	node := filepath.Join(dir, "in-node.bin")
	gtex := filepath.Join(dir, "in-gtex.bin")
	if err := os.WriteFile(node, []byte("NODE\x01\x00\x02\x00"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(gtex, []byte("GTEX\x01\x00\x00\x00"), 0o644); err != nil {
		t.Fatal(err)
	}
	dest := filepath.Join(dir, "src")
	if err := os.MkdirAll(dest, 0o755); err != nil {
		t.Fatal(err)
	}
	opts := platforms.BuildOptions{Node: node, Gtex: gtex}
	if !hasCookSlices(opts) {
		t.Fatal("expected cook slices")
	}
	if err := stageCookEmbed(dest, opts); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"NODE00.bin", "GTEX00.bin", "cook_embed.S", "cook_flags.h", "cook.mk"} {
		if _, err := os.Stat(filepath.Join(dest, name)); err != nil {
			t.Fatalf("missing %s: %v", name, err)
		}
	}
	asm, err := os.ReadFile(filepath.Join(dest, "cook_embed.S"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(asm), "NODE00.bin") || !strings.Contains(string(asm), "GTEX00.bin") {
		t.Fatalf("asm missing incbin: %s", asm)
	}
	hdr, err := os.ReadFile(filepath.Join(dest, "cook_flags.h"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(hdr), "BLAZIUM_PS2_HAS_NODE") || !strings.Contains(string(hdr), "BLAZIUM_PS2_HAS_GTEX") {
		t.Fatalf("flags: %s", hdr)
	}
	if strings.Contains(string(hdr), "BLAZIUM_PS2_HAS_MESH") {
		t.Fatal("mesh was not passed")
	}
	mk, err := os.ReadFile(filepath.Join(dest, "cook.mk"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(mk), "cook_embed.o") || !strings.Contains(string(mk), "BLAZIUM_PS2_HAS_GTEX") {
		t.Fatalf("cook.mk missing objs/flags: %s", mk)
	}
}

func TestHostFsDirIsElfParent(t *testing.T) {
	got := hostFsDir(filepath.Join("export", "Game.elf"), "", "")
	if got != "export" {
		t.Fatalf("got %q", got)
	}
	if hostFsDir("Game.elf", "Game.iso", "") != "" {
		t.Fatal("ISO run must not pin HostFs")
	}
	if hostFsDir("Game.elf", "", "custom") != "custom" {
		t.Fatal("override")
	}
}

func TestStageIrxEmbed(t *testing.T) {
	sdk := t.TempDir()
	irxDir := filepath.Join(sdk, "iop", "irx")
	if err := os.MkdirAll(irxDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(irxDir, "fileXio.irx"), []byte("FXIO"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(irxDir, "freesd.irx"), []byte("FSD"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(irxDir, "sdrdrv.irx"), []byte("SDR"), 0o644); err != nil {
		t.Fatal(err)
	}
	dest := filepath.Join(t.TempDir(), "src")
	if err := os.MkdirAll(dest, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := stageIrxEmbed(dest, sdk); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"fileXio.irx", "freesd.irx", "sdrdrv.irx", "irx_embed.S", "irx_flags.h", "irx.mk"} {
		if _, err := os.Stat(filepath.Join(dest, name)); err != nil {
			t.Fatalf("missing %s: %v", name, err)
		}
	}
	asm, err := os.ReadFile(filepath.Join(dest, "irx_embed.S"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(asm), "fileXio.irx") || !strings.Contains(string(asm), "freesd.irx") || !strings.Contains(string(asm), "sdrdrv.irx") {
		t.Fatalf("asm: %s", asm)
	}
	hdr, err := os.ReadFile(filepath.Join(dest, "irx_flags.h"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(hdr), "BLAZIUM_PS2_HAS_IRX_FILEXIO") || !strings.Contains(string(hdr), "BLAZIUM_PS2_HAS_IRX_FREESD") || !strings.Contains(string(hdr), "BLAZIUM_PS2_HAS_IRX_SDRDRV") {
		t.Fatalf("flags: %s", hdr)
	}
}

func TestBiosReady(t *testing.T) {
	dir := t.TempDir()
	if biosReady(map[string]string{"PS2_BIOS_DIR": dir}) {
		t.Fatal("empty dir should not be ready")
	}
	if err := os.WriteFile(filepath.Join(dir, "readme.txt"), []byte("no"), 0o644); err != nil {
		t.Fatal(err)
	}
	if biosReady(map[string]string{"PS2_BIOS_DIR": dir}) {
		t.Fatal("txt only")
	}
	if err := os.WriteFile(filepath.Join(dir, "scph10000.bin"), []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	if !biosReady(map[string]string{"PS2_BIOS_DIR": dir}) {
		t.Fatal("bin should count")
	}
}

func TestBiosReadyNestedPrefers39001(t *testing.T) {
	root := t.TempDir()
	other := filepath.Join(root, "SCPH-70012_BIOS_V12_USA")
	pref := filepath.Join(root, "SCPH-39001_BIOS_V7_USA")
	if err := os.MkdirAll(other, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(pref, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(other, "other.bin"), []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(pref, "scph39001.bin"), []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	if !biosReady(map[string]string{"PS2_BIOS_DIR": root}) {
		t.Fatal("nested bin should count")
	}
	hit := findBiosDir(root)
	if hit != pref && filepath.Base(hit) != filepath.Base(pref) {
		t.Fatalf("prefer 39001, got %q", hit)
	}
}

func TestISOBootsProductCodeName(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "PENGUIN.ELF"), []byte{0x7f, 'E', 'L', 'F', 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}, 0o644); err != nil {
		t.Fatal(err)
	}
	out := filepath.Join(t.TempDir(), "g.iso")
	err := New().ISO(context.Background(), platforms.ISOOptions{
		CommonOptions: platforms.CommonOptions{Prefix: t.TempDir()},
		Dir:           dir,
		Out:           out,
	})
	if err != nil {
		t.Fatal(err)
	}
	cnf, err := os.ReadFile(filepath.Join(dir, "SYSTEM.CNF"))
	if err != nil {
		t.Fatal(err)
	}
	_ = cnf
	boot := serialELFName()
	disc, err := os.ReadFile(out)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(disc), boot+";1") {
		t.Fatalf("%s is not on the disc", boot)
	}
	if strings.Contains(string(disc), "PENGUIN.ELF") {
		t.Fatal("the original ELF name is still on the disc")
	}
}

func TestIsSerialELFName(t *testing.T) {
	for name, want := range map[string]bool{
		"SLUS_000.01": true, "slus_209.46": true, "SCES_500.51": true,
		"GAME.ELF": false, "PENGUIN.ELF": false, "SLUS_000.1": false, "SLUS-000.01": false,
	} {
		if got := isSerialELFName(name); got != want {
			t.Errorf("isSerialELFName(%q) = %v", name, got)
		}
	}
}

func TestISOBootELFOption(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "GAME.ELF"), []byte{0x7f, 'E', 'L', 'F', 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}, 0o644); err != nil {
		t.Fatal(err)
	}
	out := filepath.Join(t.TempDir(), "g.iso")
	opts := platforms.ISOOptions{CommonOptions: platforms.CommonOptions{Prefix: t.TempDir()}, Dir: dir, Out: out, BootELF: "slps_123.45"}
	if err := New().ISO(context.Background(), opts); err != nil {
		t.Fatal(err)
	}
	disc, err := os.ReadFile(out)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(disc), "SLPS_123.45;1") || !strings.Contains(string(disc), "cdrom0:\\SLPS_123.45;1") {
		t.Fatal("SLPS_123.45 is not the disc's boot ELF")
	}
	// the export folder is left as it was
	if got := bootELFName(dir); got != "GAME.ELF" {
		t.Fatalf("the folder's BOOT2 changed to %q", got)
	}
	opts.BootELF = "GAME.ELF"
	if err := New().ISO(context.Background(), opts); err == nil {
		t.Fatal("a non-product-code --boot-elf should be refused")
	}
}
