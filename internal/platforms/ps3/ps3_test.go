package ps3

import (
	"bytes"
	"context"
	"crypto/sha1"
	"encoding/hex"
	"os"
	"path/filepath"
	"strings"
	"testing"

	guestps3 "github.com/blazium-games/blazium-toolchain/guest/ps3"
	"github.com/blazium-games/blazium-toolchain/internal/platforms"
)

func TestInfo(t *testing.T) {
	info := New().Info()
	if info.ID != "ps3" || info.Status != platforms.StatusSupported {
		t.Fatalf("info: %+v", info)
	}
}

// A build needs no SDK: the bundled guest, signed and packed here.
func TestBuildWithoutSDK(t *testing.T) {
	t.Setenv("PS3DEV", filepath.Join(t.TempDir(), "none"))
	t.Setenv("PSL1GHT", "")
	t.Setenv("GDIR_EXTRA_FLAGS", "")
	export := t.TempDir()
	for _, f := range []string{"MESH00.bin", "SCRP00.bin", "DISP.bin"} {
		if err := os.WriteFile(filepath.Join(export, f), []byte(f), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	out := filepath.Join(t.TempDir(), "GAME.pkg")
	err := New().Build(context.Background(), platforms.BuildOptions{CommonOptions: platforms.CommonOptions{Prefix: t.TempDir()},
		Out: out, Sample: "gdir", PackDir: export, Title: "Game", TitleID: "TEST00001"})
	if err != nil {
		t.Fatal(err)
	}
	dir := filepath.Join(filepath.Dir(out), "TEST00001")
	for _, f := range []string{"PARAM.SFO", "ICON0.PNG", "USRDIR/EBOOT.BIN", "USRDIR/DATA/MESH00.BIN"} {
		if _, err := os.Stat(filepath.Join(dir, filepath.FromSlash(f))); err != nil {
			t.Errorf("%s: %v", f, err)
		}
	}
	pkg, err := os.ReadFile(out)
	if err != nil || len(pkg) < 0x140 || string(pkg[1:4]) != "PKG" || !strings.Contains(string(pkg[0x30:0x60]), "UP0001-TEST00001_00-GAME") {
		t.Fatalf("pkg: %v", err)
	}
	eboot, _ := os.ReadFile(filepath.Join(dir, "USRDIR", "EBOOT.BIN"))
	if len(eboot) < 0x100 || string(eboot[:3]) != "SCE" {
		t.Fatal("EBOOT.BIN is not a SELF")
	}
}

func TestOverlayNeedsSDK(t *testing.T) {
	t.Setenv("PS3DEV", filepath.Join(t.TempDir(), "none"))
	t.Setenv("PSL1GHT", "")
	export := t.TempDir()
	if err := os.WriteFile(filepath.Join(export, "MESH00.bin"), []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	err := New().Build(context.Background(), platforms.BuildOptions{CommonOptions: platforms.CommonOptions{Prefix: t.TempDir()},
		Out: filepath.Join(t.TempDir(), "G.pkg"), Sample: "gdir", PackDir: export, Overlay: t.TempDir()})
	if err == nil {
		t.Fatal("an overlay without ps3dev should fail")
	}
}

func TestGDIRDataName(t *testing.T) {
	for name, want := range map[string]bool{"MESH00.bin": true, "DISP.BIN": true, "BOOTLOGO.RAW": false, "PENGUIN.ELF": false, "BUDGET.txt": false} {
		if got := gdirDataName(name); got != want {
			t.Errorf("%s: %v, want %v", name, got, want)
		}
	}
}

func TestStageGDIR(t *testing.T) {
	export := t.TempDir()
	if err := os.WriteFile(filepath.Join(export, "SCRP00.bin"), []byte("SCRP"), 0o644); err != nil {
		t.Fatal(err)
	}
	src := filepath.Join(t.TempDir(), "src")
	if _, err := stageGDIR(src, export, "", nil); err == nil {
		t.Fatal("an export without MESH00 should fail")
	}
	for _, f := range []string{"MESH00.bin", "ICON0.PNG", "PENGUIN.ELF"} {
		if err := os.WriteFile(filepath.Join(export, f), []byte(f), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	n, err := stageGDIR(src, export, "", nil)
	if err != nil || n != 2 {
		t.Fatalf("staged %d, %v", n, err)
	}
	for _, f := range []string{"pkg/USRDIR/DATA/MESH00.BIN", "pkg/USRDIR/DATA/SCRP00.BIN", "pkg/ICON0.PNG", "Makefile", "rsx_draw.cpp"} {
		if _, err := os.Stat(filepath.Join(src, filepath.FromSlash(f))); err != nil {
			t.Errorf("%s: %v", f, err)
		}
	}
	if _, err := os.Stat(filepath.Join(src, "pkg/USRDIR/DATA/PENGUIN.ELF")); err == nil {
		t.Error("the ELF should not be staged")
	}
}

func TestOutputs(t *testing.T) {
	pkg, dir := outputs(filepath.Join("a", "PENGUIN.pkg"), "BLAZ00001")
	if pkg != filepath.Join("a", "PENGUIN.pkg") || dir != filepath.Join("a", "BLAZ00001") {
		t.Fatalf("got %q %q", pkg, dir)
	}
	pkg, dir = outputs("out", "PENG00001")
	if pkg != filepath.Join("out", "PENG00001.pkg") || dir != filepath.Join("out", "PENG00001") {
		t.Fatalf("got %q %q", pkg, dir)
	}
}

func TestContentID(t *testing.T) {
	got := contentID("PENG00001", "Penguin Arcade Kart")
	if got != "UP0001-PENG00001_00-PENGUINARCADEKAR" || len(got) != 36 {
		t.Fatalf("got %q (%d)", got, len(got))
	}
	if got := contentID("BLAZ00001", "Go!"); got != "UP0001-BLAZ00001_00-GO00000000000000" {
		t.Fatalf("got %q", got)
	}
}

func TestTitleID(t *testing.T) {
	for id, ok := range map[string]bool{"BLAZ00001": true, "PENG12345": true, "BLA00001": false, "blaz00001": false, "BLAZ0001A": false} {
		if titleIDRe.MatchString(id) != ok {
			t.Errorf("%s: want %v", id, ok)
		}
	}
}

func TestNotices(t *testing.T) {
	dir := t.TempDir()
	out := filepath.Join(dir, "GAME.pkg")
	path, err := writeNotices(out)
	if err != nil {
		t.Fatal(err)
	}
	if path != filepath.Join(dir, "GAME.THIRD-PARTY-NOTICES.txt") {
		t.Fatalf("path %s", path)
	}
	raw, _ := os.ReadFile(path)
	for _, want := range []string{"PSL1GHT", "newlib", "Permission is hereby granted"} {
		if !strings.Contains(string(raw), want) {
			t.Errorf("notices lack %q", want)
		}
	}
}

// PARAM.SFO and the .pkg as PSL1GHT's sfo and pkg tools write them: these
// sizes and hashes are theirs for the same input.
func TestSFOLayout(t *testing.T) {
	b := paramSFO("Penguin Arcade Kart", "PENG00001", "01.00")
	if len(b) != 1040 || string(b[1:4]) != "PSF" {
		t.Fatalf("PARAM.SFO is %d bytes", len(b))
	}
	if sum := sha1.Sum(b); hex.EncodeToString(sum[:]) != sfoSHA1 {
		t.Fatalf("PARAM.SFO differs from PSL1GHT's sfo: %x", sum)
	}
}

func TestSelfSigns(t *testing.T) {
	elf, err := guestps3.PrebuiltELF()
	if err != nil {
		t.Fatal(err)
	}
	a, err := MakeSelfNPDRM(elf, "UP0001-TEST00001_00-GAME000000000000", "EBOOT.BIN")
	if err != nil {
		t.Fatal(err)
	}
	b, _ := MakeSelfNPDRM(elf, "UP0001-TEST00001_00-GAME000000000000", "EBOOT.BIN")
	if !bytes.Equal(a, b) {
		t.Fatal("a SELF build is not repeatable")
	}
	if be.Uint32(a[0:]) != 0x53434500 || be.Uint64(a[0x18:]) != uint64(len(elf)) {
		t.Fatal("SELF header")
	}
	if _, err := MakeSelfNPDRM([]byte("not an elf"), "x", "EBOOT.BIN"); err == nil {
		t.Fatal("not an ELF should fail")
	}
	// AES-CMAC (RFC 4493 example 2)
	key, _ := hex.DecodeString("2b7e151628aed2a6abf7158809cf4f3c")
	msg, _ := hex.DecodeString("6bc1bee22e409f96e93d7e117393172a")
	if got := hex.EncodeToString(aesOmac1(key, msg)); got != "070a16b46b4d4144f79bdd9dd04a287c" {
		t.Fatalf("cmac %s", got)
	}
}

const (
	sfoSHA1 = "df2016d4a5296d86326055ba1264ad06651a886a" // sfo --title "Penguin Arcade Kart" --appid PENG00001 -f sfo.xml
	pkgSHA1 = "f43f0280a2fb70b29a167b2eccc9d82bfd2a3e49" // pkg --contentid UP0001-PENG00001_00-PENGUINARCADEKAR p/
)

func TestPkgMatchesPSL1GHT(t *testing.T) {
	dir := t.TempDir()
	files := map[string][]byte{
		"PARAM.SFO":         paramSFO("Penguin Arcade Kart", "PENG00001", "01.00"),
		"ICON0.PNG":         []byte("icon"),
		"USRDIR/DATA/A.BIN": []byte("hello"),
	}
	eboot := make([]byte, 0x100)
	for i := range eboot {
		eboot[i] = byte(i)
	}
	files["USRDIR/EBOOT.BIN"] = eboot
	for name, raw := range files {
		p := filepath.Join(dir, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, raw, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	pkg, err := makePkg(dir, "UP0001-PENG00001_00-PENGUINARCADEKAR")
	if err != nil {
		t.Fatal(err)
	}
	if sum := sha1.Sum(pkg); hex.EncodeToString(sum[:]) != pkgSHA1 {
		t.Fatalf("the pkg differs from PSL1GHT's pkg: %x", sum)
	}
}
