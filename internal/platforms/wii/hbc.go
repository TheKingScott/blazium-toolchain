package wii

// The Homebrew Channel app: apps/NAME/ with boot.dol, meta.xml (the name and
// text the channel shows), icon.png (128x48) and the game's DATA/, and the
// same folder as a .zip to unpack onto the SD card's root.

import (
	"archive/zip"
	"bytes"
	"encoding/xml"
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

	guest "github.com/blazium-games/blazium-toolchain/guest/wii"
)

const (
	iconW = 128
	iconH = 48
)

var appNameRe = regexp.MustCompile(`^[a-z0-9][a-z0-9_-]{0,39}$`)

// appName is the app folder's name from a title or file name: lower case
// letters, digits, '-' and '_' ("Penguin Arcade Kart" -> "penguinarcadekart").
func appName(s string) string {
	var b strings.Builder
	for _, r := range strings.ToLower(s) {
		if (r >= 'a' && r <= 'z') || (r >= '0' && r <= '9') || r == '-' || r == '_' {
			b.WriteRune(r)
		}
	}
	n := strings.TrimLeft(b.String(), "-_")
	if len(n) > 40 {
		n = n[:40]
	}
	if n == "" {
		n = "blaziumgame"
	}
	return n
}

// PatchAppDir writes the app folder's name into the DOL's field (after
// guest.AppDirMarker), so the guest finds its folder when the loader passes
// no argv.
func PatchAppDir(dol []byte, name string) ([]byte, error) {
	i := bytes.Index(dol, []byte(guest.AppDirMarker))
	if i < 0 || i+guest.AppDirBytes > len(dol) {
		return nil, fmt.Errorf("boot.dol has no app folder field")
	}
	room := guest.AppDirBytes - len(guest.AppDirMarker) - 1
	if len(name) > room {
		return nil, fmt.Errorf("app folder name %q is longer than %d", name, room)
	}
	out := append([]byte(nil), dol...)
	f := out[i+len(guest.AppDirMarker) : i+guest.AppDirBytes]
	for k := range f {
		f[k] = 0
	}
	copy(f, name)
	return out, nil
}

type metaXML struct {
	XMLName     xml.Name `xml:"app"`
	Version     string   `xml:"version,attr"`
	Name        string   `xml:"name"`
	Coder       string   `xml:"coder"`
	AppVersion  string   `xml:"version"`
	ReleaseDate string   `xml:"release_date"`
	Short       string   `xml:"short_description"`
	Long        string   `xml:"long_description"`
}

// MetaXML is the Homebrew Channel's meta.xml for the game.
func MetaXML(title string, when time.Time) []byte {
	m := metaXML{
		Version:     "1",
		Name:        title,
		Coder:       "Blazium",
		AppVersion:  "1.0",
		ReleaseDate: when.UTC().Format("20060102150405"),
		Short:       "Made with Blazium Engine",
		Long:        title + ", made with Blazium Engine. HOME returns to the Homebrew Channel.",
	}
	raw, _ := xml.MarshalIndent(m, "", "\t")
	return append(append([]byte(xml.Header), raw...), '\n')
}

// Icon is icon.png: the picture at iconPath (PNG or JPEG, any size) scaled
// and cropped to fill 128x48, or, without one, a plain gradient.
func Icon(iconPath string) ([]byte, error) {
	out := image.NewNRGBA(image.Rect(0, 0, iconW, iconH))
	if iconPath == "" {
		for y := 0; y < iconH; y++ {
			for x := 0; x < iconW; x++ {
				out.Set(x, y, color.NRGBA{uint8(24 + x/4), uint8(32 + y/2), uint8(96 + y*2), 255})
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

// writeZip zips the tree under root (apps/NAME/...) to zipPath, the paths
// relative to root and sorted, so the same game makes the same zip.
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
