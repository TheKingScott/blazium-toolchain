package iso

import (
	"encoding/binary"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestWriteDataISOHasPVD(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "SYSTEM.CNF"), []byte("BOOT2 = cdrom0:\\GAME.ELF;1\nVER = 1.00\nVMODE = NTSC\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "GAME.ELF"), []byte{0x7f, 'E', 'L', 'F', 0, 1, 2, 3}, 0o644); err != nil {
		t.Fatal(err)
	}
	out := filepath.Join(t.TempDir(), "game.iso")
	if err := WriteDataISO(dir, out, "TESTPS2"); err != nil {
		t.Fatal(err)
	}
	if !HasISO9660PVD(out) {
		t.Fatal("missing CD001 PVD")
	}
}

// A PS2 export puts the resolved guest tree (~35 files) under guest/. Its
// Joliet directory needs more than one sector; records must not straddle the
// sector boundary and the declared extent size must cover every record.
func TestWriteDataISOMultiSectorDirectory(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "SYSTEM.CNF"), []byte("BOOT2 = cdrom0:\\GAME.ELF;1\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	sub := filepath.Join(dir, "guest")
	if err := os.MkdirAll(sub, 0o755); err != nil {
		t.Fatal(err)
	}
	const files = 40
	for i := 0; i < files; i++ {
		name := filepath.Join(sub, fmt.Sprintf("src_file_%02d.cpp", i))
		if err := os.WriteFile(name, []byte{byte(i)}, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	out := filepath.Join(t.TempDir(), "game.iso")
	if err := WriteDataISO(dir, out, "TESTPS2"); err != nil {
		t.Fatal(err)
	}
	img, err := os.ReadFile(out)
	if err != nil {
		t.Fatal(err)
	}
	for _, vd := range []int{isoPVDSector, isoSVDSector} {
		root := img[vd*SectorSize+156:]
		seen := walkDirForTest(t, img, binary.LittleEndian.Uint32(root[2:]), binary.LittleEndian.Uint32(root[10:]))
		if seen != files+2 { // SYSTEM.CNF, guest/, and the files under guest/
			t.Fatalf("descriptor %d: walked %d entries, want %d", vd, seen, files+2)
		}
	}
}

func walkDirForTest(t *testing.T, img []byte, extent, size uint32) int {
	t.Helper()
	data := img[int(extent)*SectorSize : int(extent)*SectorSize+int(size)]
	seen := 0
	for off := 0; off < len(data); {
		l := int(data[off])
		if l == 0 {
			off = (off/SectorSize + 1) * SectorSize
			continue
		}
		if off/SectorSize != (off+l-1)/SectorSize {
			t.Fatalf("record at %d (len %d) crosses a sector boundary", off, l)
		}
		ident := data[off+33 : off+33+int(data[off+32])]
		if !(len(ident) == 1 && ident[0] <= 1) {
			seen++
			if data[off+25]&2 != 0 {
				seen += walkDirForTest(t, img, binary.LittleEndian.Uint32(data[off+2:]), binary.LittleEndian.Uint32(data[off+10:]))
			}
		}
		off += l
	}
	return seen
}

func TestWriteDataISOSubdirKeepsRootShort(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "SYSTEM.CNF"), []byte("BOOT2 = cdrom0:\\GAME.ELF;1\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "GAME.ELF"), []byte("elf"), 0o644); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 80; i++ {
		if err := os.WriteFile(filepath.Join(dir, fmt.Sprintf("MESH%02d.BIN", i)), []byte("data"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	out := filepath.Join(t.TempDir(), "boot.iso")
	keep := func(n string) bool { return n == "SYSTEM.CNF" || n == "GAME.ELF" }
	if err := WriteDataISOSubdir(dir, out, "TEST", keep, "DATA", nil); err != nil {
		t.Fatal(err)
	}
	b, err := os.ReadFile(out)
	if err != nil {
		t.Fatal(err)
	}
	pvd := b[16*SectorSize:]
	ext := binary.LittleEndian.Uint32(pvd[156+2:])
	size := binary.LittleEndian.Uint32(pvd[156+10:])
	if size != SectorSize {
		t.Fatalf("root directory is %d bytes, want one sector", size)
	}
	root := string(b[int(ext)*SectorSize : int(ext)*SectorSize+int(size)])
	for _, want := range []string{"SYSTEM.CNF;1", "GAME.ELF;1", "DATA"} {
		if !strings.Contains(root, want) {
			t.Fatalf("root has no %s", want)
		}
	}
	if strings.Contains(root, "MESH00") {
		t.Fatal("MESH00 should be under DATA, not the root")
	}
}

func TestWriteDataISOGroupedKeepsEveryDirOneSector(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "SYSTEM.CNF"), []byte("BOOT2 = cdrom0:\\GAME.ELF;1\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	// 11 packs of 7 files: 77 records in one DATA folder would take two sectors
	for i := 0; i < 11; i++ {
		for _, stem := range []string{"CAM", "GTEX", "HIT", "HUD", "MESH", "NODE", "SCRP"} {
			if err := os.WriteFile(filepath.Join(dir, fmt.Sprintf("%s%02d.BIN", stem, i)), []byte("data"), 0o644); err != nil {
				t.Fatal(err)
			}
		}
	}
	if err := os.WriteFile(filepath.Join(dir, "STREAM.BIN"), []byte("data"), 0o644); err != nil {
		t.Fatal(err)
	}
	out := filepath.Join(t.TempDir(), "boot.iso")
	keep := func(n string) bool { return n == "SYSTEM.CNF" }
	group := func(n string) string {
		if strings.HasPrefix(n, "STREAM") {
			return ""
		}
		return "P" + n[len(n)-6:len(n)-4]
	}
	if err := WriteDataISOGrouped(dir, out, "TEST", keep, "DATA", nil, group); err != nil {
		t.Fatal(err)
	}
	if n := LargestDirRecords(out); n != 1 {
		t.Fatalf("largest directory spans %d sectors, want 1", n)
	}
	b, err := os.ReadFile(out)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(b), "P10") || !strings.Contains(string(b), "MESH10.BIN;1") {
		t.Fatal("no P10 folder with MESH10.BIN")
	}
}
