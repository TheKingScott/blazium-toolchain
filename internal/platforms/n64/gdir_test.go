package n64

import (
	"os"
	"path/filepath"
	"testing"
)

func TestGDIRDataName(t *testing.T) {
	for name, want := range map[string]bool{
		"MESH00.bin":   true,
		"SCRP00.BIN":   true,
		"BOOTLOGO.RAW": true,
		"BUDGET.txt":   false,
		"GAME.EXE":     false,
		"PENGUIN.ELF":  false,
		"game.z64":     false,
	} {
		if got := gdirDataName(name); got != want {
			t.Errorf("gdirDataName(%q) = %v, want %v", name, got, want)
		}
	}
}

func TestGDIRExportDir(t *testing.T) {
	out := filepath.Join(t.TempDir(), "x", "GAME.z64")
	got, err := gdirExportDir("", out)
	if err != nil || got != filepath.Dir(out) {
		t.Fatalf("beside --out: %q %v", got, err)
	}
	if _, err := gdirExportDir("", ""); err == nil {
		t.Fatal("no --out and no --pack-dir should fail")
	}
}

func TestStageGDIRNeedsMesh(t *testing.T) {
	export := t.TempDir()
	if err := os.WriteFile(filepath.Join(export, "SCRP00.bin"), []byte("SCRP"), 0o644); err != nil {
		t.Fatal(err)
	}
	src := filepath.Join(t.TempDir(), "gdir")
	if _, err := stageGDIR(src, export, nil); err == nil {
		t.Fatal("an export without MESH00 should fail")
	}
	if _, err := os.Stat(filepath.Join(src, "filesystem", "DATA", "SCRP00.BIN")); err != nil {
		t.Fatalf("SCRP00 not staged upper case: %v", err)
	}
	if _, err := os.Stat(filepath.Join(src, "Makefile")); err != nil {
		t.Fatalf("guest not installed: %v", err)
	}
}
