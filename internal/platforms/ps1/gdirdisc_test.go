package ps1

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestSkipOnGDIRDisc(t *testing.T) {
	cases := map[string]bool{
		"MESH04.bin":   false,
		"MCSAVE.BIN":   false,
		"SCRP00.bin":   false,
		"BUDGET.txt":   true,
		"PENGUIN.ELF":  true,
		"SYSTEM.CNF":   true,
		"BOOTLOGO.RAW": true,
		"NAMES.MAP":    true,
	}
	for name, want := range cases {
		if got := skipOnGDIRDisc(name, 1000); got != want {
			t.Errorf("%s: skip %v, want %v", name, got, want)
		}
	}
	if !skipOnGDIRDisc("ANIM00.bin", 8) || skipOnGDIRDisc("ANIM00.bin", 400) {
		t.Error("empty sidecars only are skipped")
	}
}

func TestIsBootName(t *testing.T) {
	for s, want := range map[string]bool{"SLUS_000.01": true, "SCES_123.45": true, "GAME.EXE": false, "slus_000.01": false, "SLUS-000.01": false} {
		if isBootName(s) != want {
			t.Errorf("%s: %v", s, !want)
		}
	}
}

func TestGDIRDiscFilesAndXML(t *testing.T) {
	dir := t.TempDir()
	write := func(name string, b []byte) {
		if err := os.WriteFile(filepath.Join(dir, name), b, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	write("GAME.EXE", append([]byte(psxMagic), make([]byte, 2040)...))
	write("MESH00.bin", make([]byte, 64))
	write("MESH04.bin", make([]byte, 64))
	write("SCRP00.bin", make([]byte, 64))
	write("STREAM.bin", make([]byte, 41))
	write("TILE04.bin", make([]byte, 10))
	write("PENGUIN.ELF", make([]byte, 64))
	write("BUDGET.txt", []byte("x"))
	out := filepath.Join(dir, "GAME.bin")
	write("GAME.bin", make([]byte, 64))
	write("OLD.bin", append([]byte{0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0}, make([]byte, 2340)...))
	exe, files, err := gdirDiscFiles(dir, out)
	if err != nil {
		t.Fatal(err)
	}
	if filepath.Base(exe) != "GAME.EXE" {
		t.Fatalf("exe %q", exe)
	}
	if strings.Join(files, ",") != "MESH00.bin,MESH04.bin,SCRP00.bin,STREAM.bin" {
		t.Fatalf("files %v", files)
	}
	var up []string
	for _, f := range files {
		up = append(up, strings.ToUpper(f))
	}
	x := string(gdirISOXML("/o/G.bin", "/o/G.cue", "PENGUIN", "/s/SYSTEM.CNF", exe, "SLUS_000.01", "/s/data", up))
	for _, want := range []string{
		`<file name="SLUS_000.01" source="` + filepath.ToSlash(exe) + `"`,
		`<dir name="DATA">`,
		`<file name="STREAM.BIN" source="/s/data/STREAM.BIN"`,
		`<dir name="P00">`,
		`<dir name="P04">`,
		`volume="PENGUIN"`,
	} {
		if !strings.Contains(x, want) {
			t.Errorf("xml lacks %s:\n%s", want, x)
		}
	}
	if strings.Index(x, `name="MESH04.BIN"`) < strings.Index(x, `<dir name="P04">`) {
		t.Error("MESH04 is not under P04")
	}
	if systemCNF("SLUS_000.01") != "BOOT = cdrom:\\SLUS_000.01;1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFFF0\r\n" {
		t.Error("SYSTEM.CNF")
	}
}

func TestBuildType(t *testing.T) {
	t.Setenv("BLAZIUM_PS1_BUILD_TYPE", "")
	if buildType("gdir") != "Release" || buildType("") != "Debug" || buildType("template") != "Debug" {
		t.Fatal("build types")
	}
	t.Setenv("BLAZIUM_PS1_BUILD_TYPE", "Debug")
	if buildType("gdir") != "Debug" {
		t.Fatal("override")
	}
}
