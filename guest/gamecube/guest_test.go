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
		gc, err := gdirFiles.ReadFile("gdir/runtime/" + name)
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		ps2, err := os.ReadFile(filepath.Join("..", "ps2", "runtime", name))
		if err != nil {
			t.Fatalf("ps2 %s: %v", name, err)
		}
		if !bytes.Equal(gc, ps2) {
			t.Errorf("%s differs from guest/ps2/runtime/%s", name, name)
		}
	}
}

func TestInstallGDIR(t *testing.T) {
	dir := t.TempDir()
	if err := InstallGDIR(dir); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"Makefile", "main.cpp", "gx_draw.cpp", "gs_draw.h", "iso_path.h", "disc_io.cpp", "pad_io.cpp", "sfx_io.cpp", "mc_io.cpp", "vu1_draw.h", "apploader/apploader.c"} {
		if _, err := os.Stat(filepath.Join(dir, name)); err != nil {
			t.Errorf("%s not installed: %v", name, err)
		}
	}
}

func TestNotices(t *testing.T) {
	for _, name := range []string{"libogc.txt", "newlib.txt"} {
		b, err := Notice(name)
		if err != nil || len(b) < 100 {
			t.Errorf("%s: %v (%d bytes)", name, err, len(b))
		}
	}
}

// The bundled guest is built from these sources: after changing them, run
// scripts/gamecube-guest-prebuilt.sh.
func TestPrebuiltCurrent(t *testing.T) {
	want, err := SourcesHash()
	if err != nil {
		t.Fatal(err)
	}
	if got := PrebuiltVersion(); got != want {
		t.Fatalf("prebuilt/ is from sources %q, these are %q: run scripts/gamecube-guest-prebuilt.sh", got, want)
	}
	dol, err := PrebuiltDOL()
	if err != nil || len(dol) < 0x100 {
		t.Fatalf("prebuilt/gdir.dol: %v", err)
	}
	// a DOL: the entry point and the first text section in MEM1
	if e := binary.BigEndian.Uint32(dol[0xe0:]); e < 0x80003100 || e >= 0x81800000 {
		t.Fatalf("prebuilt/gdir.dol entry %#x", e)
	}
	app, err := Apploader()
	if err != nil || len(app) < 0x40 {
		t.Fatalf("prebuilt/apploader.img: %v", err)
	}
	if e := binary.BigEndian.Uint32(app[0x10:]); e < 0x81200000 || e >= 0x81300000 {
		t.Fatalf("apploader entry %#x", e)
	}
	if n := binary.BigEndian.Uint32(app[0x14:]); int(n)+0x20 != len(app) {
		t.Fatalf("apploader size %d, image %d", n, len(app))
	}
}

// Under Nintendont (a Wii) the disc is emulated: Nintendont points code that
// reaches the drive as "lis rX,0xCC00" then "addi rY,rX,0x6000" at its own
// disc interface. The prebuilt guest must read the disc that way (libogc's
// DVD driver resets the real drive, and hangs there).
func TestPrebuiltDiscNintendont(t *testing.T) {
	dol, err := PrebuiltDOL()
	if err != nil {
		t.Fatal(err)
	}
	be := binary.BigEndian
	found := false
	for s := 0; s < 7 && !found; s++ { // the text sections
		off, size := be.Uint32(dol[s*4:]), be.Uint32(dol[0x90+s*4:])
		code := dol[off : off+size]
		for i := 0; i+8 <= len(code); i += 4 {
			op := be.Uint32(code[i:])
			if op&0xFC1FFFFF != 0x3C00CC00 {
				continue
			}
			rx := op >> 21 & 31
			next := be.Uint32(code[i+4:])
			if next&0xFC00FFFF == 0x38006000 && next>>16&31 == rx {
				found = true
				break
			}
		}
	}
	if !found {
		t.Error("gdir.dol has no disc read Nintendont can redirect (lis 0xCC00; addi 0x6000)")
	}
}

// Under Nintendont the word at 0x800031a0 is not the game's (IOS keeps
// values in that part of low memory), so the DOL leaves 0x80003100 to
// 0x80003200 empty and starts after it.
func TestPrebuiltLowMemoryGap(t *testing.T) {
	dol, err := PrebuiltDOL()
	if err != nil {
		t.Fatal(err)
	}
	be := binary.BigEndian
	off, addr := be.Uint32(dol[0:]), be.Uint32(dol[0x48:])
	if addr != 0x80003100 || !bytes.Equal(dol[off:off+0x100], make([]byte, 0x100)) {
		t.Errorf("text 0 at %#x does not start with 0x100 empty bytes", addr)
	}
	if entry := be.Uint32(dol[0xe0:]); entry < 0x80003200 {
		t.Errorf("entry point %#x is in the gap", entry)
	}
}
