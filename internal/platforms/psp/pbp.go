package psp

// The PSP's game folder: PSP/GAME/NAME/EBOOT.PBP, the package the XMB and
// PPSSPP start (PARAM.SFO, ICON0.PNG and the program as DATA.PSP), with the
// game's DATA/ beside it, and the same as a .zip to unpack onto a Memory
// Stick's root. PSPSDK's mksfo and pack-pbp do this; the CLI does it here.

import (
	"archive/zip"
	"bytes"
	"encoding/binary"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"

	_ "image/jpeg" // --icon may be a JPEG

	guest "github.com/blazium-games/blazium-toolchain/guest/psp"
)

const (
	iconW = 144
	iconH = 80
)

var gameNameRe = regexp.MustCompile(`^[A-Z0-9][A-Z0-9_-]{0,31}$`)

// gameName is the game folder's name from a title or file name: upper-case
// letters, digits, '-' and '_' ("Penguin Kart" -> "PENGUINKART").
func gameName(s string) string {
	var b strings.Builder
	for _, r := range strings.ToUpper(s) {
		if (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9') || r == '-' || r == '_' {
			b.WriteRune(r)
		}
	}
	n := strings.TrimLeft(b.String(), "-_")
	if len(n) > 32 {
		n = n[:32]
	}
	if n == "" {
		n = "BLAZIUMGAME"
	}
	return n
}

// PatchGameDir writes the game folder's name into the PRX's field (after
// guest.AppDirMarker), so the guest finds its folder whatever argv says.
func PatchGameDir(prx []byte, name string) ([]byte, error) {
	i := bytes.Index(prx, []byte(guest.AppDirMarker))
	if i < 0 || i+guest.AppDirBytes > len(prx) {
		return nil, fmt.Errorf("the PRX has no game folder field")
	}
	room := guest.AppDirBytes - len(guest.AppDirMarker) - 1
	if len(name) > room {
		return nil, fmt.Errorf("game folder name %q is longer than %d", name, room)
	}
	out := append([]byte(nil), prx...)
	f := out[i+len(guest.AppDirMarker) : i+guest.AppDirBytes]
	for k := range f {
		f[k] = 0
	}
	copy(f, name)
	return out, nil
}

type sfoEntry struct {
	key   string
	str   string
	num   uint32
	isN   bool
	align uint32
}

// ParamSFO is a PSP homebrew game's PARAM.SFO (the keys PSPSDK's mksfo
// writes): bootable, the "MG" category, the title, and MEMSIZE, which gives
// a PSP-2000 or later its larger memory for the heap.
func ParamSFO(title string) []byte {
	if len(title) > 127 {
		title = title[:127]
	}
	es := []sfoEntry{
		{key: "BOOTABLE", num: 1, isN: true},
		{key: "CATEGORY", str: "MG", align: 4},
		{key: "DISC_ID", str: "UCJS10041", align: 16},
		{key: "DISC_VERSION", str: "1.00", align: 8},
		{key: "MEMSIZE", num: 1, isN: true},
		{key: "PARENTAL_LEVEL", num: 1, isN: true},
		{key: "PSP_SYSTEM_VER", str: "1.00", align: 8},
		{key: "REGION", num: 0x8000, isN: true},
		{key: "TITLE", str: title, align: 128},
	}
	le := binary.LittleEndian
	alignUp := func(n, a uint32) uint32 { return (n + a - 1) / a * a }
	var keys, vals bytes.Buffer
	idx := make([]byte, 16*len(es))
	for i, e := range es {
		ent := idx[i*16:]
		le.PutUint16(ent[0:], uint16(keys.Len()))
		ent[2] = 4
		le.PutUint32(ent[12:], uint32(vals.Len()))
		keys.WriteString(e.key)
		keys.WriteByte(0)
		if e.isN {
			ent[3] = 4
			le.PutUint32(ent[4:], 4)
			le.PutUint32(ent[8:], 4)
			var v [4]byte
			le.PutUint32(v[:], e.num)
			vals.Write(v[:])
			continue
		}
		ent[3] = 2
		n := uint32(len(e.str)) + 1
		p := alignUp(n, e.align)
		le.PutUint32(ent[4:], n)
		le.PutUint32(ent[8:], p)
		vals.WriteString(e.str)
		vals.Write(make([]byte, p-n+1))
	}
	keyOff := uint32(0x14 + len(idx))
	valOff := alignUp(keyOff+uint32(keys.Len()), 4)
	hdr := make([]byte, 0x14)
	copy(hdr, "\x00PSF")
	le.PutUint32(hdr[4:], 0x101)
	le.PutUint32(hdr[8:], keyOff)
	le.PutUint32(hdr[12:], valOff)
	le.PutUint32(hdr[16:], uint32(len(es)))
	var out bytes.Buffer
	out.Write(hdr)
	out.Write(idx)
	out.Write(keys.Bytes())
	out.Write(make([]byte, valOff-keyOff-uint32(keys.Len())))
	out.Write(vals.Bytes())
	return out.Bytes()
}

