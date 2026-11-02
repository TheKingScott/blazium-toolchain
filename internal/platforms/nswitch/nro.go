package nswitch

// The homebrew .nro that the Switch's Homebrew Menu (hbmenu) lists and
// starts, and emulators load: the program (the guest's NRO), then its
// assets: an "ASET" header, the icon (a 256x256 JPEG), the NACP (the names
// the menu shows) and a RomFS (romfs:/ when the game runs: DATA/ and
// APPDIR.TXT). switch-tools' elf2nro, nacptool and build_romfs write these;
// the CLI does it here, byte for byte as they do, so a Windows build needs
// no devkitPro.

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"image"
	"image/color"
	"image/jpeg"
	"os"
	"sort"
	"strings"

	_ "image/png" // --icon may be a PNG
)

const (
	romfsEmpty   = 0xFFFFFFFF
	romfsDataOfs = 0x200 // the file partition
	romfsHdrSize = 0x50
	asetHdrSize  = 0x38
	nacpSize     = 0x4000
	iconSize     = 256
)

// romFile is a file of the RomFS: its path ("DATA/MESH00.BIN") and bytes.
type romFile struct {
	Path string
	Data []byte
}

type rDir struct {
	path     string // "/DATA" ("" the root)
	name     string
	parent   *rDir
	dirs     []*rDir
	files    []*rFile
	offset   uint32
	sibling  *rDir
	children map[string]*rDir
}

type rFile struct {
	path    string // "/DATA/MESH00.BIN"
	name    string
	parent  *rDir
	data    []byte
	offset  uint32 // in the file table
	dataOfs uint64 // in the file partition
	sibling *rFile
}

func align64(v, a uint64) uint64 { return (v + a - 1) &^ (a - 1) }

func romHash(parent uint32, name string) uint32 {
	h := parent ^ 123456789
	for i := 0; i < len(name); i++ {
		h = (h>>5 | h<<27) ^ uint32(name[i])
	}
	return h
}

func hashCount(n uint32) uint32 {
	if n < 3 {
		return 3
	}
	if n < 19 {
		return n | 1
	}
	for n%2 == 0 || n%3 == 0 || n%5 == 0 || n%7 == 0 || n%11 == 0 || n%13 == 0 || n%17 == 0 {
		n++
	}
	return n
}

