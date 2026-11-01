package psp

import (
	"archive/zip"
	"bytes"
	"context"
	"encoding/binary"
	"image/png"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"

	guest "github.com/blazium-games/blazium-toolchain/guest/psp"
	"github.com/blazium-games/blazium-toolchain/internal/platforms"
)

func TestInfo(t *testing.T) {
	info := New().Info()
	if info.ID != "psp" || info.Status != platforms.StatusSupported {
		t.Fatalf("info: %+v", info)
	}
}

func TestGameName(t *testing.T) {
	for in, want := range map[string]string{"Penguin Arcade Kart": "PENGUINARCADEKART", "penguin": "PENGUIN", "MY-GAME_2": "MY-GAME_2", "!!!": "BLAZIUMGAME"} {
		if got := gameName(in); got != want {
			t.Errorf("%q: %q, want %q", in, got, want)
		}
	}
}

func TestPatchGameDir(t *testing.T) {
	prx := append(append([]byte("xx"), []byte(guest.AppDirMarker)...), make([]byte, 80)...)
	out, err := PatchGameDir(prx, "PENGUIN")
	if err != nil {
		t.Fatal(err)
	}
	i := len("xx") + len(guest.AppDirMarker)
	if string(out[i:i+8]) != "PENGUIN\x00" || len(out) != len(prx) {
		t.Fatalf("patched: %q", out[i:i+10])
	}
	if _, err := PatchGameDir(prx, strings.Repeat("A", 60)); err == nil {
		t.Fatal("a name past the field should fail")
	}
	if _, err := PatchGameDir([]byte("no marker"), "X"); err == nil {
		t.Fatal("a PRX without the field should fail")
	}
}

func TestParamSFO(t *testing.T) {
	sfo := ParamSFO("Penguin Kart")
	if string(sfo[:4]) != "\x00PSF" || binary.LittleEndian.Uint32(sfo[16:]) != 9 {
		t.Fatal("bad SFO header")
	}
	for _, want := range []string{"BOOTABLE", "CATEGORY", "MG", "TITLE", "Penguin Kart", "PSP_SYSTEM_VER"} {
		if !bytes.Contains(sfo, []byte(want)) {
			t.Errorf("SFO has no %q", want)
		}
	}
}

func TestEbootPBP(t *testing.T) {
	pbp := EbootPBP([]byte("SFO!"), []byte("PNG!!"), []byte("PRX"))
	le := binary.LittleEndian
	if string(pbp[:4]) != "\x00PBP" {
		t.Fatal("magic")
	}
	off := func(i int) uint32 { return le.Uint32(pbp[8+i*4:]) }
	if string(pbp[off(0):off(1)]) != "SFO!" || string(pbp[off(1):off(2)]) != "PNG!!" || string(pbp[off(6):off(7)]) != "PRX" {
		t.Fatalf("parts: %v", pbp[:0x28])
	}
	if off(2) != off(6) || int(off(7)) != len(pbp) {
		t.Fatal("absent parts")
	}
}

