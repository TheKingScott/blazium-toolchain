package gamecube

import (
	"bytes"
	"context"
	"encoding/binary"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/blazium-games/blazium-toolchain/internal/platforms"
)

func TestInfo(t *testing.T) {
	info := New().Info()
	if info.ID != "gamecube" || info.Status != platforms.StatusSupported {
		t.Fatalf("info: %+v", info)
	}
}

// readGCM finds a file of a disc image by its path, through the FST as
// the guest's disc_io does.
func readGCM(t *testing.T, img []byte, want string) []byte {
	t.Helper()
	be := binary.BigEndian.Uint32
	fstOff, fstSize := be(img[0x424:]), be(img[0x428:])
	fst := img[fstOff : fstOff+fstSize]
	n := be(fst[8:])
	names := fst[n*12:]
	name := func(i uint32) string {
		o := be(fst[i*12:]) & 0xffffff
		return string(names[o : o+uint32(bytes.IndexByte(names[o:], 0))])
	}
	dir, end := uint32(0), n
	parts := strings.Split(want, "/")
	for pi, p := range parts {
		found := uint32(0)
		for i := dir + 1; i < end; {
			if strings.EqualFold(name(i), p) {
				found = i
				break
			}
			if fst[i*12]&1 != 0 {
				i = be(fst[i*12+8:])
			} else {
				i++
			}
		}
		if found == 0 {
			t.Fatalf("%s not on the disc", want)
		}
		if pi == len(parts)-1 {
			off, size := be(fst[found*12+4:]), be(fst[found*12+8:])
			return img[off : off+size]
		}
		dir, end = found, be(fst[found*12+8:])
	}
	return nil
}

func TestWriteGCM(t *testing.T) {
	dol := make([]byte, 0x200)
	binary.BigEndian.PutUint32(dol[0xe0:], 0x80003100)
	app := make([]byte, 0x40)
	binary.BigEndian.PutUint32(app[0x10:], 0x81200000)
	binary.BigEndian.PutUint32(app[0x14:], 0x20)
	var b bytes.Buffer
	err := WriteGCM(&b, GCM{GameID: "GBZEBZ", Title: "Test", DOL: dol, Apploader: app, Files: []GCMFile{
		{Path: "opening.bnr", Data: []byte("BNR1")},
		{Path: "DATA/MESH00.BIN", Data: []byte("mesh")},
		{Path: "DATA/SCRP00.BIN", Data: []byte("scrp!")},
		{Path: "DATA/SUB/X.BIN", Data: []byte("deep")},
		{Path: "Z.BIN", Data: []byte("z")},
	}})
	if err != nil {
		t.Fatal(err)
	}
	img := b.Bytes()
	if string(img[:6]) != "GBZEBZ" || binary.BigEndian.Uint32(img[0x1c:]) != gcmMagic || string(img[0x20:0x24]) != "Test" {
		t.Fatal("bad disc header")
	}
	if binary.BigEndian.Uint32(img[0x440+0x18:]) != 1 {
		t.Fatal("an E game ID is an American disc")
	}
	if !bytes.Equal(img[apploaderOff:apploaderOff+len(app)], app) {
		t.Fatal("apploader not at 0x2440")
	}
	dolOff := binary.BigEndian.Uint32(img[0x420:])
	if dolOff%gcmAlign != 0 || !bytes.Equal(img[dolOff:dolOff+uint32(len(dol))], dol) {
		t.Fatal("DOL not where the header says")
	}
	for path, want := range map[string]string{"DATA/MESH00.BIN": "mesh", "data/scrp00.bin": "scrp!", "DATA/SUB/X.BIN": "deep", "Z.BIN": "z", "opening.bnr": "BNR1"} {
		if got := string(readGCM(t, img, path)); got != want {
			t.Errorf("%s: %q, want %q", path, got, want)
		}
	}
	if len(img)%gcmAlign != 0 {
		t.Errorf("image is %d bytes", len(img))
	}
}

