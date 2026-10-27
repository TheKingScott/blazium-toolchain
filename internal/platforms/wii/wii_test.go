package wii

import (
	"archive/zip"
	"bytes"
	"context"
	"image/png"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	guest "github.com/blazium-games/blazium-toolchain/guest/wii"
	"github.com/blazium-games/blazium-toolchain/internal/platforms"
)

func TestInfo(t *testing.T) {
	info := New().Info()
	if info.ID != "wii" || info.Status != platforms.StatusSupported {
		t.Fatalf("info: %+v", info)
	}
}

func TestAppName(t *testing.T) {
	for in, want := range map[string]string{"Penguin Arcade Kart": "penguinarcadekart", "PENGUIN": "penguin", "my-game_2": "my-game_2", "!!!": "blaziumgame"} {
		if got := appName(in); got != want {
			t.Errorf("%q: %q, want %q", in, got, want)
		}
	}
}

func TestPatchAppDir(t *testing.T) {
	dol := append(append([]byte("xx"), []byte(guest.AppDirMarker)...), make([]byte, 80)...)
	out, err := PatchAppDir(dol, "penguin")
	if err != nil {
		t.Fatal(err)
	}
	i := len("xx") + len(guest.AppDirMarker)
	if string(out[i:i+8]) != "penguin\x00" || len(out) != len(dol) {
		t.Fatalf("patched: %q", out[i:i+10])
	}
	if _, err := PatchAppDir(dol, strings.Repeat("a", 60)); err == nil {
		t.Fatal("a name past the field should fail")
	}
	if _, err := PatchAppDir([]byte("no marker"), "x"); err == nil {
		t.Fatal("a DOL without the field should fail")
	}
}

func TestMetaXML(t *testing.T) {
	m := string(MetaXML("Penguin & Kart", time.Date(2026, 10, 26, 12, 0, 0, 0, time.UTC)))
	for _, want := range []string{"<name>Penguin &amp; Kart</name>", "<coder>Blazium</coder>", "<release_date>20261026120000</release_date>", `<app version="1">`} {
		if !strings.Contains(m, want) {
			t.Errorf("meta.xml has no %s:\n%s", want, m)
		}
	}
}

func TestIcon(t *testing.T) {
	raw, err := Icon("")
	if err != nil {
		t.Fatal(err)
	}
	img, err := png.Decode(bytes.NewReader(raw))
	if err != nil || img.Bounds().Dx() != 128 || img.Bounds().Dy() != 48 {
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
	t.Setenv("DEVKITPRO", none)
	t.Setenv("DEVKITPPC", none)
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
	app := filepath.Join(out, "apps", "penguin")
	for _, f := range []string{"boot.dol", "meta.xml", "icon.png", "THIRD-PARTY-NOTICES.txt", "DATA/MESH00.BIN", "DATA/SCRP00.BIN"} {
		if _, err := os.Stat(filepath.Join(app, filepath.FromSlash(f))); err != nil {
			t.Errorf("%s: %v", f, err)
		}
	}
	if _, err := os.Stat(filepath.Join(app, "DATA", "PENGUIN.ELF")); err == nil {
		t.Error("the PS2 build went into DATA")
	}
	dol, _ := os.ReadFile(filepath.Join(app, "boot.dol"))
	if !bytes.Contains(dol, []byte(guest.AppDirMarker+"penguin\x00")) {
		t.Error("boot.dol does not name its folder")
	}
	zr, err := zip.OpenReader(filepath.Join(out, "PENGUIN.zip"))
	if err != nil {
		t.Fatal(err)
	}
	defer zr.Close()
	names := map[string]bool{}
	for _, f := range zr.File {
		names[f.Name] = true
		if f.Name == "apps/penguin/DATA/MESH00.BIN" {
			r, _ := f.Open()
			b, _ := io.ReadAll(r)
			r.Close()
			if string(b) != "MESH00.bin" {
				t.Error("MESH00.BIN in the zip")
			}
		}
	}
	for _, want := range []string{"apps/penguin/boot.dol", "apps/penguin/meta.xml", "apps/penguin/icon.png", "apps/penguin/DATA/MESH00.BIN"} {
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
		t.Fatal("a folder name with capitals and a space should fail")
	}
}

func TestOverlayNeedsSDK(t *testing.T) {
	none := filepath.Join(t.TempDir(), "none")
	t.Setenv("DEVKITPRO", none)
	t.Setenv("DEVKITPPC", none)
	export := t.TempDir()
	if err := os.WriteFile(filepath.Join(export, "MESH00.bin"), []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	err := New().Build(context.Background(), platforms.BuildOptions{CommonOptions: platforms.CommonOptions{Prefix: t.TempDir()},
		Out: filepath.Join(t.TempDir(), "G.zip"), Sample: "gdir", PackDir: export, Overlay: t.TempDir()})
	if err == nil {
		t.Fatal("an overlay without devkitPPC should fail")
	}
}

func TestRunPaths(t *testing.T) {
	root := t.TempDir()
	app := filepath.Join(root, "apps", "penguin")
	if err := os.MkdirAll(app, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(app, "boot.dol"), []byte("dol"), 0o644); err != nil {
		t.Fatal(err)
	}
	for _, in := range []string{root, app, filepath.Join(app, "boot.dol")} {
		dol, sd, err := runPaths(in)
		if err != nil || dol != filepath.Join(app, "boot.dol") || sd != root {
			t.Errorf("%s: %s %s %v", in, dol, sd, err)
		}
	}
}
