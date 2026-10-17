package ps1

import (
	"encoding/binary"
	"math"
	"os"
	"path/filepath"
	"testing"
)

func ps2Mesh(verts []ps2Vert, idx []uint32) []byte {
	le := binary.LittleEndian
	b := make([]byte, 16)
	copy(b, "MESH")
	le.PutUint16(b[4:], 1)
	le.PutUint32(b[8:], uint32(len(verts)))
	le.PutUint32(b[12:], uint32(len(idx)))
	for _, v := range verts {
		var p [28]byte
		le.PutUint32(p[0:], math.Float32bits(v.x))
		le.PutUint32(p[4:], math.Float32bits(v.y))
		le.PutUint32(p[8:], math.Float32bits(v.z))
		p[12], p[13], p[14], p[15] = v.r, v.g, v.b, v.a
		le.PutUint32(p[16:], math.Float32bits(v.u))
		le.PutUint32(p[20:], math.Float32bits(v.v))
		le.PutUint16(p[24:], v.node)
		le.PutUint16(p[26:], v.tex)
		b = append(b, p[:]...)
	}
	for _, i := range idx {
		b = binary.LittleEndian.AppendUint32(b, i)
	}
	return b
}

func TestConvertMesh(t *testing.T) {
	verts := []ps2Vert{
		{x: 0, y: 0, z: 0, r: 128, u: 0, v: 0},
		{x: 1, y: 0, z: 0, u: 1, v: 0},
		{x: 0, y: 1, z: -2, u: 0, v: 1},
		{x: 5, y: 0, z: 0, u: 3.25, v: 2, node: 1},
		{x: 6, y: 0, z: 0, u: 4, v: 2, node: 1},
		{x: 5, y: 1, z: 0, u: 3.25, v: 3, node: 1},
	}
	texs := []ps2Tex{{w: 64, h: 32}}
	out, err := ConvertMesh(ps2Mesh(verts, []uint32{0, 1, 2, 3, 4, 5}), texs)
	if err != nil {
		t.Fatal(err)
	}
	le := binary.LittleEndian
	if string(out[:4]) != "P1MS" || le.Uint32(out[8:]) != 6 || le.Uint32(out[12:]) != 2 {
		t.Fatalf("header %q verts %d tris %d", out[:4], le.Uint32(out[8:]), le.Uint32(out[12:]))
	}
	scale := int(le.Uint16(out[6:]))
	if scale != 1024 {
		t.Fatalf("scale %d", scale)
	}
	if x := int16(le.Uint16(out[32+1*12:])); int(x) != scale {
		t.Fatalf("vert 1 x = %d, want %d", x, scale)
	}
	tri := out[32+6*12:]
	// first triangle: 0..1 in each axis, kept to the last texel
	if tri[6] != 0 || tri[8] != 63 || tri[11] != 31 {
		t.Fatalf("tri 0 uv %v", tri[6:12])
	}
	// second: u 3.25..4 tiles -> 16..64 after the shift (beyond one tile? no: 208..256 - 192)
	t2 := tri[16:]
	if t2[6] != 16 || t2[8] != 63 {
		t.Fatalf("tri 1 uv %v", t2[6:12])
	}
	if texs[0].tiled {
		t.Fatal("no triangle runs past one tile")
	}
	if n := le.Uint32(out[16:]); n != 2 {
		t.Fatalf("chunks %d, want 2 (one per node)", n)
	}
}

func TestPS1Color(t *testing.T) {
	for in, want := range map[uint16]uint16{0x0000: 0, 0x1234: 0, 0x8000: 0x8000, 0x801f: 0x001f, 0xffff: 0x7fff} {
		if got := ps1Color(in); got != want {
			t.Errorf("ps1Color(%#x) = %#x, want %#x", in, got, want)
		}
	}
}

func TestConvertPacksDev(t *testing.T) {
	dir := os.Getenv("P1_CONVERT_DIR")
	if dir == "" {
		t.Skip("P1_CONVERT_DIR not set")
	}
	n, err := ConvertPacks(dir)
	if err != nil {
		t.Fatal(err)
	}
	t.Logf("converted %d meshes", n)
}

func TestPatchStreamCosts(t *testing.T) {
	dir := t.TempDir()
	le := binary.LittleEndian
	b := []byte("STRM")
	b = le.AppendUint16(b, 1)
	b = le.AppendUint16(b, 2)
	b = le.AppendUint16(b, 1)
	b = le.AppendUint16(b, 2)
	for _, p := range []string{"res://a.tscn", "res://b.tscn"} {
		b = append(b, byte(len(p)))
		b = append(b, p...)
		b = le.AppendUint32(b, 1000)
		b = le.AppendUint32(b, 64)
	}
	if err := os.WriteFile(filepath.Join(dir, "STREAM.bin"), b, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := patchStreamCosts(dir, map[int]int{2: 300}); err != nil {
		t.Fatal(err)
	}
	got, _ := os.ReadFile(filepath.Join(dir, "STREAM.bin"))
	first := 8 + 4 + 1 + 12
	second := first + 8 + 1 + 12
	if le.Uint32(got[first:]) != 1000 || le.Uint32(got[second:]) != 700 || le.Uint32(got[second+4:]) != 64 {
		t.Fatalf("costs %d %d", le.Uint32(got[first:]), le.Uint32(got[second:]))
	}
}