func TestWriteGCMErrors(t *testing.T) {
	dol := make([]byte, 0x200)
	app := make([]byte, 0x40)
	for _, g := range []GCM{
		{GameID: "SHORT", DOL: dol, Apploader: app},
		{GameID: "GBZEBZ", DOL: dol},
		{GameID: "GBZEBZ", DOL: dol, Apploader: app, Files: []GCMFile{{Path: "A.BIN"}, {Path: "a.bin"}}},
		{GameID: "GBZEBZ", DOL: dol, Apploader: app, Files: []GCMFile{{Path: "A"}, {Path: "A/B"}}},
	} {
		if err := WriteGCM(&bytes.Buffer{}, g); err == nil {
			t.Errorf("%+v should fail", g.Files)
		}
	}
}

func TestRegionOf(t *testing.T) {
	for id, want := range map[string]uint32{"GBZEBZ": 1, "GBZJBZ": 0, "GBZPBZ": 2, "GBZDBZ": 2} {
		if got := RegionOf(id); got != want {
			t.Errorf("%s: %d, want %d", id, got, want)
		}
	}
}

func TestBanner(t *testing.T) {
	bnr, err := Banner("Penguin Arcade Kart", "Blazium", "a kart game", "")
	if err != nil {
		t.Fatal(err)
	}
	if len(bnr) != bannerSize || string(bnr[:4]) != "BNR1" || !strings.HasPrefix(string(bnr[0x1820:]), "Penguin Arcade Kart") {
		t.Fatal("bad banner")
	}
	if binary.BigEndian.Uint16(bnr[0x20:])&0x8000 == 0 {
		t.Fatal("the plain banner is opaque")
	}
}

// A build needs no SDK: the bundled guest and apploader, the disc image
// written here.
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
		Out: out, Sample: "gdir", PackDir: export, Title: "Game", TitleID: "GTSTBZ"})
	if err != nil {
		t.Fatal(err)
	}
	img, err := os.ReadFile(filepath.Join(out, "GTSTBZ.iso"))
	if err != nil {
		t.Fatal(err)
	}
	if string(readGCM(t, img, "DATA/MESH00.BIN")) != "MESH00.bin" || string(readGCM(t, img, "opening.bnr")[:4]) != "BNR1" {
		t.Fatal("disc files")
	}
	if _, err := os.Stat(filepath.Join(out, "GTSTBZ.THIRD-PARTY-NOTICES.txt")); err != nil {
		t.Fatal(err)
	}
}

func TestBuildNeedsMesh(t *testing.T) {
	err := New().Build(context.Background(), platforms.BuildOptions{CommonOptions: platforms.CommonOptions{Prefix: t.TempDir()},
		Out: filepath.Join(t.TempDir(), "G.iso"), Sample: "gdir", PackDir: t.TempDir()})
	if err == nil {
		t.Fatal("an export without MESH00.bin should fail")
	}
}

func TestBadGameID(t *testing.T) {
	err := New().Build(context.Background(), platforms.BuildOptions{CommonOptions: platforms.CommonOptions{Prefix: t.TempDir()},
		Out: filepath.Join(t.TempDir(), "G.iso"), Sample: "gdir", PackDir: t.TempDir(), TitleID: "BLAZ00001"})
	if err == nil {
		t.Fatal("a nine-character game ID should fail")
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
		Out: filepath.Join(t.TempDir(), "G.iso"), Sample: "gdir", PackDir: export, Overlay: t.TempDir()})
	if err == nil {
		t.Fatal("an overlay without devkitPPC should fail")
	}
}

func TestIsoOut(t *testing.T) {
	if got := isoOut("x/GAME.iso", "GBZEBZ"); got != "x/GAME.iso" {
		t.Error(got)
	}
	if got := isoOut("x", "GBZEBZ"); got != filepath.Join("x", "GBZEBZ.iso") {
		t.Error(got)
	}
}
