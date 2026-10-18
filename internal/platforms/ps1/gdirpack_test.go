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
	// two triangles of different nodes: nothing to join
	if string(out[:4]) != "P1MS" || le.Uint16(out[4:]) != 3 || le.Uint32(out[8:]) != 6 || le.Uint32(out[12:]) != 2 {
		t.Fatalf("header %q v%d verts %d prims %d", out[:4], le.Uint16(out[4:]), le.Uint32(out[8:]), le.Uint32(out[12:]))
	}
	scale := int(le.Uint16(out[6:]))
	if scale != 1024 {
		t.Fatalf("scale %d", scale)
	}
	if x := int16(le.Uint16(out[32+1*12:])); int(x) != scale {
		t.Fatalf("vert 1 x = %d, want %d", x, scale)
	}
	pr := out[32+6*12:]
	if le.Uint16(pr[6:]) != 0xffff {
		t.Fatal("prim 0 should be a triangle")
	}
	// first triangle: 0..1 in each axis, kept to the last texel
	if pr[8] != 0 || pr[10] != 63 || pr[13] != 31 {
		t.Fatalf("prim 0 uv %v", pr[8:16])
	}
	// second: u 3.25..4 -> 208..256 less one tile of 64 per repeat
	p2 := pr[20:]
	if p2[8] != 16 || p2[10] != 63 {
		t.Fatalf("prim 1 uv %v", p2[8:16])
	}
	if texs[0].tiled {
		t.Fatal("no triangle runs past one tile")
	}
	if n := le.Uint32(out[16:]); n != 2 {
		t.Fatalf("chunks %d, want 2 (one per node)", n)
	}
	ch := pr[2*20:]
	// chunk 0: prim 0, no quads, one triangle; chunk 1: prim 1
	if le.Uint32(ch[0:]) != 0 || le.Uint16(ch[4:]) != 0 || le.Uint16(ch[24:]) != 1 || le.Uint32(ch[28:]) != 1 || le.Uint16(ch[28+24:]) != 1 {
		t.Fatalf("chunks %d+%dq+%dt, %d+%dt", le.Uint32(ch[0:]), le.Uint16(ch[4:]), le.Uint16(ch[24:]), le.Uint32(ch[28:]), le.Uint16(ch[28+24:]))
	}
}

// A quad written as two triangles comes back as one quad primitive; two
// triangles at a fold (or not sharing an edge) stay triangles.
func TestPairQuads(t *testing.T) {
	a := ps2Vert{x: 0, z: 0, r: 200}
	b := ps2Vert{x: 12, z: 0, r: 200}
	c := ps2Vert{x: 12, z: -30, r: 200}
	d := ps2Vert{x: 0, z: -30, r: 200}
	ps := pairQuads([]tri{{v: [3]ps2Vert{a, b, c}}, {v: [3]ps2Vert{a, c, d}}})
	if len(ps) != 1 || !ps[0].quad {
		t.Fatalf("%d prims, quad %v", len(ps), len(ps) > 0 && ps[0].quad)
	}
	// the GPU draws v0,v1,v2 and v1,v2,v3: together the two triangles
	q := ps[0].v
	if posKey(q[0]) == posKey(q[3]) || posKey(q[1]) == posKey(q[2]) {
		t.Fatalf("quad corners %v", q)
	}
	// a fold: the second triangle stood up at 90 degrees
	e := ps2Vert{x: 0, y: 30, z: 0, r: 200}
	ps = pairQuads([]tri{{v: [3]ps2Vert{a, b, c}}, {v: [3]ps2Vert{a, c, e}}})
	if len(ps) != 2 || ps[0].quad || ps[1].quad {
		t.Fatalf("fold joined: %d prims", len(ps))
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
