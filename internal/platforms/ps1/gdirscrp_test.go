package ps1

import (
	"bytes"
	"encoding/binary"
	"testing"
)

// A minimal SCRP: no tape, a GDIR program with one constant, one class
// (one node ref, no functions), no code and two packs with 2 and 1 nodes.
func tinySCRP() ([]byte, [][]byte) {
	le := binary.LittleEndian
	var g []byte
	g = append(g, "GDIR"...)
	g = le.AppendUint16(g, 1)
	g = le.AppendUint16(g, 0)
	g = le.AppendUint32(g, 0) // total, set below
	g = le.AppendUint32(g, 1) // constants
	g = append(g, 2)          // int
	g = le.AppendUint32(g, 7)
	g = le.AppendUint16(g, 1)       // classes
	g = append(g, 0, 0, 1, 0, 0, 0) // members, noderefs 1, funcs 0
	g = append(g, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff)
	g = le.AppendUint32(g, 0) // code
	g = le.AppendUint16(g, 2) // packs
	var recs [][]byte
	for p, nn := range []int{2, 1} {
		g = append(g, byte(p+3), 1, 2, 3, 1)
		g = le.AppendUint16(g, 0)          // camera
		g = le.AppendUint32(g, 0x42000000) // fov
		g = le.AppendUint16(g, uint16(nn))
		g = le.AppendUint16(g, 0) // path
		var r []byte
		for n := 0; n < nn; n++ {
			r = append(r, bytes.Repeat([]byte{byte(0x10*p + n + 1)}, gdirNodeRecord)...)
		}
		recs = append(recs, r)
		g = append(g, r...)
		g = le.AppendUint16(g, 1)      // instances
		g = le.AppendUint16(g, 0)      // node
		g = le.AppendUint16(g, 0)      // class 0: one node ref
		g = le.AppendUint16(g, 0xabcd) // the ref
		g = append(g, 1, 0, 0, 0)      // one override
		g = le.AppendUint16(g, 1)      // connections
		g = append(g, 1, 2, 3, 4, 5, 6, 7)
	}
	le.PutUint32(g[8:], uint32(len(g)))
	var s []byte
	s = append(s, "SCRP"...)
	s = le.AppendUint16(s, 1)
	s = le.AppendUint16(s, 1) // GDIR
	s = le.AppendUint32(s, 0) // tape
	return append(s, g...), recs
}

func TestSplitSCRP(t *testing.T) {
	in, recs := tinySCRP()
	out, files, err := splitSCRP(in)
	if err != nil {
		t.Fatal(err)
	}
	if len(in)-len(out) != 3*gdirNodeRecord {
		t.Fatalf("took out %d bytes, want %d", len(in)-len(out), 3*gdirNodeRecord)
	}
	if !bytes.Equal(files[3], recs[0]) || !bytes.Equal(files[4], recs[1]) {
		t.Fatalf("PNOD contents wrong: %d / %d bytes", len(files[3]), len(files[4]))
	}
	le := binary.LittleEndian
	g := out[12:]
	if le.Uint16(g[6:])&gdirFlagPNOD == 0 || int(le.Uint32(g[8:])) != len(g) {
		t.Fatalf("flags %#x total %d of %d", le.Uint16(g[6:]), le.Uint32(g[8:]), len(g))
	}
	// the rest of each pack record survives: the instance's node ref
	if !bytes.Contains(out, []byte{0xcd, 0xab}) {
		t.Fatal("instance records lost")
	}
	// a second pass leaves it alone
	if again, _, err := splitSCRP(out); err != nil || again != nil {
		t.Fatalf("split twice: %v %v", again != nil, err)
	}
}
