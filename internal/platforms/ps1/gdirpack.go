package ps1

// The PS1 GDIR runtime (guest/ps1/gdir) draws the same cooked scenes as the
// PS2 guest, but in 2 MiB of RAM and with the GTE's fixed point. The PS2
// cook's MESH (float verts, u32 indices) and GTEX (T4/T8 + RGB555 CLUT, or
// CT16) are rewritten here into the PS1 forms the runtime reads:
//
//	P1MS: int16 world positions at `scale` units a metre, vertex colours,
//	      triangles and quads (meshquad.go) with per-corner texel UVs
//	      (wrapped for the GPU's texture window), and the culling chunks the
//	      PS2 guest builds at load time.
//	P1TX: the same textures with PS1 colour semantics (0x0000 is the only
//	      transparent texel; opaque black becomes 0x8000).
//
// Everything else the PS2 cook writes (NODE, HUD, CAM, SCRP/GDIR, SFX...)
// is used as it is.

import (
	"encoding/binary"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

const (
	p1msVersion  = 3
	p1msHeader   = 32
	p1msVertSize = 12 // int16 x,y,z,pad (a GTE SVECTOR), u8 r,g,b,0
	p1msPrimSize = 20 // u16 a,b,c,d (0xffff: a triangle); u8 uv[8], tex, flags; pad
	p1msChunk    = 28

	chunkTris = 48
	chunkSpan = 40.0 // metres, any axis (as the PS2 guest)

	noTex = 0xff
)

type ps2Vert struct {
	x, y, z    float32
	r, g, b, a uint8
	u, v       float32
	node, tex  uint16
}

type ps2Tex struct {
	w, h  int
	psm   int // 0 T4, 1 T8, 2 CT16
	tiled bool
}

// ConvertMesh rewrites a PS2 MESH blob as P1MS (v3). texs gives each
// texture slot's size (from the pack's GTEX), for the texel UVs; a texture
// whose UVs run past one tile is marked tiled in texs (the runtime then
// draws it through the GPU's texture window).
//
// P1MS v3: header (32), vertices (12 each), primitives (20 each: u16
// a,b,c,d with d 0xffff for a triangle, u8 uv[8], tex, flags, pad),
// chunks (28 each: first primitive u32, quads u16, node u16, sphere centre
// and radius f32 x4, triangles u16 (after the quads), pad), then the
// cook's extras (instances, NFOG...).
func ConvertMesh(mesh []byte, texs []ps2Tex) ([]byte, error) {
	if len(mesh) < 16 || string(mesh[:4]) != "MESH" {
		return nil, fmt.Errorf("not a MESH blob")
	}
	le := binary.LittleEndian
	vn := int(le.Uint32(mesh[8:]))
	in := int(le.Uint32(mesh[12:]))
	vend := 16 + vn*28
	iend := vend + in*4
	if iend > len(mesh) {
		return nil, fmt.Errorf("MESH: %d verts / %d indices overrun %d bytes", vn, in, len(mesh))
	}
	verts := make([]ps2Vert, vn)
	maxAbs := 0.0
	for i := range verts {
		p := mesh[16+i*28:]
		v := ps2Vert{
			x: math.Float32frombits(le.Uint32(p[0:])),
			y: math.Float32frombits(le.Uint32(p[4:])),
			z: math.Float32frombits(le.Uint32(p[8:])),
			r: p[12], g: p[13], b: p[14], a: p[15],
			u:    math.Float32frombits(le.Uint32(p[16:])),
			v:    math.Float32frombits(le.Uint32(p[20:])),
			node: le.Uint16(p[24:]),
			tex:  le.Uint16(p[26:]),
		}
		verts[i] = v
		for _, c := range []float32{v.x, v.y, v.z} {
			if a := math.Abs(float64(c)); a > maxAbs && !math.IsInf(a, 0) && !math.IsNaN(a) {
				maxAbs = a
			}
		}
	}
	scale := meshScale(maxAbs)
	// the source triangles (one with an index past the vertices is dropped)
	var src []tri
	for t := 0; t < in/3; t++ {
		a, b, c := int(le.Uint32(mesh[vend+t*12:])), int(le.Uint32(mesh[vend+t*12+4:])), int(le.Uint32(mesh[vend+t*12+8:]))
		if a >= vn || b >= vn || c >= vn {
			continue
		}
		src = append(src, tri{v: [3]ps2Vert{verts[a], verts[b], verts[c]}, orig: len(src)})
	}
	chunks := buildChunks(src)

	q := func(f float32) uint16 {
		v := math.Round(float64(f) * float64(scale))
		v = math.Max(-32767, math.Min(32767, v))
		return uint16(int16(v))
	}
	// one vertex per distinct position and colour; UVs go on the primitives
	var vout []byte
	seen := map[[p1msVertSize]byte]int{}
	vid := func(v ps2Vert) int {
		var b [p1msVertSize]byte
		le.PutUint16(b[0:], q(v.x))
		le.PutUint16(b[2:], q(v.y))
		le.PutUint16(b[4:], q(v.z))
		b[8], b[9], b[10] = v.r, v.g, v.b
		if j, ok := seen[b]; ok {
			return j
		}
		j := len(seen)
		seen[b] = j
		vout = append(vout, b[:]...)
		return j
	}
	var pout []byte
	nprim := 0
	emit := func(ps []prim) {
		for _, pr := range ps {
			var b [p1msPrimSize]byte
			n := 3
			if pr.quad {
				n = 4
			}
			for k := 0; k < 4; k++ {
				id := 0xffff
				if k < n {
					id = vid(pr.v[k])
				}
				le.PutUint16(b[k*2:], uint16(id))
			}
			tex := int(pr.v[0].tex)
			if tex >= len(texs) {
				b[16] = noTex
			} else {
				b[16] = uint8(tex)
				uv, tiled := primUV(texs[tex], pr.v[:n])
				copy(b[8:16], uv[:])
				if tiled {
					texs[tex].tiled = true
				}
			}
			pout = append(pout, b[:]...)
			nprim++
		}
	}
	// each chunk's quads, then its triangles: the runtime draws each run
	// with a loop of its own
	type crange struct{ first, quads, tris int }
	ranges := make([]crange, len(chunks))
	for i, c := range chunks {
		ps := pairQuads(src[c.first : c.first+c.n])
		var qs, ts []prim
		for _, p := range ps {
			if p.quad {
				qs = append(qs, p)
			} else {
				ts = append(ts, p)
			}
		}
		ranges[i] = crange{first: nprim, quads: len(qs), tris: len(ts)}
		emit(qs)
		emit(ts)
	}
	if len(seen) > 65535 {
		return nil, fmt.Errorf("MESH: %d distinct vertices (the PS1 form holds 65535)", len(seen))
	}
	out := make([]byte, p1msHeader, p1msHeader+len(vout)+len(pout)+len(chunks)*p1msChunk+64)
	copy(out, "P1MS")
	le.PutUint16(out[4:], p1msVersion)
	le.PutUint16(out[6:], uint16(scale))
	le.PutUint32(out[8:], uint32(len(seen)))
	le.PutUint32(out[12:], uint32(nprim))
	le.PutUint32(out[16:], uint32(len(chunks)))
	out = append(out, vout...)
	out = append(out, pout...)
	for i, c := range chunks {
		var b [p1msChunk]byte
		le.PutUint32(b[0:], uint32(ranges[i].first))
		le.PutUint16(b[4:], uint16(ranges[i].quads))
		le.PutUint16(b[6:], c.node)
		for k := 0; k < 3; k++ {
			le.PutUint32(b[8+k*4:], math.Float32bits(c.c[k]))
		}
		le.PutUint32(b[20:], math.Float32bits(c.r))
		le.PutUint16(b[24:], uint16(ranges[i].tris))
		out = append(out, b[:]...)
	}
	extras := mesh[iend:]
	le.PutUint32(out[20:], uint32(len(extras)))
	out = append(out, extras...)
	return out, nil
}

// The largest power of two (at most 1024) that keeps every position inside
// an int16 with room to spare.
func meshScale(maxAbs float64) int {
	s := 1024
	for s > 1 && maxAbs*float64(s) > 30000 {
		s >>= 1
	}
	return s
}

// Texel UVs for one primitive (3 or 4 corners): the PS2 UVs times the
// texture size, shifted by whole repeats so the smallest is inside the first
// tile (the GPU's texture window wraps the rest). A primitive inside one
// tile keeps to its last texel.
func primUV(t ps2Tex, v []ps2Vert) ([8]byte, bool) {
	var out [8]byte
	tiled := false
	for axis := 0; axis < 2; axis++ {
		size := t.w
		if axis == 1 {
			size = t.h
		}
		if size <= 0 {
			size = 1
		}
		tc := make([]float64, len(v))
		lo := math.Inf(1)
		for k := range v {
			f := float64(v[k].u)
			if axis == 1 {
				f = float64(v[k].v)
			}
			if math.IsNaN(f) || math.IsInf(f, 0) {
				f = 0
			}
			tc[k] = f * float64(size)
			lo = math.Min(lo, tc[k])
		}
		shift := math.Floor(lo/float64(size)) * float64(size)
		hi := 0.0
		for k := range tc {
			tc[k] -= shift
			hi = math.Max(hi, tc[k])
		}
		if hi > float64(size)+0.5 {
			tiled = true
		}
		for k := range tc {
			c := math.Round(tc[k])
			if hi <= float64(size) {
				c = math.Min(c, float64(size-1))
			}
			c = math.Max(0, math.Min(255, c))
			out[k*2+axis] = uint8(c)
		}
	}
	return out, tiled
}

type chunk struct {
	first, n int
	node     uint16
	c        [3]float32
	r        float32
}

// Runs of up to chunkTris consecutive source triangles of one node no wider
// than chunkSpan, each with a bounding sphere (as the PS2 guest's
// build_chunks).
func buildChunks(ts []tri) []chunk {
	var out []chunk
	t := 0
	for t < len(ts) {
		c := chunk{first: t, node: ts[t].v[0].node}
		lo := [3]float64{1e9, 1e9, 1e9}
		hi := [3]float64{-1e9, -1e9, -1e9}
		for t < len(ts) && c.n < chunkTris {
			if ts[t].v[0].node != c.node {
				break
			}
			nlo, nhi := lo, hi
			for k := 0; k < 3; k++ {
				v := ts[t].v[k]
				p := [3]float64{float64(v.x), float64(v.y), float64(v.z)}
				for a := 0; a < 3; a++ {
					nlo[a] = math.Min(nlo[a], p[a])
					nhi[a] = math.Max(nhi[a], p[a])
				}
			}
			if c.n > 0 && (nhi[0]-nlo[0] > chunkSpan || nhi[1]-nlo[1] > chunkSpan || nhi[2]-nlo[2] > chunkSpan) {
				break
			}
			lo, hi = nlo, nhi
			c.n++
			t++
		}
		r2 := 0.0
		for a := 0; a < 3; a++ {
			c.c[a] = float32(0.5 * (lo[a] + hi[a]))
			h := 0.5 * (hi[a] - lo[a])
			r2 += h * h
		}
		c.r = float32(math.Sqrt(r2))
		out = append(out, c)
	}
	return out
}

// parseGTEX reads a GTEX blob's texture sizes and formats.
func parseGTEX(b []byte) []ps2Tex {
	if len(b) < 8 || string(b[:4]) != "GTEX" {
		return nil
	}
	le := binary.LittleEndian
	n := int(le.Uint16(b[6:]))
	off := 8
	var out []ps2Tex
	for i := 0; i < n && off+12 <= len(b); i++ {
		t := ps2Tex{w: int(le.Uint16(b[off:])), h: int(le.Uint16(b[off+2:])), psm: int(b[off+4])}
		clut := int(le.Uint16(b[off+6:]))
		data := int(le.Uint32(b[off+8:]))
		off += 12 + data + clut*2
		if off > len(b) {
			break
		}
		out = append(out, t)
	}
	return out
}

// ps1Color: the PS2 cook's RGB555 with bit 15 as "opaque" to the PS1's rule
// that only 0x0000 is see-through.
func ps1Color(c uint16) uint16 {
	if c&0x8000 == 0 {
		return 0
	}
	if c&0x7fff == 0 {
		return 0x8000
	}
	return c & 0x7fff
}

// P1TX texture flags (the byte after psm, unused by the PS2's GTEX)
const p1txTiled = 1

// ConvertGTEX rewrites a GTEX blob as P1TX: the same layout, PS1 colours,
// and the tiled flag for textures in tiled.
func ConvertGTEX(b []byte, texs []ps2Tex) ([]byte, error) {
	if len(b) < 8 || string(b[:4]) != "GTEX" {
		return nil, fmt.Errorf("not a GTEX blob")
	}
	le := binary.LittleEndian
	out := append([]byte(nil), b...)
	copy(out, "P1TX")
	n := int(le.Uint16(b[6:]))
	off := 8
	for i := 0; i < n && off+12 <= len(out); i++ {
		psm := int(out[off+4])
		out[off+5] = 0
		if i < len(texs) && texs[i].tiled {
			out[off+5] = p1txTiled
		}
		clut := int(le.Uint16(out[off+6:]))
		data := int(le.Uint32(out[off+8:]))
		off += 12
		if off+data+clut*2 > len(out) {
			return nil, fmt.Errorf("GTEX texture %d overruns the blob", i)
		}
		if psm == 2 {
			for p := off; p+1 < off+data; p += 2 {
				le.PutUint16(out[p:], ps1Color(le.Uint16(out[p:])))
			}
		}
		off += data
		for p := off; p < off+clut*2; p += 2 {
			le.PutUint16(out[p:], ps1Color(le.Uint16(out[p:])))
		}
		off += clut * 2
	}
	return out, nil
}

// ConvertPacks rewrites every MESHnn/GTEXnn under dir in place (PS2 forms
// become P1MS/P1TX; a file already converted is left alone), and the RAM
// cost STREAM.bin records for each converted pack (the guest checks a pack
// fits before loading it). Returns how many meshes were converted.
func ConvertPacks(dir string) (int, error) {
	ents, err := os.ReadDir(dir)
	if err != nil {
		return 0, err
	}
	done := 0
	saved := map[int]int{} // pack -> bytes the conversion saved
	for _, e := range ents {
		name := strings.ToUpper(e.Name())
		if e.IsDir() || !strings.HasPrefix(name, "MESH") || !strings.HasSuffix(name, ".BIN") {
			continue
		}
		num := strings.TrimSuffix(strings.TrimPrefix(name, "MESH"), ".BIN")
		meshPath := filepath.Join(dir, e.Name())
		mesh, err := os.ReadFile(meshPath)
		if err != nil {
			return done, err
		}
		if len(mesh) >= 4 && string(mesh[:4]) == "P1MS" {
			continue
		}
		var texs []ps2Tex
		var gtex []byte
		gtexPath := findCase(dir, "GTEX"+num+".bin")
		if gtexPath != "" {
			g, err := os.ReadFile(gtexPath)
			if err != nil {
				return done, err
			}
			if len(g) >= 4 && string(g[:4]) == "GTEX" {
				gtex = g
				texs = parseGTEX(g)
			}
		}
		p1, err := ConvertMesh(mesh, texs)
		if err != nil {
			return done, fmt.Errorf("%s: %w", meshPath, err)
		}
		delta := len(mesh) - len(p1)
		if gtex != nil {
			t1, err := ConvertGTEX(gtex, texs)
			if err != nil {
				return done, fmt.Errorf("%s: %w", gtexPath, err)
			}
			if err := os.WriteFile(gtexPath, t1, 0o644); err != nil {
				return done, err
			}
			delta += len(gtex) - len(t1)
		}
		if err := os.WriteFile(meshPath, p1, 0o644); err != nil {
			return done, err
		}
		if id, err := strconv.Atoi(num); err == nil {
			saved[id] = delta
		}
		done++
	}
	if len(saved) > 0 {
		if err := patchStreamCosts(dir, saved); err != nil {
			return done, err
		}
	}
	return done, nil
}

func findCase(dir, name string) string {
	ents, err := os.ReadDir(dir)
	if err != nil {
		return ""
	}
	for _, e := range ents {
		if strings.EqualFold(e.Name(), name) {
			return filepath.Join(dir, e.Name())
		}
	}
	return ""
}

// patchStreamCosts lowers STREAM.bin's per-pack RAM cost by what the
// conversion saved. STREAM: "STRM", u16 version 1, u16 count, u16 pack ids,
// then per pack: u8 path length, path, u32 RAM cost, u32 VRAM cost.
func patchStreamCosts(dir string, saved map[int]int) error {
	path := findCase(dir, "STREAM.bin")
	if path == "" {
		return nil
	}
	b, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	le := binary.LittleEndian
	if len(b) < 8 || string(b[:4]) != "STRM" || le.Uint16(b[4:]) != 1 {
		return nil
	}
	n := int(le.Uint16(b[6:]))
	off := 8 + n*2
	if off > len(b) {
		return nil
	}
	changed := false
	for i := 0; i < n && off < len(b); i++ {
		id := int(le.Uint16(b[8+i*2:]))
		l := int(b[off])
		off += 1 + l
		if off+8 > len(b) {
			break
		}
		if d, ok := saved[id]; ok && d != 0 {
			cost := int(le.Uint32(b[off:])) - d
			if cost < 0 {
				cost = 0
			}
			le.PutUint32(b[off:], uint32(cost))
			changed = true
		}
		off += 8
	}
	if !changed {
		return nil
	}
	return os.WriteFile(path, b, 0o644)
}