// EbootPBP packs PARAM.SFO, ICON0.PNG and the program (DATA.PSP) as
// EBOOT.PBP: "\0PBP", the version, then the offsets of its eight parts
// (PARAM.SFO, ICON0.PNG, ICON1.PMF, PIC0.PNG, PIC1.PNG, SND0.AT3, DATA.PSP,
// DATA.PSAR; an absent part has the next one's offset) and the parts.
func EbootPBP(sfo, icon0, dataPSP []byte) []byte {
	parts := [][]byte{sfo, icon0, nil, nil, nil, nil, dataPSP, nil}
	le := binary.LittleEndian
	hdr := make([]byte, 0x28)
	copy(hdr, "\x00PBP")
	le.PutUint32(hdr[4:], 0x00010000)
	off := uint32(len(hdr))
	var body bytes.Buffer
	for i, p := range parts {
		le.PutUint32(hdr[8+i*4:], off)
		body.Write(p)
		off += uint32(len(p))
	}
	return append(hdr, body.Bytes()...)
}

// Icon is ICON0.PNG: the picture at iconPath (PNG or JPEG, any size)
// scaled and cropped to fill 144x80, or, without one, a plain gradient.
func Icon(iconPath string) ([]byte, error) {
	out := image.NewNRGBA(image.Rect(0, 0, iconW, iconH))
	if iconPath == "" {
		for y := 0; y < iconH; y++ {
			for x := 0; x < iconW; x++ {
				out.Set(x, y, color.NRGBA{uint8(24 + x/4), uint8(32 + y/3), uint8(96 + y), 255})
			}
		}
	} else {
		raw, err := os.ReadFile(iconPath)
		if err != nil {
			return nil, err
		}
		img, _, err := image.Decode(bytes.NewReader(raw))
		if err != nil {
			return nil, err
		}
		b := img.Bounds()
		cw, ch := b.Dx(), b.Dy()
		if cw*iconH > ch*iconW {
			cw = ch * iconW / iconH
		} else {
			ch = cw * iconH / iconW
		}
		x0 := b.Min.X + (b.Dx()-cw)/2
		y0 := b.Min.Y + (b.Dy()-ch)/2
		for y := 0; y < iconH; y++ {
			for x := 0; x < iconW; x++ {
				// the mean of the source pixels under this one (a box filter)
				sx0, sx1 := x0+x*cw/iconW, x0+(x+1)*cw/iconW
				sy0, sy1 := y0+y*ch/iconH, y0+(y+1)*ch/iconH
				if sx1 <= sx0 {
					sx1 = sx0 + 1
				}
				if sy1 <= sy0 {
					sy1 = sy0 + 1
				}
				var r, g, bl, a, n uint64
				for sy := sy0; sy < sy1; sy++ {
					for sx := sx0; sx < sx1; sx++ {
						cr, cg, cb, ca := img.At(sx, sy).RGBA()
						r, g, bl, a, n = r+uint64(cr), g+uint64(cg), bl+uint64(cb), a+uint64(ca), n+1
					}
				}
				out.Set(x, y, color.RGBA64{uint16(r / n), uint16(g / n), uint16(bl / n), uint16(a / n)})
			}
		}
	}
	var buf bytes.Buffer
	if err := png.Encode(&buf, out); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

// writeZip zips the tree under root (PSP/GAME/NAME/...) to zipPath, the
// paths relative to root and sorted, so the same game makes the same zip.
func writeZip(root, zipPath string, when time.Time) error {
	var files []string
	err := filepath.WalkDir(root, func(p string, d os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if !d.IsDir() {
			files = append(files, p)
		}
		return nil
	})
	if err != nil {
		return err
	}
	sort.Strings(files)
	f, err := os.Create(zipPath)
	if err != nil {
		return err
	}
	zw := zip.NewWriter(f)
	for _, p := range files {
		rel, err := filepath.Rel(root, p)
		if err != nil {
			return err
		}
		h := &zip.FileHeader{Name: filepath.ToSlash(rel), Method: zip.Deflate, Modified: when}
		w, err := zw.CreateHeader(h)
		if err != nil {
			return err
		}
		src, err := os.Open(p)
		if err != nil {
			return err
		}
		_, err = io.Copy(w, src)
		src.Close()
		if err != nil {
			return err
		}
	}
	if err := zw.Close(); err != nil {
		f.Close()
		return err
	}
	return f.Close()
}
