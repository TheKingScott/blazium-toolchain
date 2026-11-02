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
		sw, err := gdirFiles.ReadFile("gdir/runtime/" + name)
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		ps2, err := os.ReadFile(filepath.Join("..", "ps2", "runtime", name))
		if err != nil {
			t.Fatalf("ps2 %s: %v", name, err)
		}
		if !bytes.Equal(sw, ps2) {
			t.Errorf("%s differs from guest/ps2/runtime/%s", name, name)
		}
	}
}

func TestInstallGDIR(t *testing.T) {
	dir := t.TempDir()
	if err := InstallGDIR(dir); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"Makefile", "main.cpp", "dk_draw.cpp", "dk_shaders.h", "dk_vsh.glsl", "dk_fsh.glsl", "gs_draw.h", "iso_path.h", "pad_io.cpp", "sfx_io.cpp", "mc_io.cpp", "vu1_draw.h"} {
		if _, err := os.Stat(filepath.Join(dir, name)); err != nil {
			t.Errorf("%s not installed: %v", name, err)
		}
	}
}

func TestNotices(t *testing.T) {
	for _, name := range []string{"libnx.txt", "deko3d.txt", "newlib.txt"} {
		b, err := Notice(name)
		if err != nil || len(b) < 100 {
			t.Errorf("%s: %v (%d bytes)", name, err, len(b))
		}
	}
}

// The bundled guest is built from these sources: after changing them, run
// scripts/switch-guest-prebuilt.sh.
func TestPrebuiltCurrent(t *testing.T) {
	want, err := SourcesHash()
	if err != nil {
		t.Fatal(err)
	}
	if got := PrebuiltVersion(); got != want {
		t.Fatalf("prebuilt/gdir.nro is from sources %q, these are %q: run scripts/switch-guest-prebuilt.sh", got, want)
	}
	nro, err := PrebuiltNRO()
	// an .nro: "NRO0" at 0x10, its size at 0x18, and no assets after it
	if err != nil || len(nro) < 0x80 || string(nro[0x10:0x14]) != "NRO0" {
		t.Fatalf("prebuilt/gdir.nro is not an NRO (%v)", err)
	}
	if size := binary.LittleEndian.Uint32(nro[0x18:]); int(size) != len(nro) {
		t.Fatalf("prebuilt/gdir.nro is %d bytes, its header says %d", len(nro), size)
	}
}
