package wiiu

// The .wuhb (Wii U Homebrew Bundle) that the Aroma environment's Homebrew
// Launcher and the Wii U Menu start, and Cemu loads: a RomFS with
// code/root.rpx, meta/ (meta.ini, the icon and the two boot pictures as
// gzipped TGAs) and content/ (mounted at /vol/content when the game runs).
// wut-tools' wuhbtool writes it; the CLI does it here, in the same layout
// and order (wuhbtool's RomFSService), so a Windows build needs no wut.

import (
	"bytes"
	"compress/gzip"
	"encoding/binary"
	"fmt"
	"image"
	"image/color"
	"os"
	"path"
	"strings"

	_ "image/jpeg" // --icon may be a JPEG
	_ "image/png"
)

const (
	romfsEmpty   = 0xFFFFFFFF
	romfsDataOfs = 0x200 // the file partition
	romfsHdrSize = 0x50
)

// bundleFile is a file of the bundle: its path ("code/root.rpx") and bytes.
type bundleFile struct {
	Path string
	Data []byte
}

type romDir struct {
	name    string
	parent  *romDir
	dirs    []*romDir
	files   []*romFile
	offset  uint32 // in the directory table
	sibling *romDir
}

type romFile struct {
	name    string
	parent  *romDir
	data    []byte
	offset  uint32 // in the file table
	dataOfs uint64 // in the file partition
	sibling *romFile
}

func align(v, a uint64) uint64 { return (v + a - 1) &^ (a - 1) }