// RomFS builds the RomFS of files as build_romfs does: directories and
// files in the order of their full paths, file data aligned to 16 bytes
// from 0x200, then the hash tables and entry tables, little-endian.
func RomFS(files []romFile) ([]byte, error) {
	root := &rDir{children: map[string]*rDir{}}
	var allDirs = []*rDir{root}
	var allFiles []*rFile
	dirOf := func(dir string) *rDir {
		d := root
		if dir == "" {
			return d
		}
		for _, part := range strings.Split(dir, "/") {
			c := d.children[part]
			if c == nil {
				c = &rDir{path: d.path + "/" + part, name: part, parent: d, children: map[string]*rDir{}}
				d.children[part] = c
				d.dirs = append(d.dirs, c)
				allDirs = append(allDirs, c)
			}
			d = c
		}
		return d
	}
	seen := map[string]bool{}
	for _, f := range files {
		p := f.Path
		if p == "" || strings.HasPrefix(p, "/") || strings.HasSuffix(p, "/") || strings.Contains(p, "\\") || strings.Contains(p, "//") {
			return nil, fmt.Errorf("bad path %q", p)
		}
		if seen[p] {
			return nil, fmt.Errorf("%s twice", p)
		}
		seen[p] = true
		dir, name := "", p
		if i := strings.LastIndex(p, "/"); i >= 0 {
			dir, name = p[:i], p[i+1:]
		}
		d := dirOf(dir)
		rf := &rFile{path: "/" + p, name: name, parent: d, data: f.Data}
		d.files = append(d.files, rf)
		allFiles = append(allFiles, rf)
	}
	// build_romfs keeps every list in strcmp order of the full paths
	sort.Slice(allDirs, func(i, j int) bool { return allDirs[i].path < allDirs[j].path })
	sort.Slice(allFiles, func(i, j int) bool { return allFiles[i].path < allFiles[j].path })
	for _, d := range allDirs {
		sort.Slice(d.dirs, func(i, j int) bool { return d.dirs[i].path < d.dirs[j].path })
		sort.Slice(d.files, func(i, j int) bool { return d.files[i].path < d.files[j].path })
		for i := 0; i+1 < len(d.dirs); i++ {
			d.dirs[i].sibling = d.dirs[i+1]
		}
		for i := 0; i+1 < len(d.files); i++ {
			d.files[i].sibling = d.files[i+1]
		}
	}

	var dirTable, fileTable, part uint64
	var eo uint32
	for _, d := range allDirs {
		d.offset = eo
		if d == root {
			eo += 0x18
		} else {
			eo += 0x18 + uint32(align64(uint64(len(d.name)), 4))
		}
	}
	dirTable = uint64(eo)
	eo = 0
	for _, f := range allFiles {
		part = align64(part, 0x10)
		f.dataOfs = part
		part += uint64(len(f.data))
		f.offset = eo
		eo += 0x20 + uint32(align64(uint64(len(f.name)), 4))
	}
	fileTable = uint64(eo)

	dirHashN, fileHashN := hashCount(uint32(len(allDirs))), hashCount(uint32(len(allFiles)))
	dirHash := make([]uint32, dirHashN)
	fileHash := make([]uint32, fileHashN)
	for i := range dirHash {
		dirHash[i] = romfsEmpty
	}
	for i := range fileHash {
		fileHash[i] = romfsEmpty
	}
	le := binary.LittleEndian
	ft := make([]byte, fileTable)
	for _, f := range allFiles {
		e := ft[f.offset:]
		le.PutUint32(e[0:], f.parent.offset)
		sib := uint32(romfsEmpty)
		if f.sibling != nil {
			sib = f.sibling.offset
		}
		le.PutUint32(e[4:], sib)
		le.PutUint64(e[8:], f.dataOfs)
		le.PutUint64(e[16:], uint64(len(f.data)))
		h := romHash(f.parent.offset, f.name)
		le.PutUint32(e[24:], fileHash[h%fileHashN])
		fileHash[h%fileHashN] = f.offset
		le.PutUint32(e[28:], uint32(len(f.name)))
		copy(e[32:], f.name)
	}
	dt := make([]byte, dirTable)
	for _, d := range allDirs {
		e := dt[d.offset:]
		parent := d.offset
		if d.parent != nil {
			parent = d.parent.offset
		}
		le.PutUint32(e[0:], parent)
		sib, child, file := uint32(romfsEmpty), uint32(romfsEmpty), uint32(romfsEmpty)
		if d.sibling != nil {
			sib = d.sibling.offset
		}
		if len(d.dirs) > 0 {
			child = d.dirs[0].offset
		}
		if len(d.files) > 0 {
			file = d.files[0].offset
		}
		le.PutUint32(e[4:], sib)
		le.PutUint32(e[8:], child)
		le.PutUint32(e[12:], file)
		var h uint32
		if d == root {
			h = romHash(0, "")
		} else {
			h = romHash(parent, d.name)
		}
		le.PutUint32(e[16:], dirHash[h%dirHashN])
		dirHash[h%dirHashN] = d.offset
		le.PutUint32(e[20:], uint32(len(d.name)))
		copy(e[24:], d.name)
	}

	dirHashOfs := align64(part+romfsDataOfs, 4)
	dirTableOfs := dirHashOfs + uint64(dirHashN)*4
	fileHashOfs := dirTableOfs + dirTable
	fileTableOfs := fileHashOfs + uint64(fileHashN)*4
	out := make([]byte, fileTableOfs+fileTable)
	for i, v := range []uint64{romfsHdrSize, dirHashOfs, uint64(dirHashN) * 4, dirTableOfs, dirTable,
		fileHashOfs, uint64(fileHashN) * 4, fileTableOfs, fileTable, romfsDataOfs} {
		le.PutUint64(out[8*i:], v)
	}
	for _, f := range allFiles {
		copy(out[romfsDataOfs+f.dataOfs:], f.data)
	}
	for i, v := range dirHash {
		le.PutUint32(out[dirHashOfs+uint64(i)*4:], v)
	}
	copy(out[dirTableOfs:], dt)
	for i, v := range fileHash {
		le.PutUint32(out[fileHashOfs+uint64(i)*4:], v)
	}
	copy(out[fileTableOfs:], ft)
	return out, nil
}

