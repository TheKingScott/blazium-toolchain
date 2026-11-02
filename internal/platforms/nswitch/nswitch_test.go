package nswitch

import (
	"archive/zip"
	"bytes"
	"context"
	"encoding/binary"
	"image"
	"image/color"
	"image/jpeg"
	"image/png"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/blazium-games/blazium-toolchain/internal/platforms"
)

func TestInfo(t *testing.T) {
	info := New().Info()
	if info.ID != "switch" || info.Status != platforms.StatusSupported {
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

// readRomFS lists a RomFS's files by path, walking its tables as libnx's
// romfs device does (and finding each entry on its hash chain).
func readRomFS(t *testing.T, b []byte) map[string][]byte {
	t.Helper()
	le := binary.LittleEndian
	if le.Uint64(b[0:]) != romfsHdrSize || le.Uint64(b[0x48:]) != romfsDataOfs {
		t.Fatalf("header: % x", b[:0x50])
	}
	dh, dhs := le.Uint64(b[0x08:]), le.Uint64(b[0x10:])
	dt := le.Uint64(b[0x18:])
	fh, fhs := le.Uint64(b[0x28:]), le.Uint64(b[0x30:])
	ft := le.Uint64(b[0x38:])
	out := map[string][]byte{}
	var walk func(d uint32, pre string)
	walk = func(d uint32, pre string) {
		e := b[dt+uint64(d):]
		n := le.Uint32(e[20:])
		name := pre + string(e[24:24+n])
		if d != 0 {
			h := romHash(le.Uint32(e[0:]), string(e[24:24+n]))
			found := false
			for c := le.Uint32(b[dh+uint64(h%uint32(dhs/4))*4:]); c != romfsEmpty; c = le.Uint32(b[dt+uint64(c)+16:]) {
				found = found || c == d
			}
			if !found {
				t.Errorf("directory %s is not on its hash chain", name)
			}
			name += "/"
		}
		for f := le.Uint32(e[12:]); f != romfsEmpty; {
			fe := b[ft+uint64(f):]
			fn := string(fe[32 : 32+le.Uint32(fe[28:])])
			h := romHash(d, fn)
			found := false
			for c := le.Uint32(b[fh+uint64(h%uint32(fhs/4))*4:]); c != romfsEmpty; c = le.Uint32(b[ft+uint64(c)+24:]) {
				found = found || c == f
			}
			if !found {
				t.Errorf("file %s%s is not on its hash chain", name, fn)
			}
			o := romfsDataOfs + le.Uint64(fe[8:])
			if o%0x10 != 0 {
				t.Errorf("%s%s at %#x is not aligned", name, fn, o)
			}
			out[name+fn] = b[o : o+le.Uint64(fe[16:])]
			f = le.Uint32(fe[4:])
		}
		for c := le.Uint32(e[8:]); c != romfsEmpty; c = le.Uint32(b[dt+uint64(c)+4:]) {
			walk(c, name)
		}
	}
	walk(0, "")
	return out
}

func TestRomFS(t *testing.T) {
	files := []romFile{
		{"DATA/SCRP00.BIN", bytes.Repeat([]byte{7}, 33)},
		{"DATA/MESH00.BIN", []byte("mesh")},
		{"APPDIR.TXT", []byte("penguin")},
		{"a-b/x", []byte("1")},
		{"a/b/y", []byte("2")},
	}
	b, err := RomFS(files)
	if err != nil {
		t.Fatal(err)
	}
	got := readRomFS(t, b)
	if len(got) != len(files) {
		t.Fatalf("%d files: %v", len(got), got)
	}
	for _, f := range files {
		if !bytes.Equal(got[f.Path], f.Data) {
			t.Errorf("%s: %q, want %q", f.Path, got[f.Path], f.Data)
		}
	}
	for _, bad := range []string{"/abs", "dir/", "a\\b", "a//b"} {
		if _, err := RomFS([]romFile{{bad, nil}}); err == nil {
			t.Errorf("%q should fail", bad)
		}
	}
	if _, err := RomFS([]romFile{{"x", nil}, {"x", nil}}); err == nil {
		t.Error("a path twice should fail")
	}
}

func TestNACP(t *testing.T) {
	b := NACP("Penguin Kart", "me", "1.2.3")
	if len(b) != 0x4000 {
		t.Fatalf("%d bytes", len(b))
	}
	for i := 0; i < 12; i++ {
		if !bytes.HasPrefix(b[i*0x300:], []byte("Penguin Kart\x00")) || !bytes.HasPrefix(b[i*0x300+0x200:], []byte("me\x00")) {
			t.Fatalf("language %d", i)
		}
	}
	if !bytes.HasPrefix(b[0x3060:], []byte("1.2.3\x00")) || binary.LittleEndian.Uint32(b[0x30F0:]) != 0x102 {
		t.Fatal("version or fixed fields")
	}
	long := NACP(strings.Repeat("x", 0x300), "a", "1")
	if long[0x1ff] != 0 || long[0x1fe] != 'x' {
		t.Fatal("a long name should stop one short of its field")
	}
}

// fakeNRO is an NRO header and body of n bytes, with junk after it
func fakeNRO(n int) []byte {
	b := make([]byte, n+16)
	copy(b[0x10:], "NRO0")
	binary.LittleEndian.PutUint32(b[0x18:], uint32(n))
	for i := n; i < len(b); i++ {
		b[i] = 0xee
	}
	return b
}

func TestWithAssets(t *testing.T) {
	out, err := WithAssets(fakeNRO(0x100), []byte("ICON"), []byte("NACP!"), []byte("RFS"))
	if err != nil {
		t.Fatal(err)
	}
	le := binary.LittleEndian
	a := out[0x100:]
	if string(a[:4]) != "ASET" || le.Uint32(a[4:]) != 0 {
		t.Fatalf("% x", a[:8])
	}
	part := func(i int) string {
		o, n := le.Uint64(a[8+16*i:]), le.Uint64(a[16+16*i:])
		return string(a[o : o+n])
	}
	if part(0) != "ICON" || part(1) != "NACP!" || part(2) != "RFS" || len(out) != 0x100+0x38+12 {
		t.Fatalf("parts %q %q %q, %d bytes", part(0), part(1), part(2), len(out))
	}
	if _, err := WithAssets([]byte("not an nro at all, not an nro at all, not an nro at all, not an nro at all, not an nro at all, not an nro at all, not an nro at all"), nil, nil, nil); err == nil {
		t.Fatal("not an NRO should fail")
	}
}

func TestIcon(t *testing.T) {
	pic := image.NewNRGBA(image.Rect(0, 0, 300, 200))
	for y := 0; y < 200; y++ {
		for x := 0; x < 300; x++ {
			pic.SetNRGBA(x, y, color.NRGBA{200, 50, 50, 255})
		}
	}
	for _, p := range []image.Image{nil, pic} {
		raw, err := Icon(p)
		if err != nil {
			t.Fatal(err)
		}
		img, err := jpeg.Decode(bytes.NewReader(raw))
		if err != nil || img.Bounds().Dx() != 256 || img.Bounds().Dy() != 256 {
			t.Fatalf("icon: %v", err)
		}
	}
}

// A build needs no SDK: the bundled guest, the assets written here.
func TestBuildWithoutSDK(t *testing.T) {
	t.Setenv("DEVKITPRO", filepath.Join(t.TempDir(), "none"))
	t.Setenv("GDIR_EXTRA_FLAGS", "")
	export := t.TempDir()
	for _, f := range []string{"MESH00.bin", "SCRP00.bin", "DISP.bin", "PENGUIN.ELF"} {
		if err := os.WriteFile(filepath.Join(export, f), []byte(f), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	var buf bytes.Buffer
	if err := png.Encode(&buf, image.NewNRGBA(image.Rect(0, 0, 64, 64))); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(export, "ICON.PNG"), buf.Bytes(), 0o644); err != nil {
		t.Fatal(err)
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
	app := filepath.Join(out, "switch", "penguin")
	nro, err := os.ReadFile(filepath.Join(app, "penguin.nro"))
	if err != nil {
		t.Fatal(err)
	}
	le := binary.LittleEndian
	if string(nro[0x10:0x14]) != "NRO0" {
		t.Fatal("not an NRO")
	}
	a := nro[le.Uint32(nro[0x18:]):]
	if string(a[:4]) != "ASET" {
		t.Fatal("no assets after the program")
	}
	part := func(i int) []byte {
		o, n := le.Uint64(a[8+16*i:]), le.Uint64(a[16+16*i:])
		return a[o : o+n]
	}
	if _, err := jpeg.Decode(bytes.NewReader(part(0))); err != nil {
		t.Fatalf("icon: %v", err)
	}
	if !bytes.HasPrefix(part(1), []byte("Penguin Kart\x00")) || !bytes.HasPrefix(part(1)[0x200:], []byte(defaultAuthor)) {
		t.Fatal("NACP")
	}
	got := readRomFS(t, part(2))
	if string(got["APPDIR.TXT"]) != "penguin" || string(got["DATA/MESH00.BIN"]) != "MESH00.bin" ||
		string(got["DATA/SCRP00.BIN"]) != "SCRP00.bin" || got["DATA/PENGUIN.ELF"] != nil {
		t.Fatalf("RomFS: %v", got)
	}
	notices, err := os.ReadFile(filepath.Join(app, "THIRD-PARTY-NOTICES.txt"))
	if err != nil || !bytes.Contains(notices, []byte("libnx")) || !bytes.Contains(notices, []byte("deko3d")) || !bytes.Contains(notices, []byte("newlib")) {
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
	if strings.Join(names, ",") != "switch/penguin/THIRD-PARTY-NOTICES.txt,switch/penguin/penguin.nro" {
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
