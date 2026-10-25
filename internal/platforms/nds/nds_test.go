package nds

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/blazium-games/blazium-toolchain/internal/platforms"
)

func TestInfo(t *testing.T) {
	info := New().Info()
	if info.ID != "nds" || info.Status != platforms.StatusSupported {
		t.Fatalf("info: %+v", info)
	}
}

func TestBuildNeedsSample(t *testing.T) {
	t.Setenv("BLOCKSDS", "")
	tool := New()
	err := tool.Build(context.Background(), platforms.BuildOptions{CommonOptions: platforms.CommonOptions{Prefix: t.TempDir()}, Out: "x.nds"})
	if err == nil {
		t.Fatal("build without tools or --sample should fail")
	}
}

func TestGDIRDataName(t *testing.T) {
	for name, want := range map[string]bool{"MESH00.bin": true, "BOOTLOGO.RAW": true, "PENGUIN.ELF": false, "BUDGET.txt": false} {
		if got := gdirDataName(name); got != want {
			t.Errorf("%s: %v, want %v", name, got, want)
		}
	}
}

func TestStageGDIRNeedsMesh(t *testing.T) {
	export := t.TempDir()
	if err := os.WriteFile(filepath.Join(export, "SCRP00.bin"), []byte("SCRP"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := stageGDIR(filepath.Join(t.TempDir(), "src"), export, nil); err == nil {
		t.Fatal("an export without MESH00 should fail")
	}
}

func TestGDIRExportDir(t *testing.T) {
	if _, err := gdirExportDir("", ""); err == nil {
		t.Fatal("no --out and no --pack-dir should fail")
	}
	d, err := gdirExportDir("", "/a/b/game.nds")
	if err != nil || d != "/a/b" {
		t.Fatalf("got %q %v", d, err)
	}
}

func TestNotices(t *testing.T) {
	dir := t.TempDir()
	rom := filepath.Join(dir, "GAME.nds")
	path, err := writeNotices(rom, "")
	if err != nil {
		t.Fatal(err)
	}
	if path != filepath.Join(dir, "GAME.THIRD-PARTY-NOTICES.txt") {
		t.Fatalf("path %s", path)
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"GAME.nds", "libnds", "FatFs", "MPL-2.0", "maxmod", "DSWiFi", "picolibc", "Mozilla Public License"} {
		if !strings.Contains(string(raw), want) {
			t.Errorf("notices lack %q", want)
		}
	}
}

func TestDSiInfo(t *testing.T) {
	tool := NewDSi()
	info := tool.Info()
	if info.ID != "dsi" || info.Status != platforms.StatusSupported {
		t.Fatalf("info: %+v", info)
	}
	if got := tool.guestDir("/p"); got != filepath.Join("/p", "dsi", "guest", "gdir") && !strings.HasSuffix(got, filepath.Join("dsi", "guest", "gdir")) {
		t.Fatalf("guest dir %s", got)
	}
}

func TestMarkDSiOnly(t *testing.T) {
	rom := make([]byte, 0x200)
	rom[0x12] = 2
	if err := markDSiOnly(rom); err != nil {
		t.Fatal(err)
	}
	if rom[0x12] != 3 {
		t.Fatalf("unit code %d", rom[0x12])
	}
	got := uint16(rom[0x15e]) | uint16(rom[0x15f])<<8
	if got != headerCRC(rom[:0x15e]) {
		t.Fatal("header CRC not fixed")
	}
	nds := make([]byte, 0x200)
	if err := markDSiOnly(nds); err == nil {
		t.Fatal("a DS-only ROM should not be marked")
	}
}