func TestIcon(t *testing.T) {
	raw, err := Icon("")
	if err != nil {
		t.Fatal(err)
	}
	img, err := png.Decode(bytes.NewReader(raw))
	if err != nil || img.Bounds().Dx() != 144 || img.Bounds().Dy() != 80 {
		t.Fatalf("icon: %v %v", err, img.Bounds())
	}
	// from a picture of another shape
	src := filepath.Join(t.TempDir(), "i.png")
	if err := os.WriteFile(src, raw, 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := Icon(src); err != nil {
		t.Fatal(err)
	}
}

// A build needs no SDK: the bundled guest, the app written here.
func TestBuildWithoutSDK(t *testing.T) {
	none := filepath.Join(t.TempDir(), "none")
	t.Setenv("PSPDEV", none)
	t.Setenv("GDIR_EXTRA_FLAGS", "")
	export := t.TempDir()
	for _, f := range []string{"MESH00.bin", "SCRP00.bin", "DISP.bin", "PENGUIN.ELF"} {
		if err := os.WriteFile(filepath.Join(export, f), []byte(f), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	out := t.TempDir()
	err := New().Build(context.Background(), platforms.BuildOptions{CommonOptions: platforms.CommonOptions{Prefix: t.TempDir()},
		Out: filepath.Join(out, "PENGUIN.zip"), Sample: "gdir", PackDir: export, Title: "Penguin"})
	if err != nil {
		t.Fatal(err)
	}
	app := filepath.Join(out, "PSP", "GAME", "PENGUIN")
	for _, f := range []string{"EBOOT.PBP", "THIRD-PARTY-NOTICES.txt", "DATA/MESH00.BIN", "DATA/SCRP00.BIN"} {
		if _, err := os.Stat(filepath.Join(app, filepath.FromSlash(f))); err != nil {
			t.Errorf("%s: %v", f, err)
		}
	}
	if _, err := os.Stat(filepath.Join(app, "DATA", "PENGUIN.ELF")); err == nil {
		t.Error("the PS2 build went into DATA")
	}
	pbp, _ := os.ReadFile(filepath.Join(app, "EBOOT.PBP"))
	if len(pbp) < 0x28 || string(pbp[:4]) != "\x00PBP" || !bytes.Contains(pbp, []byte(guest.AppDirMarker+"PENGUIN\x00")) {
		t.Error("EBOOT.PBP is not one, or does not name its folder")
	}
	zr, err := zip.OpenReader(filepath.Join(out, "PENGUIN.zip"))
	if err != nil {
		t.Fatal(err)
	}
	defer zr.Close()
	names := map[string]bool{}
	for _, f := range zr.File {
		names[f.Name] = true
		if f.Name == "PSP/GAME/PENGUIN/DATA/MESH00.BIN" {
			r, _ := f.Open()
			b, _ := io.ReadAll(r)
			r.Close()
			if string(b) != "MESH00.bin" {
				t.Error("MESH00.BIN in the zip")
			}
		}
	}
	for _, want := range []string{"PSP/GAME/PENGUIN/EBOOT.PBP", "PSP/GAME/PENGUIN/DATA/MESH00.BIN"} {
		if !names[want] {
			t.Errorf("zip has no %s (%v)", want, names)
		}
	}
	if _, err := os.Stat(filepath.Join(out, "PENGUIN.THIRD-PARTY-NOTICES.txt")); err != nil {
		t.Fatal(err)
	}

	// a build over the folder keeps the save, and the zip has none
	if err := os.WriteFile(filepath.Join(app, "BLAZIUM.SAV"), []byte("save"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(app, "DATA", "OLD.BIN"), []byte("old"), 0o644); err != nil {
		t.Fatal(err)
	}
	err = New().Build(context.Background(), platforms.BuildOptions{CommonOptions: platforms.CommonOptions{Prefix: t.TempDir()},
		Out: filepath.Join(out, "PENGUIN.zip"), Sample: "gdir", PackDir: export, Title: "Penguin"})
	if err != nil {
		t.Fatal(err)
	}
	if b, _ := os.ReadFile(filepath.Join(app, "BLAZIUM.SAV")); string(b) != "save" {
		t.Error("a rebuild lost the save")
	}
	if _, err := os.Stat(filepath.Join(app, "DATA", "OLD.BIN")); err == nil {
		t.Error("a rebuild kept an old DATA file")
	}
	zr2, err := zip.OpenReader(filepath.Join(out, "PENGUIN.zip"))
	if err != nil {
		t.Fatal(err)
	}
	defer zr2.Close()
	for _, f := range zr2.File {
		if strings.HasSuffix(f.Name, "BLAZIUM.SAV") {
			t.Error("the zip has the save")
		}
	}
}

func TestBuildNeedsMesh(t *testing.T) {
	err := New().Build(context.Background(), platforms.BuildOptions{CommonOptions: platforms.CommonOptions{Prefix: t.TempDir()},
		Out: filepath.Join(t.TempDir(), "G.zip"), Sample: "gdir", PackDir: t.TempDir()})
	if err == nil {
		t.Fatal("an export without MESH00.bin should fail")
	}
}

func TestBadAppName(t *testing.T) {
	err := New().Build(context.Background(), platforms.BuildOptions{CommonOptions: platforms.CommonOptions{Prefix: t.TempDir()},
		Out: filepath.Join(t.TempDir(), "G.zip"), Sample: "gdir", PackDir: t.TempDir(), TitleID: "Bad Name"})
	if err == nil {
		t.Fatal("a folder name with lower case and a space should fail")
	}
}

func TestOverlayNeedsSDK(t *testing.T) {
	none := filepath.Join(t.TempDir(), "none")
	t.Setenv("PSPDEV", none)
	export := t.TempDir()
	if err := os.WriteFile(filepath.Join(export, "MESH00.bin"), []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	err := New().Build(context.Background(), platforms.BuildOptions{CommonOptions: platforms.CommonOptions{Prefix: t.TempDir()},
		Out: filepath.Join(t.TempDir(), "G.zip"), Sample: "gdir", PackDir: export, Overlay: t.TempDir()})
	if err == nil {
		t.Fatal("an overlay without pspdev should fail")
	}
}
