package app

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/blazium-games/blazium-toolchain/internal/platforms"
)

func plantCompile(t *testing.T, prefix string) {
	t.Helper()
	gcc := filepath.Join(prefix, "ps1", "gcc", "bin", "mipsel-none-elf-gcc")
	if err := os.MkdirAll(filepath.Dir(gcc), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(gcc, []byte("gcc"), 0o644); err != nil {
		t.Fatal(err)
	}
	elf := filepath.Join(prefix, "ps1", "elf2x", "elf2x")
	if err := os.MkdirAll(filepath.Dir(elf), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(elf, []byte("elf2x"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(prefix, "ps1", "psn00bsdk", "lib", "libpsn00b"), 0o755); err != nil {
		t.Fatal(err)
	}
}

func TestVersionJSON(t *testing.T) {
	var out bytes.Buffer
	code := Run(context.Background(), []string{"--json", "version"}, &out, &bytes.Buffer{})
	if code != ExitOK {
		t.Fatalf("exit %d", code)
	}
	var m map[string]string
	if err := json.Unmarshal(out.Bytes(), &m); err != nil {
		t.Fatal(err)
	}
	if m["name"] != "blazium-toolchain" || m["version"] != Version || m["license"] != License {
		t.Fatalf("%v", m)
	}
}

func TestListJSONHasPS1(t *testing.T) {
	var out bytes.Buffer
	code := Run(context.Background(), []string{"--json", "list"}, &out, &bytes.Buffer{})
	if code != ExitOK {
		t.Fatalf("exit %d", code)
	}
	var list []platforms.Info
	if err := json.Unmarshal(out.Bytes(), &list); err != nil {
		t.Fatal(err)
	}
	var ps1 bool
	for _, info := range list {
		if info.ID == "ps1" && info.Status == platforms.StatusSupported {
			ps1 = true
			var fmv, exp bool
			for _, c := range info.Commands {
				if c == "fmv" {
					fmv = true
				}
				if c == "export-guest" {
					exp = true
				}
			}
			if !fmv || !exp {
				t.Fatalf("ps1 commands missing fmv/export-guest: %v", info.Commands)
			}
		}
	}
	if !ps1 {
		t.Fatalf("list: %s", out.String())
	}
	var interdvd bool
	for _, info := range list {
		if info.ID == "interdvd" && info.Status == platforms.StatusSupported {
			interdvd = true
			var meta bool
			for _, c := range info.Commands {
				if c == "meta" {
					meta = true
				}
			}
			if !meta {
				t.Fatalf("interdvd commands missing meta: %v", info.Commands)
			}
		}
	}
	if !interdvd {
		t.Fatalf("missing interdvd in %s", out.String())
	}
	for _, info := range list {
		if info.ID != "interdvd" {
			continue
		}
		var ffmpeg, ffprobe bool
		for _, c := range info.Commands {
			if c == "ffmpeg" {
				ffmpeg = true
			}
			if c == "ffprobe" {
				ffprobe = true
			}
		}
		if !ffmpeg || !ffprobe {
			t.Fatalf("interdvd commands missing ffmpeg/ffprobe: %v", info.Commands)
		}
	}
	var ps2ok bool
	for _, info := range list {
		if info.ID == "ps2" && info.Status == platforms.StatusSupported {
			ps2ok = true
		}
	}
	if !ps2ok {
		t.Fatalf("missing supported ps2 in %s", out.String())
	}
	for _, info := range list {
		if info.ID != "ps2" {
			continue
		}
		if !strings.Contains(info.Description, "Windows") || !strings.Contains(info.Description, "Linux") {
			t.Fatalf("ps2 description must name Windows/Linux: %s", info.Description)
		}
		if !strings.Contains(info.Description, "bundled") {
			t.Fatalf("ps2 description must say guest is bundled: %s", info.Description)
		}
	}
	for _, want := range []string{"ps4"} {
		var found bool
		for _, info := range list {
			if info.ID == want && info.Status == platforms.StatusPlanned {
				found = true
			}
		}
		if !found {
			t.Fatalf("missing planned %s in %s", want, out.String())
		}
	}
	var n64ok bool
	for _, info := range list {
		if info.ID == "n64" && info.Status == platforms.StatusSupported {
			n64ok = true
			var exp, rom bool
			for _, c := range info.Commands {
				if c == "export-guest" {
					exp = true
				}
				if c == "rom" {
					rom = true
				}
			}
			if !exp || !rom {
				t.Fatalf("n64 commands missing export-guest/rom: %v", info.Commands)
			}
		}
	}
	if !n64ok {
		t.Fatalf("missing supported n64 in %s", out.String())
	}
	var steamok bool
	for _, info := range list {
		if info.ID == "steam" && info.Status == platforms.StatusSupported {
			steamok = true
			var setup bool
			for _, c := range info.Commands {
				if c == "setup" {
					setup = true
				}
			}
			if !setup {
				t.Fatalf("steam commands missing setup: %v", info.Commands)
			}
		}
	}
	if !steamok {
		t.Fatalf("missing supported steam in %s", out.String())
	}
}

func TestPS2IsSupportedNotPlanned(t *testing.T) {
	var errBuf bytes.Buffer
	code := Run(context.Background(), []string{"ps2", "setup", "--offline"}, &bytes.Buffer{}, &errBuf)
	if code == ExitPlanned {
		t.Fatalf("ps2 should not be planned: %s", errBuf.String())
	}
	if code != ExitOK && code != ExitTool {
		t.Fatalf("exit %d body %s", code, errBuf.String())
	}
}

func plantDev(t *testing.T, prefix string) {
	t.Helper()
	dir := filepath.Join(prefix, "ps1", "pcsx-redux")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	for _, n := range []string{"pcsx-redux", "pcsx-redux.exe"} {
		if err := os.WriteFile(filepath.Join(dir, n), []byte("pcsx"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	bios := filepath.Join(prefix, "ps1", "openbios", "openbios.bin")
	if err := os.MkdirAll(filepath.Dir(bios), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(bios, []byte("OB"), 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestPS1SetupThenEnv(t *testing.T) {
	t.Setenv("PCSX_EXE", "")
	t.Setenv("OPENBIOS", "")
	dir := t.TempDir()
	plantCompile(t, dir)
	plantDev(t, dir)
	var out, errb bytes.Buffer
	code := Run(context.Background(), []string{"--prefix", dir, "ps1", "setup", "--profile", "dev", "--offline"}, &out, &errb)
	if code != ExitOK {
		t.Fatalf("setup %d %s", code, errb.String())
	}
	out.Reset()
	code = Run(context.Background(), []string{"--prefix", dir, "--json", "ps1", "env"}, &out, &errb)
	if code != ExitOK {
		t.Fatalf("env %d %s", code, errb.String())
	}
	var env map[string]string
	if err := json.Unmarshal(out.Bytes(), &env); err != nil {
		t.Fatal(err)
	}
}

func TestPS1StatusReadyJSON(t *testing.T) {
	dir := t.TempDir()
	plantCompile(t, dir)
	var out, errb bytes.Buffer
	code := Run(context.Background(), []string{"--prefix", dir, "ps1", "setup", "--profile", "compile", "--offline"}, &out, &errb)
	if code != ExitOK {
		t.Fatalf("setup %d %s", code, errb.String())
	}
	out.Reset()
	code = Run(context.Background(), []string{"--prefix", dir, "--json", "ps1", "status"}, &out, &errb)
	if code != ExitOK {
		t.Fatalf("status %d %s", code, errb.String())
	}
	var st map[string]any
	if err := json.Unmarshal(out.Bytes(), &st); err != nil {
		t.Fatal(err)
	}
	ready, _ := st["ready"].(bool)
	if !ready {
		t.Fatalf("status %s", out.String())
	}
	abi, _ := st["guest_abi"].(float64)
	if int(abi) != 20 {
		t.Fatalf("guest_abi %v in %s", st["guest_abi"], out.String())
	}
	if supported, _ := st["host_supported"].(bool); !supported {
		t.Fatalf("host_supported %s", out.String())
	}
	if _, ok := st["host_os"].(string); !ok {
		t.Fatalf("host_os %s", out.String())
	}
}

func TestHelp(t *testing.T) {
	var out bytes.Buffer
	if Run(context.Background(), []string{"help"}, &out, &bytes.Buffer{}) != ExitOK {
		t.Fatal("help")
	}
	if !strings.Contains(out.String(), "ps1") || !strings.Contains(out.String(), "interdvd") || !strings.Contains(out.String(), "steam") {
		t.Fatal(out.String())
	}
	if !strings.Contains(out.String(), "Windows and Linux") {
		t.Fatal(out.String())
	}
}

func TestSteamSetupOffline(t *testing.T) {
	prefix := t.TempDir()
	dest := filepath.Join(prefix, "steam", "steamcmd")
	if err := os.MkdirAll(dest, 0o755); err != nil {
		t.Fatal(err)
	}
	name := "steamcmd"
	if runtime.GOOS == "windows" {
		name = "steamcmd.exe"
	}
	if err := os.WriteFile(filepath.Join(dest, name), []byte("steamcmd"), 0o755); err != nil {
		t.Fatal(err)
	}
	var out, errb bytes.Buffer
	code := Run(context.Background(), []string{"--prefix", prefix, "--json", "steam", "setup", "--offline"}, &out, &errb)
	if code != ExitOK {
		t.Fatalf("setup %d %s", code, errb.String())
	}
	out.Reset()
	code = Run(context.Background(), []string{"--prefix", prefix, "--json", "steam", "status"}, &out, &errb)
	if code != ExitOK {
		t.Fatalf("status %d %s", code, errb.String())
	}
	var st map[string]any
	if err := json.Unmarshal(out.Bytes(), &st); err != nil {
		t.Fatal(err)
	}
	if ready, _ := st["ready"].(bool); !ready {
		t.Fatalf("%s", out.String())
	}
	if st["steamcmd"] == "" {
		t.Fatalf("%s", out.String())
	}
}

func TestPS1FMVJSON(t *testing.T) {
	var out bytes.Buffer
	code := Run(context.Background(), []string{"--json", "ps1", "fmv"}, &out, &bytes.Buffer{})
	if code != ExitOK {
		t.Fatalf("exit %d body %s", code, out.String())
	}
	var m map[string]any
	if err := json.Unmarshal(out.Bytes(), &m); err != nil {
		t.Fatal(err)
	}
	if m["encoder"] != "blazium-mit" || m["in_editor"] != true || m["spawn"] != false {
		t.Fatalf("%v", m)
	}
}

func TestUnknownCommand(t *testing.T) {
	var errBuf bytes.Buffer
	code := Run(context.Background(), []string{"ps1", "frobnicate"}, &bytes.Buffer{}, &errBuf)
	if code != ExitUsage {
		t.Fatalf("exit %d", code)
	}
	got := errBuf.String()
	for _, want := range []string{
		"error      usage",
		"unknown command",
		"---",
		"command    blazium-toolchain ps1 frobnicate",
		"host       ",
		"version    blazium-toolchain",
	} {
		if !strings.Contains(got, want) {
			t.Fatalf("missing %q in:\n%s", want, got)
		}
	}
}

func TestPS2ChdUsage(t *testing.T) {
	code := Run(context.Background(), []string{"ps2", "chd"}, &bytes.Buffer{}, &bytes.Buffer{})
	if code != ExitUsage {
		t.Fatalf("ps2 chd without flags exit %d", code)
	}
	code = Run(context.Background(), []string{"ps1", "chd", "--iso", "x.iso", "--out", "x.chd"}, &bytes.Buffer{}, &bytes.Buffer{})
	if code != ExitUsage {
		t.Fatalf("ps1 chd exit %d", code)
	}
}

func TestPS2ElfInfo(t *testing.T) {
	const ehSize, shSize, shNum, textSize = 52, 40, 3, 16
	shstrtab := []byte("\x00.text\x00.shstrtab\x00")
	shoff := ehSize
	shstrtabOff := shoff + shSize*shNum
	textOff := shstrtabOff + len(shstrtab)
	raw := make([]byte, textOff+textSize)
	raw[0], raw[1], raw[2], raw[3] = 0x7f, 'E', 'L', 'F'
	raw[4], raw[5], raw[6] = 1, 1, 1
	binary.LittleEndian.PutUint16(raw[16:], 2)
	binary.LittleEndian.PutUint16(raw[18:], 8)
	binary.LittleEndian.PutUint32(raw[20:], 1)
	binary.LittleEndian.PutUint32(raw[32:], uint32(shoff))
	binary.LittleEndian.PutUint16(raw[40:], ehSize)
	binary.LittleEndian.PutUint16(raw[46:], shSize)
	binary.LittleEndian.PutUint16(raw[48:], shNum)
	binary.LittleEndian.PutUint16(raw[50:], 2)
	writeSH := func(idx int, name, typ, off, size uint32) {
		o := shoff + idx*shSize
		binary.LittleEndian.PutUint32(raw[o:], name)
		binary.LittleEndian.PutUint32(raw[o+4:], typ)
		binary.LittleEndian.PutUint32(raw[o+16:], off)
		binary.LittleEndian.PutUint32(raw[o+20:], size)
	}
	writeSH(1, 1, 1, uint32(textOff), textSize)
	writeSH(2, 7, 3, uint32(shstrtabOff), uint32(len(shstrtab)))
	copy(raw[shstrtabOff:], shstrtab)
	path := filepath.Join(t.TempDir(), "hello.elf")
	if err := os.WriteFile(path, raw, 0o644); err != nil {
		t.Fatal(err)
	}
	var out, errb bytes.Buffer
	code := Run(context.Background(), []string{"ps2", "elf-info", path}, &out, &errb)
	if code != ExitOK {
		t.Fatalf("exit %d %s", code, errb.String())
	}
	if !strings.Contains(out.String(), "text_bytes=16") {
		t.Fatalf("body %s", out.String())
	}
	out.Reset()
	errb.Reset()
	code = Run(context.Background(), []string{"--json", "ps2", "elf-info", path}, &out, &errb)
	if code != ExitOK {
		t.Fatalf("json exit %d %s", code, errb.String())
	}
	var m map[string]any
	if err := json.Unmarshal(out.Bytes(), &m); err != nil {
		t.Fatal(err)
	}
	if int(m["text_bytes"].(float64)) != 16 {
		t.Fatalf("%v", m)
	}
	code = Run(context.Background(), []string{"ps1", "elf-info", path}, &bytes.Buffer{}, &bytes.Buffer{})
	if code != ExitUsage {
		t.Fatalf("ps1 elf-info exit %d", code)
	}
}

func plantInterDVD(t *testing.T, root string) {
	t.Helper()
	v := filepath.Join(root, "VIDEO_TS")
	if err := os.MkdirAll(v, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(v, "VIDEO_TS.IFO"), []byte("IFO"), 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestInterDVDISOAndMeta(t *testing.T) {
	src := t.TempDir()
	plantInterDVD(t, src)
	isoPath := filepath.Join(t.TempDir(), "game.iso")
	extra := filepath.Join(t.TempDir(), "note.txt")
	if err := os.WriteFile(extra, []byte("hi"), 0o644); err != nil {
		t.Fatal(err)
	}
	var out, errb bytes.Buffer
	code := Run(context.Background(), []string{
		"--json", "interdvd", "iso",
		"--dir", src, "--out", isoPath,
		"--title", "CLI Title", "--license", "MIT",
		"--extra", extra + ":NOTES.TXT",
	}, &out, &errb)
	if code != ExitOK {
		t.Fatalf("iso %d %s %s", code, errb.String(), out.String())
	}
	var m map[string]any
	if err := json.Unmarshal(out.Bytes(), &m); err != nil {
		t.Fatal(err)
	}
	if m["format"] != "iso9660+udf" || m["title"] != "CLI Title" {
		t.Fatalf("%v", m)
	}

	metaPath := filepath.Join(t.TempDir(), "disc.interdvd.json")
	out.Reset()
	errb.Reset()
	code = Run(context.Background(), []string{"--json", "interdvd", "meta", "init", "--out", metaPath}, &out, &errb)
	if code != ExitOK {
		t.Fatalf("init %d %s", code, errb.String())
	}
	b, err := os.ReadFile(metaPath)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(b), "blazium.interdvd.meta/v1") {
		t.Fatalf("%s", b)
	}

	// Fill dir/out and validate + master via --meta with title override.
	var doc map[string]any
	if err := json.Unmarshal(b, &doc); err != nil {
		t.Fatal(err)
	}
	doc["dir"] = src
	doc["out"] = filepath.Join(t.TempDir(), "frommeta.iso")
	nb, _ := json.Marshal(doc)
	if err := os.WriteFile(metaPath, nb, 0o644); err != nil {
		t.Fatal(err)
	}
	out.Reset()
	errb.Reset()
	code = Run(context.Background(), []string{"interdvd", "meta", "validate", "--meta", metaPath}, &out, &errb)
	if code != ExitOK {
		t.Fatalf("validate %d %s", code, errb.String())
	}
	out.Reset()
	errb.Reset()
	writeMeta := filepath.Join(t.TempDir(), "saved.json")
	code = Run(context.Background(), []string{
		"--json", "interdvd", "iso", "--meta", metaPath, "--title", "Override", "--write-meta", writeMeta,
	}, &out, &errb)
	if code != ExitOK {
		t.Fatalf("meta iso %d %s", code, errb.String())
	}
	if err := json.Unmarshal(out.Bytes(), &m); err != nil {
		t.Fatal(err)
	}
	if m["title"] != "Override" {
		t.Fatalf("%v", m)
	}
	saved, err := os.ReadFile(writeMeta)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(saved), "Override") {
		t.Fatalf("%s", saved)
	}

	bad := t.TempDir()
	badMeta := filepath.Join(t.TempDir(), "bad.json")
	if err := os.WriteFile(badMeta, []byte(`{"schema":"blazium.interdvd.meta/v1","dir":"`+strings.ReplaceAll(bad, `\`, `\\`)+`"}`), 0o644); err != nil {
		t.Fatal(err)
	}
	code = Run(context.Background(), []string{"interdvd", "meta", "validate", "--meta", badMeta}, &bytes.Buffer{}, &bytes.Buffer{})
	if code == ExitOK {
		t.Fatal("validate should fail without IFO")
	}

	code = Run(context.Background(), []string{"ps1", "iso"}, &bytes.Buffer{}, &bytes.Buffer{})
	if code != ExitUsage {
		t.Fatalf("ps1 iso without xml exit %d", code)
	}
}

func TestListTextMentionsPS2Host(t *testing.T) {
	var out bytes.Buffer
	code := Run(context.Background(), []string{"list"}, &out, &bytes.Buffer{})
	if code != ExitOK {
		t.Fatalf("exit %d", code)
	}
	body := out.String()
	if !strings.Contains(body, "ps2") {
		t.Fatalf("list missing ps2: %s", body)
	}
	if !strings.Contains(body, "Windows/Linux") && !strings.Contains(body, "compile/run unavailable") {
		t.Fatalf("list must mention PS2 host restriction: %s", body)
	}
}

func TestExportGuestWritesCPP(t *testing.T) {
	dir := t.TempDir()
	dest := filepath.Join(dir, "guest")
	var out, errb bytes.Buffer
	code := Run(context.Background(), []string{"--json", "ps1", "export-guest", "--out", dest}, &out, &errb)
	if code != ExitOK {
		t.Fatalf("export-guest %d %s", code, errb.String())
	}
	var m map[string]any
	if err := json.Unmarshal(out.Bytes(), &m); err != nil {
		t.Fatal(err)
	}
	if m["out"] != dest {
		t.Fatalf("%v", m)
	}
	for _, name := range []string{"CMakeLists.txt", "main.cpp", "script_vm.cpp", "script_vm.h", "fmv_play.cpp"} {
		if _, err := os.Stat(filepath.Join(dest, name)); err != nil {
			t.Fatalf("missing %s: %v", name, err)
		}
	}
	cmake, err := os.ReadFile(filepath.Join(dest, "CMakeLists.txt"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(cmake), "extra/*.cpp") {
		t.Fatalf("CMakeLists missing extra glob: %s", cmake)
	}
}

func TestN64IsSupportedNotPlanned(t *testing.T) {
	var errBuf bytes.Buffer
	code := Run(context.Background(), []string{"n64", "setup", "--offline"}, &bytes.Buffer{}, &errBuf)
	if code == ExitPlanned {
		t.Fatalf("n64 should not be planned: %s", errBuf.String())
	}
	if code != ExitOK && code != ExitTool {
		t.Fatalf("exit %d body %s", code, errBuf.String())
	}
}

func TestExportGuestWritesN64(t *testing.T) {
	dir := t.TempDir()
	dest := filepath.Join(dir, "guest")
	var out, errb bytes.Buffer
	code := Run(context.Background(), []string{"--json", "n64", "export-guest", "--out", dest}, &out, &errb)
	if code != ExitOK {
		t.Fatalf("export-guest %d %s", code, errb.String())
	}
	var m map[string]any
	if err := json.Unmarshal(out.Bytes(), &m); err != nil {
		t.Fatal(err)
	}
	if m["out"] != dest {
		t.Fatalf("%v", m)
	}
	if int(m["abi"].(float64)) != 1 {
		t.Fatalf("abi %v", m["abi"])
	}
	for _, name := range []string{"CMakeLists.txt", "Makefile", "main.cpp", "rdpq_draw.cpp", "script_vm.cpp", "dfs_io.cpp"} {
		if _, err := os.Stat(filepath.Join(dest, name)); err != nil {
			t.Fatalf("missing %s: %v", name, err)
		}
	}
	main, err := os.ReadFile(filepath.Join(dest, "main.cpp"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(main), "BLAZIUM_N64_COOK_ABI") {
		t.Fatalf("main.cpp missing cook ABI: %s", main)
	}
}

func TestN64ISOUsage(t *testing.T) {
	code := Run(context.Background(), []string{"n64", "iso", "--dir", "x", "--out", "y.iso"}, &bytes.Buffer{}, &bytes.Buffer{})
	if code != ExitUsage {
		t.Fatalf("n64 iso exit %d", code)
	}
}

func TestExportGuestWritesPS2(t *testing.T) {
	dir := t.TempDir()
	dest := filepath.Join(dir, "guest")
	var out, errb bytes.Buffer
	code := Run(context.Background(), []string{"--json", "ps2", "export-guest", "--out", dest}, &out, &errb)
	if code != ExitOK {
		t.Fatalf("export-guest %d %s", code, errb.String())
	}
	var m map[string]any
	if err := json.Unmarshal(out.Bytes(), &m); err != nil {
		t.Fatal(err)
	}
	if m["out"] != dest {
		t.Fatalf("%v", m)
	}
	for _, name := range []string{"CMakeLists.txt", "Makefile", "main.cpp", "vu1_draw.cpp", "draw_3D.vsm"} {
		if _, err := os.Stat(filepath.Join(dest, name)); err != nil {
			t.Fatalf("missing %s: %v", name, err)
		}
	}
	main, err := os.ReadFile(filepath.Join(dest, "main.cpp"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(main), "BLAZIUM_PS2_COOK_ABI") {
		t.Fatalf("main.cpp missing cook ABI: %s", main)
	}
}

func TestInterDVDEnvStatusAndMissingFFmpeg(t *testing.T) {
	prefix := t.TempDir()
	var out, errb bytes.Buffer
	code := Run(context.Background(), []string{"--prefix", prefix, "--json", "interdvd", "env"}, &out, &errb)
	if code != ExitOK {
		t.Fatalf("env %d %s", code, errb.String())
	}
	var env map[string]string
	if err := json.Unmarshal(out.Bytes(), &env); err != nil {
		t.Fatal(err)
	}
	if env["ISO_TOOL"] != "builtin" || env["SCHEMA"] != "blazium.interdvd.meta/v1" {
		t.Fatalf("%v", env)
	}
	out.Reset()
	errb.Reset()
	code = Run(context.Background(), []string{"--prefix", prefix, "--json", "interdvd", "status"}, &out, &errb)
	if code != ExitOK {
		t.Fatalf("status %d %s", code, errb.String())
	}
	var st map[string]any
	if err := json.Unmarshal(out.Bytes(), &st); err != nil {
		t.Fatal(err)
	}
	if ready, _ := st["encode_ready"].(bool); ready {
		t.Fatalf("encode_ready %s", out.String())
	}
	if ready, _ := st["iso_ready"].(bool); !ready {
		t.Fatalf("iso_ready %s", out.String())
	}
	errb.Reset()
	code = Run(context.Background(), []string{"--prefix", prefix, "interdvd", "ffmpeg", "--", "-version"}, &bytes.Buffer{}, &errb)
	if code != ExitTool {
		t.Fatalf("ffmpeg missing exit %d %s", code, errb.String())
	}
	errb.Reset()
	code = Run(context.Background(), []string{"--prefix", prefix, "interdvd", "ffprobe", "--", "-version"}, &bytes.Buffer{}, &errb)
	if code != ExitTool {
		t.Fatalf("ffprobe missing exit %d %s", code, errb.String())
	}
}
