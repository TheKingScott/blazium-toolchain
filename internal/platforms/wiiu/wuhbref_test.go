package wiiu

import (
	"bytes"
	"encoding/binary"
	"os"
	"testing"
)

// WUHB_REF names a .wuhb from wuhbtool: the same files through WUHB give
// the same bytes (its directories hold one subdirectory or one file each,
// as wuhbtool lists a folder in the order the OS does).
func TestWUHBMatchesWuhbtool(t *testing.T) {
	ref := os.Getenv("WUHB_REF")
	if ref == "" {
		t.Skip("WUHB_REF not set")
	}
	b, err := os.ReadFile(ref)
	if err != nil {
		t.Fatal(err)
	}
	be := binary.BigEndian
	dt, ft, part := be.Uint64(b[0x18:]), be.Uint64(b[0x38:]), be.Uint64(b[0x48:])
	var files []bundleFile
	var walk func(d uint32, pre string)
	walk = func(d uint32, pre string) {
		e := b[dt+uint64(d):]
		n := be.Uint32(e[20:])
		name := pre + string(e[24:24+n])
		if name != "" {
			name += "/"
		}
		for f := be.Uint32(e[12:]); f != romfsEmpty; {
			fe := b[ft+uint64(f):]
			fn := be.Uint32(fe[28:])
			o := part + be.Uint64(fe[8:])
			files = append(files, bundleFile{name + string(fe[32:32+fn]), b[o : o+be.Uint64(fe[16:])]})
			f = be.Uint32(fe[4:])
		}
		for c := be.Uint32(e[8:]); c != romfsEmpty; c = be.Uint32(b[dt+uint64(c)+4:]) {
			walk(c, name)
		}
	}
	walk(0, "")
	// walk lists a directory's files before its subdirectories; WUHB takes
	// them in creation order, which for these trees is the same
	got, err := WUHB(files)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, b) {
		n := len(got)
		if len(b) < n {
			n = len(b)
		}
		i := 0
		for i < n && got[i] == b[i] {
			i++
		}
		t.Fatalf("differs at %#x (len %d, want %d)", i, len(got), len(b))
	}
	t.Logf("%d files, %d bytes: identical", len(files), len(b))
}
