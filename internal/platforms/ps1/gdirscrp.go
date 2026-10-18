package ps1

// The GDIR program (inside SCRP00) carries every scene's node records, 88
// bytes a node: on a big project that is most of the program, and the PS1
// guest keeps the program in RAM for good. SplitPackNodes moves them to one
// PNODnn.BIN per pack (in the pack's disc folder) and sets GDIR flag bit 0,
// so the VM reads a scene's records from the disc when it enters the scene.

import (
	"encoding/binary"
	"fmt"
	"os"
	"path/filepath"
)

const (
	gdirFlagPNOD   = 1
	gdirNodeRecord = 88
)

// SplitPackNodes rewrites dir's SCRP00.BIN without the pack node records
// and writes them as PNODnn.BIN. Returns the bytes taken out of SCRP00 (0
// when there is no GDIR program, or it was split already).
func SplitPackNodes(dir string) (int, error) {
	path := findCase(dir, "SCRP00.BIN")
	if path == "" {
		return 0, nil
	}
	b, err := os.ReadFile(path)
	if err != nil {
		return 0, err
	}
	out, files, err := splitSCRP(b)
	if err != nil {
		return 0, fmt.Errorf("%s: %w", path, err)
	}
	if out == nil {
		return 0, nil
	}
	for pack, recs := range files {
		if err := os.WriteFile(filepath.Join(dir, fmt.Sprintf("PNOD%02d.BIN", pack)), recs, 0o644); err != nil {
			return 0, err
		}
	}
	if err := os.WriteFile(path, out, 0o644); err != nil {
		return 0, err
	}
	return len(b) - len(out), nil
}

// splitSCRP: the SCRP blob without node records, and the records by pack.
// nil, nil, nil when there is nothing to do.
func splitSCRP(b []byte) ([]byte, map[int][]byte, error) {
	le := binary.LittleEndian
	if len(b) < 12 || string(b[:4]) != "SCRP" || le.Uint16(b[6:])&1 == 0 {
		return nil, nil, nil
	}
	tape := int(le.Uint32(b[8:]))
	g0 := 12 + tape
	if g0+12 > len(b) {
		return nil, nil, fmt.Errorf("SCRP: tape runs past the end")
	}
	g := b[g0:]
	if string(g[:4]) != "GDIR" {
		return nil, nil, nil
	}
	if le.Uint16(g[6:])&gdirFlagPNOD != 0 {
		return nil, nil, nil
	}
	total := int(le.Uint32(g[8:]))
	if total > len(g) {
		return nil, nil, fmt.Errorf("GDIR: %d bytes promised, %d there", total, len(g))
	}
	g = g[:total]
	r := &gdirReader{b: g, p: 12}
	// constants
	kn := r.u32()
	for i := 0; i < kn && r.err == nil; i++ {
		switch r.u8() {
		case 0: // nil
		case 1:
			r.skip(1)
		case 2, 3:
			r.skip(4)
		case 4:
			r.skip(r.u16() + 1)
		case 5:
			r.skip(8)
		case 6:
			r.skip(12)
		case 7:
			r.skip(r.u16() * 2)
		default:
			r.fail("unknown constant type")
		}
	}
	// classes: node-ref counts for the instance records
	cn := r.u16()
	noderefs := make([]int, cn)
	for c := 0; c < cn && r.err == nil; c++ {
		noderefs[c] = int(le.Uint16(g[r.p+2:]))
		funcs := int(le.Uint16(g[r.p+4:]))
		r.skip(14 + funcs*8)
	}
	r.skip(r.u32()) // code
	np := r.u16()
	out := append([]byte(nil), g[:r.p]...)
	files := map[int][]byte{}
	for i := 0; i < np && r.err == nil; i++ {
		start := r.p
		r.skip(15)
		if r.err != nil {
			break
		}
		idx := int(g[start])
		nn := int(le.Uint16(g[start+11:]))
		out = append(out, g[start:r.p]...)
		nodes := r.take(nn * gdirNodeRecord)
		files[idx] = append([]byte(nil), nodes...)
		rest := r.p
		in := r.u16()
		for k := 0; k < in && r.err == nil; k++ {
			cls := int(le.Uint16(g[r.p+2:]))
			nr := 0
			if cls < len(noderefs) {
				nr = noderefs[cls]
			}
			r.skip(4 + nr*2)
			r.skip(r.u8() * 3)
		}
		r.skip(r.u16() * 7)
		if r.err == nil {
			out = append(out, g[rest:r.p]...)
		}
	}
	if r.err != nil {
		return nil, nil, r.err
	}
	out = append(out, g[r.p:]...)
	le.PutUint16(out[6:], le.Uint16(out[6:])|gdirFlagPNOD)
	le.PutUint32(out[8:], uint32(len(out)))
	whole := append(append([]byte(nil), b[:g0]...), out...)
	return whole, files, nil
}

type gdirReader struct {
	b   []byte
	p   int
	err error
}

func (r *gdirReader) fail(msg string) {
	if r.err == nil {
		r.err = fmt.Errorf("GDIR at %d: %s", r.p, msg)
	}
}

func (r *gdirReader) take(n int) []byte {
	if r.err != nil || n < 0 || r.p+n > len(r.b) {
		r.fail("cut short")
		return nil
	}
	s := r.b[r.p : r.p+n]
	r.p += n
	return s
}

func (r *gdirReader) skip(n int) { r.take(n) }

func (r *gdirReader) u8() int {
	s := r.take(1)
	if s == nil {
		return 0
	}
	return int(s[0])
}

func (r *gdirReader) u16() int {
	s := r.take(2)
	if s == nil {
		return 0
	}
	return int(binary.LittleEndian.Uint16(s))
}

func (r *gdirReader) u32() int {
	s := r.take(4)
	if s == nil {
		return 0
	}
	return int(binary.LittleEndian.Uint32(s))
}