// NACP is control.nacp as nacptool --create writes it: the name and author
// in every language's entry, the version, and nacptool's fixed fields.
func NACP(name, author, version string) []byte {
	b := make([]byte, nacpSize)
	put := func(off, n int, s string) {
		if len(s) > n-1 {
			s = s[:n-1]
		}
		copy(b[off:off+n], s)
	}
	for i := 0; i < 12; i++ {
		put(i*0x300, 0x200, name)
		put(i*0x300+0x200, 0x100, author)
	}
	le := binary.LittleEndian
	le.PutUint32(b[0x3024:], 0x100)
	le.PutUint32(b[0x302C:], 0xbff)
	le.PutUint32(b[0x3034:], 0x10000)
	copy(b[0x3040:], []byte{0x0C, 0xFF, 0xFF, 0x0A, 0xFF, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0D, 0x0D, 0xFF, 0xFF, 0xFF, 0xFF,
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF})
	put(0x3060, 0x10, version)
	le.PutUint32(b[0x3080:], 0x3e00000)
	le.PutUint32(b[0x3088:], 0x180000)
	le.PutUint32(b[0x30F0:], 0x102)
	return b
}

// WithAssets is the program nro (its header's size; anything after is
// dropped) with the ASET section after it, as elf2nro --icon --nacp
// --romfs writes it.
func WithAssets(nro, icon, nacp, romfs []byte) ([]byte, error) {
	if len(nro) < 0x80 || string(nro[0x10:0x14]) != "NRO0" {
		return nil, fmt.Errorf("not an NRO")
	}
	size := int(binary.LittleEndian.Uint32(nro[0x18:]))
	if size < 0x80 || size > len(nro) {
		return nil, fmt.Errorf("the NRO's size (%d) is past its end (%d)", size, len(nro))
	}
	le := binary.LittleEndian
	hdr := make([]byte, asetHdrSize)
	copy(hdr, "ASET")
	off := uint64(asetHdrSize)
	for i, part := range [][]byte{icon, nacp, romfs} {
		if len(part) == 0 {
			continue
		}
		le.PutUint64(hdr[8+16*i:], off)
		le.PutUint64(hdr[16+16*i:], uint64(len(part)))
		off += uint64(len(part))
	}
	out := make([]byte, 0, size+int(off))
	out = append(out, nro[:size]...)
	out = append(out, hdr...)
	out = append(out, icon...)
	out = append(out, nacp...)
	out = append(out, romfs...)
	return out, nil
}

// loadPicture reads a PNG or JPEG.
func loadPicture(p string) (image.Image, error) {
	raw, err := os.ReadFile(p)
	if err != nil {
		return nil, err
	}
	img, _, err := image.Decode(bytes.NewReader(raw))
	return img, err
}

// Icon is the menu's icon, a 256x256 baseline JPEG: the picture scaled and
// cropped to fill it (each pixel the mean of the source pixels under it,
// laid on black), or, without one, a plain gradient.
func Icon(img image.Image) ([]byte, error) {
	out := image.NewRGBA(image.Rect(0, 0, iconSize, iconSize))
	if img == nil {
		for y := 0; y < iconSize; y++ {
			for x := 0; x < iconSize; x++ {
				out.SetRGBA(x, y, color.RGBA{uint8(24 + x/8), uint8(32 + y/6), uint8(96 + y/2), 255})
			}
		}
	} else {
		b := img.Bounds()
		cw, ch := b.Dx(), b.Dy()
		if cw > ch {
			cw = ch
		} else {
			ch = cw
		}
		x0, y0 := b.Min.X+(b.Dx()-cw)/2, b.Min.Y+(b.Dy()-ch)/2
		for y := 0; y < iconSize; y++ {
			for x := 0; x < iconSize; x++ {
				sx0, sx1 := x0+x*cw/iconSize, x0+(x+1)*cw/iconSize
				sy0, sy1 := y0+y*ch/iconSize, y0+(y+1)*ch/iconSize
				if sx1 <= sx0 {
					sx1 = sx0 + 1
				}
				if sy1 <= sy0 {
					sy1 = sy0 + 1
				}
				var r, g, bl, n uint64
				for sy := sy0; sy < sy1; sy++ {
					for sx := sx0; sx < sx1; sx++ {
						cr, cg, cb, _ := img.At(sx, sy).RGBA() // premultiplied: on black
						r, g, bl, n = r+uint64(cr), g+uint64(cg), bl+uint64(cb), n+1
					}
				}
				out.SetRGBA(x, y, color.RGBA{uint8(r / n >> 8), uint8(g / n >> 8), uint8(bl / n >> 8), 255})
			}
		}
	}
	var buf bytes.Buffer
	if err := jpeg.Encode(&buf, out, &jpeg.Options{Quality: 92}); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}
