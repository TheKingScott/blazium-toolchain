package ps1

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"
)

func TestMCPS1FromMCSave(t *testing.T) {
	raw := make([]byte, 76)
	copy(raw, "MCSV")
	binary.LittleEndian.PutUint16(raw[4:], 1)
	binary.LittleEndian.PutUint16(raw[6:], 76)
	copy(raw[8:], "BAPenguin Kart Racer")
	copy(raw[40:], "Penguin Kart Racer")
	ico := make([]byte, 100+128*128*2)
	tex := ico[100:]
	for i := 0; i < 128*128; i++ {
		c := uint16(0x8000 | 31) // red
		if (i % 128) >= 64 {
			c = 0x8000 | 31<<10 // blue on the right half
		}
		binary.LittleEndian.PutUint16(tex[i*2:], c)
	}
	binary.LittleEndian.PutUint32(raw[72:], uint32(len(ico)))
	raw = append(raw, ico...)

	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "MCSAVE.BIN"), raw, 0o644); err != nil {
		t.Fatal(err)
	}
	made, err := MakeMCPS1(dir, "BLAZ_000.01")
	if err != nil || !made {
		t.Fatalf("MakeMCPS1: %v %v", made, err)
	}
	if _, err := os.Stat(filepath.Join(dir, "MCSAVE.BIN")); !os.IsNotExist(err) {
		t.Fatalf("MCSAVE.BIN should be gone")
	}
	out, err := os.ReadFile(filepath.Join(dir, "MCPS1.BIN"))
	if err != nil {
		t.Fatal(err)
	}
	if len(out) != 8+20+256 || string(out[:4]) != "MCP1" {
		t.Fatalf("bad MCPS1: %d bytes", len(out))
	}
	if name := cString(out[8:28]); name != "BABLAZ-00001PENGUIN" {
		t.Fatalf("name %q", name)
	}
	h := out[28:]
	if h[0] != 'S' || h[1] != 'C' || h[2] != 0x11 || h[3] != 1 {
		t.Fatalf("header % x", h[:4])
	}
	// "P" full width
	if h[4] != 0x82 || h[5] != 0x6f {
		t.Fatalf("title starts % x", h[4:6])
	}
	// the left pixel of the top row red, the right blue: different palette entries
	pal := func(i int) uint16 { return binary.LittleEndian.Uint16(h[0x60+i*2:]) }
	left := int(h[0x80] & 15)
	right := int(h[0x80+7] >> 4)
	if pal(left)&31 < 28 || (pal(right)>>10)&31 < 28 {
		t.Fatalf("icon colours %04x %04x", pal(left), pal(right))
	}
}
