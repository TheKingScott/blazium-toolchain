package guest

import (
	"bytes"
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"
)

// The shared runtime is the PS2 guest's, byte for byte.
func TestGDIRSharedMatchesPS2(t *testing.T) {
	for _, name := range GDIRShared {
		wii, err := gdirFiles.ReadFile("gdir/runtime/" + name)
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		ps2, err := os.ReadFile(filepath.Join("..", "ps2", "runtime", name))
		if err != nil {
			t.Fatalf("ps2 %s: %v", name, err)
		}
		if !bytes.Equal(wii, ps2) {
			t.Errorf("%s differs from guest/ps2/runtime/%s", name, name)
		}
	}
}

func TestInstallGDIR(t *testing.T) {
	dir := t.TempDir()
	if err := InstallGDIR(dir); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"Makefile", "main.cpp", "gx_draw.cpp", "gs_draw.h", "iso_path.h", "data_io.cpp", "pad_io.cpp", "wii_pad.h", "sfx_io.cpp", "mc_io.cpp", "vu1_draw.h"} {
		if _, err := os.Stat(filepath.Join(dir, name)); err != nil {
			t.Errorf("%s not installed: %v", name, err)
		}
	}
}

func TestNotices(t *testing.T) {
	for _, name := range []string{"libogc.txt", "wiiuse.txt", "libfat.txt", "newlib.txt"} {
		b, err := Notice(name)
		if err != nil || len(b) < 100 {
			t.Errorf("%s: %v (%d bytes)", name, err, len(b))
		}
	}
}

// The bundled guest is built from these sources: after changing them, run
// scripts/wii-guest-prebuilt.sh.
func TestPrebuiltCurrent(t *testing.T) {
	want, err := SourcesHash()
	if err != nil {
		t.Fatal(err)
	}
	if got := PrebuiltVersion(); got != want {
		t.Fatalf("prebuilt/boot.dol is from sources %q, these are %q: run scripts/wii-guest-prebuilt.sh", got, want)
	}
	dol, err := PrebuiltDOL()
	if err != nil || len(dol) < 0x100 {
		t.Fatalf("prebuilt/boot.dol: %v", err)
	}
	if e := binary.BigEndian.Uint32(dol[0xe0:]); e < 0x80003100 || e >= 0x81800000 {
		t.Fatalf("prebuilt/boot.dol entry %#x", e)
	}
	// the app folder's field, once and empty
	i := bytes.Index(dol, []byte(AppDirMarker))
	if i < 0 || bytes.Count(dol, []byte(AppDirMarker)) != 1 {
		t.Fatal("boot.dol has no single app folder field")
	}
	if !bytes.Equal(dol[i+len(AppDirMarker):i+AppDirBytes], make([]byte, AppDirBytes-len(AppDirMarker))) {
		t.Fatal("the app folder field is not empty")
	}
}