func romHash(parent uint32, name string) uint32 {
	h := parent ^ 123456789
	for i := 0; i < len(name); i++ {
		c := name[i]
		if c >= 'a' && c <= 'z' {
			c -= 'a' - 'A'
		}
		h = (h>>5 | h<<27) ^ uint32(c)
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

// WUHB writes the bundle, its directories and files in the order given
// (wuhbtool's is code/root.rpx, meta/meta.ini, the icon, the TV and the
// GamePad pictures, then content/).
func WUHB(files []bundleFile) ([]byte, error) {
	root := &romDir{}
	get := func(dir string) *romDir {
		d := root
		if dir == "." || dir == "" {
			return d
		}
		for _, part := range strings.Split(dir, "/") {
			var next *romDir
			for _, c := range d.dirs {
				if c.name == part {
					next = c
				}
			}
			if next == nil {
				next = &romDir{name: part, parent: d}
				d.dirs = append(d.dirs, next)
			}
			d = next
		}
		return d
	}
	for _, f := range files {
		if f.Path == "" || strings.HasPrefix(f.Path, "/") || strings.Contains(f.Path, "\\") {
			return nil, fmt.Errorf("bad path %q", f.Path)
		}
		d := get(path.Dir(f.Path))
		d.files = append(d.files, &romFile{name: path.Base(f.Path), parent: d, data: f.Data})
	}

	// sizes; wuhbtool counts the root twice (its table starts with the
	// root's 0x18 bytes and the root adds itself again), so the directory
	// table ends with 0x18 bytes to spare: the same here
	numDirs, dirTable := uint32(1), uint64(0x18)
	numFiles, fileTable := uint32(0), uint64(0)
	var count func(d *romDir)
	count = func(d *romDir) {
		numDirs++
		dirTable += 0x18 + align(uint64(len(d.name)), 4)
		for _, c := range d.dirs {
			count(c)
		}
		for _, f := range d.files {
			numFiles++
			fileTable += 0x20 + align(uint64(len(f.name)), 4)
		}
	}
	count(root)

	// directory offsets: the root, then depth first
	var dofs uint32
	var dirOffsets func(d *romDir)
	dirOffsets = func(d *romDir) {
		d.offset = dofs
		if d == root {
			dofs += 0x18
		} else {
			dofs += 0x18 + uint32(align(uint64(len(d.name)), 4))
		}
		for _, c := range d.dirs {
			dirOffsets(c)
		}
	}
	dirOffsets(root)
	// file offsets and data: a directory's subdirectories first, then its own
	var fofs uint32
	var part uint64
	var fileOffsets func(d *romDir)
	fileOffsets = func(d *romDir) {
		for _, c := range d.dirs {
			fileOffsets(c)
		}
		for _, f := range d.files {
			part = align(part, 0x10)
			f.dataOfs = part
			part += uint64(len(f.data))
			f.offset = fofs
			fofs += 0x20 + uint32(align(uint64(len(f.name)), 4))
		}
	}
	fileOffsets(root)

	dirHashN, fileHashN := hashCount(numDirs), hashCount(numFiles)
	dirHash := make([]uint32, dirHashN)
	fileHash := make([]uint32, fileHashN)
	for i := range dirHash {
		dirHash[i] = romfsEmpty
	}
	for i := range fileHash {
		fileHash[i] = romfsEmpty
	}
	dt := make([]byte, dirTable)
	ft := make([]byte, fileTable)
	be := binary.BigEndian
	var populate func(d *romDir)
	populate = func(d *romDir) {
		for i, c := range d.dirs {
			if i+1 < len(d.dirs) {
				c.sibling = d.dirs[i+1]
			}
		}
		for i, f := range d.files {
			if i+1 < len(d.files) {
				f.sibling = d.files[i+1]
			}
		}
		e := dt[d.offset:]
		parent := d.offset
		if d.parent != nil {
			parent = d.parent.offset
		}
		be.PutUint32(e[0:], parent)
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
		be.PutUint32(e[4:], sib)
		be.PutUint32(e[8:], child)
		be.PutUint32(e[12:], file)
		var h uint32
		if d == root {
			h = romHash(0, "")
		} else {
			h = romHash(parent, d.name)
		}
		be.PutUint32(e[16:], dirHash[h%dirHashN])
		dirHash[h%dirHashN] = d.offset
		be.PutUint32(e[20:], uint32(len(d.name)))
		copy(e[24:], d.name)
		for _, c := range d.dirs {
			populate(c)
		}
		for _, f := range d.files {
			e := ft[f.offset:]
			be.PutUint32(e[0:], d.offset)
			sib := uint32(romfsEmpty)
			if f.sibling != nil {
				sib = f.sibling.offset
			}
			be.PutUint32(e[4:], sib)
			be.PutUint64(e[8:], f.dataOfs)
			be.PutUint64(e[16:], uint64(len(f.data)))
			h := romHash(d.offset, f.name)
			be.PutUint32(e[24:], fileHash[h%fileHashN])
			fileHash[h%fileHashN] = f.offset
			be.PutUint32(e[28:], uint32(len(f.name)))
			copy(e[32:], f.name)
		}
	}
	populate(root)

	dirHashOfs := align(part+romfsDataOfs, 4)
	dirTableOfs := dirHashOfs + uint64(dirHashN)*4
	fileHashOfs := dirTableOfs + dirTable
	fileTableOfs := fileHashOfs + uint64(fileHashN)*4
	out := make([]byte, fileTableOfs+fileTable)
	copy(out, "WUHB")
	be.PutUint32(out[4:], romfsHdrSize)
	for i, v := range []uint64{dirHashOfs, uint64(dirHashN) * 4, dirTableOfs, dirTable,
		fileHashOfs, uint64(fileHashN) * 4, fileTableOfs, fileTable, romfsDataOfs} {
		be.PutUint64(out[8+8*i:], v)
	}
	var data func(d *romDir)
	data = func(d *romDir) {
		for _, c := range d.dirs {
			data(c)
		}
		for _, f := range d.files {
			copy(out[romfsDataOfs+f.dataOfs:], f.data)
		}
	}
	data(root)
	for i, v := range dirHash {
		be.PutUint32(out[dirHashOfs+uint64(i)*4:], v)
	}
	copy(out[dirTableOfs:], dt)
	for i, v := range fileHash {
		be.PutUint32(out[fileHashOfs+uint64(i)*4:], v)
	}
	copy(out[fileTableOfs:], ft)
	return out, nil
}

// MetaINI is meta/meta.ini: the names the menu shows, as wuhbtool writes it.
func MetaINI(longName, shortName, author string) []byte {
	clean := func(s string) string {
		return strings.TrimSpace(strings.NewReplacer("\r", " ", "\n", " ").Replace(s))
	}
	return []byte(fmt.Sprintf("[menu]\nlongname=%s\nshortname=%s\nauthor=%s\n", clean(longName), clean(shortName), clean(author)))
}

// TGA is an uncompressed TGA of img (bpp 24 or 32), rows from the bottom up,
// as FreeImage (wuhbtool) writes them.
func TGA(img image.Image, bpp int) []byte {
	b := img.Bounds()
	w, h := b.Dx(), b.Dy()
	px := bpp / 8
	out := make([]byte, 18, 18+w*h*px)
	out[2] = 2 // uncompressed true-colour
	binary.LittleEndian.PutUint16(out[12:], uint16(w))
	binary.LittleEndian.PutUint16(out[14:], uint16(h))
	out[16] = byte(bpp)
	if bpp == 32 {
		out[17] = 8 // 8 bits of alpha, bottom-left origin
	}
	for y := h - 1; y >= 0; y-- {
		for x := 0; x < w; x++ {
			c := color.NRGBAModel.Convert(img.At(b.Min.X+x, b.Min.Y+y)).(color.NRGBA)
			if bpp == 32 {
				out = append(out, c.B, c.G, c.R, c.A)
			} else {
				// on black, as a 24-bit picture shows it
				a := uint32(c.A)
				out = append(out, byte(uint32(c.B)*a/255), byte(uint32(c.G)*a/255), byte(uint32(c.R)*a/255))
			}
		}
	}
	return out
}

// gz is data gzipped (best compression, no name or time: the same bytes
// every build).
func gz(data []byte) []byte {
	var buf bytes.Buffer
	w, _ := gzip.NewWriterLevel(&buf, gzip.BestCompression)
	_, _ = w.Write(data)
	_ = w.Close()
	return buf.Bytes()
}

// The picture sizes the bundle's meta/ takes.
const (
	iconSize      = 128
	tvW, tvH      = 1280, 720
	drcW, drcH    = 854, 480
	bootBGR       = 0x10
	bootBGG       = 0x14
	bootBGB       = 0x24
	defaultAuthor = "Built with Blazium"
)

// loadPicture reads a PNG or JPEG.
func loadPicture(p string) (image.Image, error) {
	raw, err := os.ReadFile(p)
	if err != nil {
		return nil, err
	}
	img, _, err := image.Decode(bytes.NewReader(raw))
	return img, err
}

// scale draws src's rectangle (sx0, sy0, sw, sh) into dst's (dx0, dy0, dw,
// dh), each pixel the mean of the source pixels under it (a box filter),
// over what dst has there.
func scale(dst *image.NRGBA, dx0, dy0, dw, dh int, src image.Image, sx0, sy0, sw, sh int) {
	for y := 0; y < dh; y++ {
		for x := 0; x < dw; x++ {
			ax0, ax1 := sx0+x*sw/dw, sx0+(x+1)*sw/dw
			ay0, ay1 := sy0+y*sh/dh, sy0+(y+1)*sh/dh
			if ax1 <= ax0 {
				ax1 = ax0 + 1
			}
			if ay1 <= ay0 {
				ay1 = ay0 + 1
			}
			var r, g, bl, a, n uint64
			for sy := ay0; sy < ay1; sy++ {
				for sx := ax0; sx < ax1; sx++ {
					cr, cg, cb, ca := src.At(sx, sy).RGBA()
					r, g, bl, a, n = r+uint64(cr), g+uint64(cg), bl+uint64(cb), a+uint64(ca), n+1
				}
			}
			// premultiplied means, laid over dst
			pr, pg, pb, pa := r/n, g/n, bl/n, a/n
			under := dst.NRGBAAt(dx0+x, dy0+y)
			inv := 0xffff - pa
			mix := func(p uint64, u uint8) uint8 { return uint8((p + uint64(u)*0x101*inv/0xffff) >> 8) }
			outA := uint8((pa + uint64(under.A)*0x101*inv/0xffff) >> 8)
			c := color.NRGBA{mix(pr, under.R), mix(pg, under.G), mix(pb, under.B), outA}
			if outA != 0 && outA != 255 {
				// back to straight alpha
				c.R = uint8(min(255, uint32(c.R)*255/uint32(outA)))
				c.G = uint8(min(255, uint32(c.G)*255/uint32(outA)))
				c.B = uint8(min(255, uint32(c.B)*255/uint32(outA)))
			}
			dst.SetNRGBA(dx0+x, dy0+y, c)
		}
	}
}

// Icon is the 128x128 icon: the picture scaled and cropped to fill it, or,
// without one, a plain gradient.
func Icon(img image.Image) image.Image {
	out := image.NewNRGBA(image.Rect(0, 0, iconSize, iconSize))
	if img == nil {
		for y := 0; y < iconSize; y++ {
			for x := 0; x < iconSize; x++ {
				out.SetNRGBA(x, y, color.NRGBA{uint8(24 + x/4), uint8(32 + y/3), uint8(96 + y), 255})
			}
		}
		return out
	}
	b := img.Bounds()
	cw, ch := b.Dx(), b.Dy()
	if cw > ch {
		cw = ch
	} else {
		ch = cw
	}
	scale(out, 0, 0, iconSize, iconSize, img, b.Min.X+(b.Dx()-cw)/2, b.Min.Y+(b.Dy()-ch)/2, cw, ch)
	return out
}

// BootPicture is a boot screen (w x h): the picture, or the icon, whole and
// as large as fits half the height, centred on a dark blue.
func BootPicture(img image.Image, w, h int) image.Image {
	out := image.NewNRGBA(image.Rect(0, 0, w, h))
	bg := color.NRGBA{bootBGR, bootBGG, bootBGB, 255}
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			out.SetNRGBA(x, y, bg)
		}
	}
	if img == nil {
		img = Icon(nil)
	}
	b := img.Bounds()
	dh := h / 2
	dw := b.Dx() * dh / b.Dy()
	if dw > w*3/4 {
		dw = w * 3 / 4
		dh = b.Dy() * dw / b.Dx()
	}
	scale(out, (w-dw)/2, (h-dh)/2, dw, dh, img, b.Min.X, b.Min.Y, b.Dx(), b.Dy())
	return out
}

// MetaFiles are meta/: meta.ini, the icon and the TV and GamePad boot
// pictures, from the picture at picPath (or none).
func MetaFiles(title, author, picPath string) ([]bundleFile, error) {
	var pic image.Image
	if picPath != "" {
		var err error
		if pic, err = loadPicture(picPath); err != nil {
			return nil, err
		}
	}
	if author == "" {
		author = defaultAuthor
	}
	short := title
	if len([]rune(short)) > 64 {
		short = string([]rune(short)[:64])
	}
	return []bundleFile{
		{"meta/meta.ini", MetaINI(title, short, author)},
		{"meta/iconTex.tga.gz", gz(TGA(Icon(pic), 32))},
		{"meta/bootTvTex.tga.gz", gz(TGA(BootPicture(pic, tvW, tvH), 24))},
		{"meta/bootDrcTex.tga.gz", gz(TGA(BootPicture(pic, drcW, drcH), 24))},
	}, nil
}
