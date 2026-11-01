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
		psp, err := gdirFiles.ReadFile("gdir/runtime/" + name)
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		ps2, err := os.ReadFile(filepath.Join("..", "ps2", "runtime", name))
		if err != nil {
			t.Fatalf("ps2 %s: %v", name, err)
		}
		if !bytes.Equal(psp, ps2) {
			t.Errorf("%s differs from guest/ps2/runtime/%s", name, name)
		}
	}
}

func TestInstallGDIR(t *testing.T) {
	dir := t.TempDir()
	if err := InstallGDIR(dir); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"Makefile", "main.cpp", "gu_draw.cpp", "gs_draw.h", "iso_path.h", "data_io.cpp", "pad_io.cpp", "sfx_io.cpp", "mc_io.cpp", "vu1_draw.h"} {
		if _, err := os.Stat(filepath.Join(dir, name)); err != nil {
			t.Errorf("%s not installed: %v", name, err)
		}
	}
}

func TestNotices(t *testing.T) {
	for _, name := range []string{"pspsdk.txt", "newlib.txt"} {
		b, err := Notice(name)
		if err != nil || len(b) < 100 {
			t.Errorf("%s: %v (%d bytes)", name, err, len(b))
		}
	}
}

// The bundled guest is built from these sources: after changing them, run
// scripts/psp-guest-prebuilt.sh.
func TestPrebuiltCurrent(t *testing.T) {
	want, err := SourcesHash()
	if err != nil {
		t.Fatal(err)
	}
	if got := PrebuiltVersion(); got != want {
		t.Fatalf("prebuilt/gdir.prx is from sources %q, these are %q: run scripts/psp-guest-prebuilt.sh", got, want)
	}
	prx, err := PrebuiltPRX()
	if err != nil || len(prx) < 0x100 || string(prx[1:4]) != "ELF" || prx[4] != 1 || prx[5] != 1 {
		t.Fatalf("prebuilt/gdir.prx is not a 32-bit little-endian ELF (%v)", err)
	}
	// a PRX: ELF type 0xFFA0
	if prx[16] != 0xa0 || prx[17] != 0xff {
		t.Fatalf("prebuilt/gdir.prx is not a PRX (type %#x)", int(prx[16])|int(prx[17])<<8)
	}
	i := bytes.Index(prx, []byte(AppDirMarker))
	if i < 0 || bytes.Count(prx, []byte(AppDirMarker)) != 1 {
		t.Fatal("gdir.prx has no single game folder field")
	}
	if !bytes.Equal(prx[i+len(AppDirMarker):i+AppDirBytes], make([]byte, AppDirBytes-len(AppDirMarker))) {
		t.Fatal("the game folder field is not empty")
	}
}
