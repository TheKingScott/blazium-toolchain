package nswitch

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"
)

// NRO_REF is a folder with switch-tools' output for the same inputs:
// rom/ (a tree), ref.romfs (build_romfs rom), ref.nacp (nacptool --create
// "Penguin Arcade Kart" "Built with Blazium" 1.0.0), and, if there,
// prog.nro (elf2nro, no assets), icon.jpg and ref.nro (elf2nro --icon
// --nacp --romfs). RomFS, NACP and WithAssets give the same bytes.
func TestMatchesSwitchTools(t *testing.T) {
	ref := os.Getenv("NRO_REF")
	if ref == "" {
		t.Skip("NRO_REF not set")
	}
	var files []romFile
	root := filepath.Join(ref, "rom")
	err := filepath.Walk(root, func(p string, info os.FileInfo, err error) error {
		if err != nil || info.IsDir() {
			return err
		}
		raw, err := os.ReadFile(p)
		rel, _ := filepath.Rel(root, p)
		files = append(files, romFile{filepath.ToSlash(rel), raw})
		return err
	})
	if err != nil {
		t.Fatal(err)
	}
	same := func(what string, got []byte, file string) {
		want, err := os.ReadFile(filepath.Join(ref, file))
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(got, want) {
			i := 0
			for i < len(got) && i < len(want) && got[i] == want[i] {
				i++
			}
			t.Fatalf("%s differs at %#x (len %d, want %d)", what, i, len(got), len(want))
		}
		t.Logf("%s: %d bytes, identical", what, len(got))
	}
	romfs, err := RomFS(files)
	if err != nil {
		t.Fatal(err)
	}
	same("RomFS of rom/", romfs, "ref.romfs")
	nacp := NACP("Penguin Arcade Kart", "Built with Blazium", "1.0.0")
	same("NACP", nacp, "ref.nacp")
	prog, err := os.ReadFile(filepath.Join(ref, "prog.nro"))
	if err != nil {
		return
	}
	icon, err := os.ReadFile(filepath.Join(ref, "icon.jpg"))
	if err != nil {
		t.Fatal(err)
	}
	nro, err := WithAssets(prog, icon, nacp, romfs)
	if err != nil {
		t.Fatal(err)
	}
	same("NRO with assets", nro, "ref.nro")
}
