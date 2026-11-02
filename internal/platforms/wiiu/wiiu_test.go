package wiiu

import (
	"archive/zip"
	"bytes"
	"compress/gzip"
	"context"
	"encoding/binary"
	"image"
	"image/color"
	"image/png"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/blazium-games/blazium-toolchain/internal/platforms"
)

func TestInfo(t *testing.T) {
	info := New().Info()
	if info.ID != "wiiu" || info.Status != platforms.StatusSupported {
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

func TestHashCount(t *testing.T) {
	for n, want := range map[uint32]uint32{0: 3, 2: 3, 3: 3, 4: 5, 18: 19, 19: 19, 20: 23, 176: 179} {
		if got := hashCount(n); got != want {
			t.Errorf("%d: %d, want %d", n, got, want)
		}
	}
}

// readBundle lists a .wuhb's files by path, walking its tables as Cemu and
// the Wii U's loader do (a file looked up by its hash chain too).
func readBundle(t *testing.T, b []byte) map[string][]byte {
	t.Helper()
	be := binary.BigEndian
	if string(b[:4]) != "WUHB" || be.Uint32(b[4:]) != romfsHdrSize || be.Uint64(b[0x48:]) != romfsDataOfs {
		t.Fatalf("header: % x", b[:0x50])
	}
	dh, dhs := be.Uint64(b[0x08:]), be.Uint64(b[0x10:])
	dt := be.Uint64(b[0x18:])
	fh, fhs := be.Uint64(b[0x28:]), be.Uint64(b[0x30:])
	ft := be.Uint64(b[0x38:])
	out := map[string][]byte{}
	var walk func(d uint32, pre string)
	walk = func(d uint32, pre string) {
		e := b[dt+uint64(d):]
		n := be.Uint32(e[20:])
		name := pre + string(e[24:24+n])
		if d != 0 {
			// the directory is on its hash chain
			h := romHash(be.Uint32(e[0:]), string(e[24:24+n]))
			found := false
			for c := be.Uint32(b[dh+uint64(h%uint32(dhs/4))*4:]); c != romfsEmpty; c = be.Uint32(b[dt+uint64(c)+16:]) {
				found = found || c == d
			}
			if !found {
				t.Errorf("directory %s is not on its hash chain", name)
			}
			name += "/"
		}
		for f := be.Uint32(e[12:]); f != romfsEmpty; {
			fe := b[ft+uint64(f):]
			fn := string(fe[32 : 32+be.Uint32(fe[28:])])
			h := romHash(d, fn)
			found := false
			for c := be.Uint32(b[fh+uint64(h%uint32(fhs/4))*4:]); c != romfsEmpty; c = be.Uint32(b[ft+uint64(c)+24:]) {
				found = found || c == f
			}
			if !found {
				t.Errorf("file %s%s is not on its hash chain", name, fn)
			}
			o := romfsDataOfs + be.Uint64(fe[8:])
			if o%0x10 != 0 {
				t.Errorf("%s%s at %#x is not aligned", name, fn, o)
			}
			out[name+fn] = b[o : o+be.Uint64(fe[16:])]
			f = be.Uint32(fe[4:])
		}
		for c := be.Uint32(e[8:]); c != romfsEmpty; c = be.Uint32(b[dt+uint64(c)+4:]) {
			walk(c, name)
		}
	}
	walk(0, "")
	return out
}

func TestWUHB(t *testing.T) {
	files := []bundleFile{
		{"code/root.rpx", []byte("RPX")},
		{"meta/meta.ini", MetaINI("Penguin", "Penguin", "me")},
		{"content/DATA/MESH00.BIN", []byte("mesh")},
		{"content/DATA/SCRP00.BIN", bytes.Repeat([]byte{7}, 33)},
		{"content/APPDIR.TXT", []byte("penguin")},
	}
	b, err := WUHB(files)
	if err != nil {
		t.Fatal(err)
	}
	got := readBundle(t, b)
	if len(got) != len(files) {
		t.Fatalf("%d files: %v", len(got), got)
	}
	for _, f := range files {
		if !bytes.Equal(got[f.Path], f.Data) {
			t.Errorf("%s: %q, want %q", f.Path, got[f.Path], f.Data)
		}
	}
	if _, err := WUHB([]bundleFile{{"/abs", nil}}); err == nil {
		t.Fatal("an absolute path should fail")
	}
}

func TestMetaINI(t *testing.T) {
	if got := string(MetaINI("Penguin\nKart", "P", "A")); got != "[menu]\nlongname=Penguin Kart\nshortname=P\nauthor=A\n" {
		t.Fatalf("%q", got)
	}
}

func TestTGA(t *testing.T) {
	img := image.NewNRGBA(image.Rect(0, 0, 2, 2))
	img.SetNRGBA(0, 0, color.NRGBA{1, 2, 3, 4})   // top left
	img.SetNRGBA(1, 1, color.NRGBA{9, 8, 7, 255}) // bottom right
	b := TGA(img, 32)
	if b[2] != 2 || b[12] != 2 || b[14] != 2 || b[16] != 32 || b[17] != 8 || len(b) != 18+16 {
		t.Fatalf("header % x", b[:18])
	}
	// rows from the bottom: bottom right is the second pixel, top left the third
	if !bytes.Equal(b[18+4:18+8], []byte{7, 8, 9, 255}) || !bytes.Equal(b[18+8:18+12], []byte{3, 2, 1, 4}) {
		t.Fatalf("pixels % x", b[18:])
	}
	if b24 := TGA(img, 24); len(b24) != 18+12 || b24[16] != 24 || b24[17] != 0 {
		t.Fatalf("24-bit: % x", b24[:18])
	}
}

func TestMetaFiles(t *testing.T) {
	src := filepath.Join(t.TempDir(), "i.png")
	pic := image.NewNRGBA(image.Rect(0, 0, 300, 200))
	for y := 0; y < 200; y++ {
		for x := 0; x < 300; x++ {
			pic.SetNRGBA(x, y, color.NRGBA{200, 50, 50, 255})
		}
	}
	var buf bytes.Buffer
	if err := png.Encode(&buf, pic); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(src, buf.Bytes(), 0o644); err != nil {
		t.Fatal(err)
	}
	for _, p := range []string{"", src} {
		meta, err := MetaFiles("Penguin", "", p)
		if err != nil {
			t.Fatal(err)
		}
		want := map[string][3]int{"meta/iconTex.tga.gz": {128, 128, 32}, "meta/bootTvTex.tga.gz": {1280, 720, 24}, "meta/bootDrcTex.tga.gz": {854, 480, 24}}
		for _, f := range meta {
			w, ok := want[f.Path]
			if !ok {
				if f.Path != "meta/meta.ini" || !strings.Contains(string(f.Data), "author="+defaultAuthor) {
					t.Errorf("%s: %q", f.Path, f.Data)
				}
				continue
			}
			zr, err := gzip.NewReader(bytes.NewReader(f.Data))
			if err != nil {
				t.Fatal(err)
			}
			tga, _ := io.ReadAll(zr)
			le := binary.LittleEndian
			if int(le.Uint16(tga[12:])) != w[0] || int(le.Uint16(tga[14:])) != w[1] || int(tga[16]) != w[2] || len(tga) != 18+w[0]*w[1]*w[2]/8 {
				t.Errorf("%s: % x (%d bytes)", f.Path, tga[:18], len(tga))
			}
		}
	}
	// the same game makes the same bytes
	a, _ := MetaFiles("Penguin", "", src)
	b, _ := MetaFiles("Penguin", "", src)
	for i := range a {
		if !bytes.Equal(a[i].Data, b[i].Data) {
			t.Errorf("%s differs between builds", a[i].Path)
		}
	}
}

// A build needs no SDK: the bundled guest, the bundle written here.
func TestBuildWithoutSDK(t *testing.T) {
	t.Setenv("DEVKITPRO", filepath.Join(t.TempDir(), "none"))
	t.Setenv("GDIR_EXTRA_FLAGS", "")
	export := t.TempDir()
	for _, f := range []string{"MESH00.bin", "SCRP00.bin", "DISP.bin", "PENGUIN.ELF"} {
		if err := os.WriteFile(filepath.Join(export, f), []byte(f), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	out := t.TempDir()
	build := func() {
		err := New().Build(context.Background(), platforms.BuildOptions{CommonOptions: platforms.CommonOptions{Prefix: t.TempDir()},
			Out: filepath.Join(out, "PENGUIN.zip"), Sample: "gdir", PackDir: export, Title: "Penguin Kart"})
		if err != nil {
			t.Fatal(err)
		}
	}
	build()
	app := filepath.Join(out, "wiiu", "apps", "penguin")
	raw, err := os.ReadFile(filepath.Join(app, "penguin.wuhb"))
	if err != nil {
		t.Fatal(err)
	}
	got := readBundle(t, raw)
	rpx := got["code/root.rpx"]
	if len(rpx) < 0x40 || string(rpx[1:4]) != "ELF" || rpx[7] != 0xca || rpx[8] != 0xfe {
		t.Fatal("code/root.rpx is not the guest's RPX")
	}
	if string(got["content/APPDIR.TXT"]) != "penguin" || string(got["content/DATA/MESH00.BIN"]) != "MESH00.bin" ||
		string(got["content/DATA/SCRP00.BIN"]) != "SCRP00.bin" || got["content/DATA/PENGUIN.ELF"] != nil {
		t.Fatalf("content: %v", got)
	}
	if !strings.Contains(string(got["meta/meta.ini"]), "longname=Penguin Kart\n") {
		t.Fatalf("meta.ini: %q", got["meta/meta.ini"])
	}
	for _, f := range []string{"meta/iconTex.tga.gz", "meta/bootTvTex.tga.gz", "meta/bootDrcTex.tga.gz"} {
		if len(got[f]) == 0 {
			t.Errorf("no %s", f)
		}
	}
	notices, err := os.ReadFile(filepath.Join(app, "THIRD-PARTY-NOTICES.txt"))
	if err != nil || !bytes.Contains(notices, []byte("wut")) || !bytes.Contains(notices, []byte("newlib")) {
		t.Fatalf("notices: %v", err)
	}
	// a save stays over a rebuild, and the zip never holds one
	if err := os.WriteFile(filepath.Join(app, "BLAZIUM.SAV"), []byte("save"), 0o644); err != nil {
		t.Fatal(err)
	}
	build()
	if b, err := os.ReadFile(filepath.Join(app, "BLAZIUM.SAV")); err != nil || string(b) != "save" {
		t.Fatal("the rebuild lost the save")
	}
	zr, err := zip.OpenReader(filepath.Join(out, "PENGUIN.zip"))
	if err != nil {
		t.Fatal(err)
	}
	defer zr.Close()
	var names []string
	for _, f := range zr.File {
		names = append(names, f.Name)
	}
	if strings.Join(names, ",") != "wiiu/apps/penguin/THIRD-PARTY-NOTICES.txt,wiiu/apps/penguin/penguin.wuhb" {
		t.Fatalf("zip: %v", names)
	}
	if _, err := os.Stat(filepath.Join(out, "PENGUIN.THIRD-PARTY-NOTICES.txt")); err != nil {
		t.Fatal(err)
	}
}

func TestBuildNeedsGDIR(t *testing.T) {
	err := New().Build(context.Background(), platforms.BuildOptions{Out: filepath.Join(t.TempDir(), "x.zip")})
	if err == nil {
		t.Fatal("build without --sample gdir should fail")
	}
	err = New().Build(context.Background(), platforms.BuildOptions{Out: filepath.Join(t.TempDir(), "x.zip"), Sample: "gdir", TitleID: "Bad Name"})
	if err == nil {
		t.Fatal("an upper-case --title-id should fail")
	}
}

func TestRunNeedsGame(t *testing.T) {
	if err := New().Run(context.Background(), platforms.RunOptions{}); err == nil {
		t.Fatal("run without a game should fail")
	}
}
