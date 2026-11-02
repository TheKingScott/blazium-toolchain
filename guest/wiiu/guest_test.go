package guest

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"
)

// The shared runtime is the PS2 guest's, byte for byte.
func TestGDIRSharedMatchesPS2(t *testing.T) {
	for _, name := range GDIRShared {
		wiiu, err := gdirFiles.ReadFile("gdir/runtime/" + name)
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		ps2, err := os.ReadFile(filepath.Join("..", "ps2", "runtime", name))
		if err != nil {
			t.Fatalf("ps2 %s: %v", name, err)
		}
		if !bytes.Equal(wiiu, ps2) {
			t.Errorf("%s differs from guest/ps2/runtime/%s", name, name)
		}
	}
}

func TestInstallGDIR(t *testing.T) {
	dir := t.TempDir()
	if err := InstallGDIR(dir); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"Makefile", "main.cpp", "gx2_draw.cpp", "gx2_shaders.h", "gx2_shaders.vsh", "gx2_shaders.psh", "gs_draw.h", "iso_path.h", "pad_io.cpp", "sfx_io.cpp", "mc_io.cpp", "vu1_draw.h"} {
		if _, err := os.Stat(filepath.Join(dir, name)); err != nil {
			t.Errorf("%s not installed: %v", name, err)
		}
	}
}

func TestNotices(t *testing.T) {
	for _, name := range []string{"wut.txt", "newlib.txt"} {
		b, err := Notice(name)
		if err != nil || len(b) < 100 {
			t.Errorf("%s: %v (%d bytes)", name, err, len(b))
		}
	}
}

// The bundled guest is built from these sources: after changing them, run
// scripts/wiiu-guest-prebuilt.sh.
func TestPrebuiltCurrent(t *testing.T) {
	want, err := SourcesHash()
	if err != nil {
		t.Fatal(err)
	}
	if got := PrebuiltVersion(); got != want {
		t.Fatalf("prebuilt/gdir.rpx is from sources %q, these are %q: run scripts/wiiu-guest-prebuilt.sh", got, want)
	}
	rpx, err := PrebuiltRPX()
	// an RPX: a 32-bit big-endian PowerPC ELF of the Cafe OS ABI (0xCA 0xFE),
	// type 0xFE01
	if err != nil || len(rpx) < 0x40 || string(rpx[1:4]) != "ELF" || rpx[4] != 1 || rpx[5] != 2 {
		t.Fatalf("prebuilt/gdir.rpx is not a 32-bit big-endian ELF (%v)", err)
	}
	if rpx[7] != 0xca || rpx[8] != 0xfe || rpx[16] != 0xfe || rpx[17] != 0x01 {
		t.Fatalf("prebuilt/gdir.rpx is not an RPX (ABI %#x %#x, type %#x%02x)", rpx[7], rpx[8], rpx[16], rpx[17])
	}
}
