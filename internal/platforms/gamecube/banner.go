package gamecube

// opening.bnr: the banner the GameCube's menu, Dolphin's game list and
// Swiss show for a disc (BNR1). A 96x32 picture in RGB5A3, 4x4 tiles, and
// the game's names.
//
//	0x0000 "BNR1", padding to 0x20
//	0x0020 the picture, 96x32 RGB5A3 (0x1800 bytes)
//	0x1820 short name (0x20), short maker (0x20), name (0x40), maker (0x40),
//	       description (0x80)

import (
	"bytes"
	"encoding/binary"
	"image"
	_ "image/jpeg" // --icon may be a JPEG
	_ "image/png"
	"os"
)

const (
	bannerW    = 96
	bannerH    = 32
	bannerSize = 0x1960
)

// rgb5a3 of 8-bit RGBA: opaque as 1RRRRRGGGGGBBBBB, else 0AAARRRRGGGGBBBB.
func rgb5a3(r, g, b, a uint8) uint16 {
	if a >= 0xe0 {
		return 0x8000 | uint16(r>>3)<<10 | uint16(g>>3)<<5 | uint16(b>>3)
	}
	return uint16(a>>5)<<12 | uint16(r>>4)<<8 | uint16(g>>4)<<4 | uint16(b>>4)
}

// bannerPixels is the picture: the image scaled to 96x32 (cropped to fill),
// or, without one, a plain gradient.
func bannerPixels(img image.Image) [bannerH][bannerW]uint16 {
	var px [bannerH][bannerW]uint16
	if img == nil {
		for y := 0; y < bannerH; y++ {
			for x := 0; x < bannerW; x++ {
				px[y][x] = rgb5a3(uint8(24+x/3), uint8(32+y), uint8(96+y*3), 255)
			}
		}
		return px
	}
	b := img.Bounds()
	// crop to 3:1 about the middle, then nearest
	cw, ch := b.Dx(), b.Dy()
	if cw*bannerH > ch*bannerW {
		cw = ch * bannerW / bannerH
	} else {
		ch = cw * bannerH / bannerW
	}
	x0 := b.Min.X + (b.Dx()-cw)/2
	y0 := b.Min.Y + (b.Dy()-ch)/2
	for y := 0; y < bannerH; y++ {
		for x := 0; x < bannerW; x++ {
			r, g, bl, a := img.At(x0+(x*cw+cw/2)/bannerW, y0+(y*ch+ch/2)/bannerH).RGBA()
			px[y][x] = rgb5a3(uint8(r>>8), uint8(g>>8), uint8(bl>>8), uint8(a>>8))
		}
	}
	return px
}

func putText(dst []byte, s string) {
	// the menu reads Latin-1 (Shift JIS on Japanese consoles): ASCII only
	n := 0
	for _, r := range s {
		if n >= len(dst)-1 {
			break
		}
		if r < 0x20 || r > 0x7e {
			r = '?'
		}
		dst[n] = byte(r)
		n++
	}
}

// Banner is opening.bnr for the title, with the picture from iconPath (a
// PNG or JPEG of any size; empty for the plain one).
func Banner(title, maker, description, iconPath string) ([]byte, error) {
	var img image.Image
	if iconPath != "" {
		raw, err := os.ReadFile(iconPath)
		if err != nil {
			return nil, err
		}
		img, _, err = image.Decode(bytes.NewReader(raw))
		if err != nil {
			return nil, err
		}
	}
	px := bannerPixels(img)
	out := make([]byte, bannerSize)
	copy(out, "BNR1")
	o := 0x20
	for ty := 0; ty < bannerH; ty += 4 {
		for tx := 0; tx < bannerW; tx += 4 {
			for y := 0; y < 4; y++ {
				for x := 0; x < 4; x++ {
					binary.BigEndian.PutUint16(out[o:], px[ty+y][tx+x])
					o += 2
				}
			}
		}
	}
	putText(out[0x1820:0x1840], title)
	putText(out[0x1840:0x1860], maker)
	putText(out[0x1860:0x18a0], title)
	putText(out[0x18a0:0x18e0], maker)
	putText(out[0x18e0:0x1960], description)
	return out, nil
}
