package ps1

import (
	"encoding/binary"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// MCPS1.BIN is the PS1 guest's memory card save identity (mc_io.cpp), made
// from the export's MCSAVE.BIN (the PS2 save's directory name, title and
// icon): "MCP1", u16 1, u16 0, the save file name but its slot digit
// (20 bytes, NUL padded), then the 256-byte PS1 save header every slot's
// file starts with (frame 0: "SC", one icon frame, one block, the title in
// Shift-JIS, the icon's palette; frame 1: the 16x16 4-bit icon).
const mcps1Name = "MCPS1.BIN"

// MakeMCPS1 writes MCPS1.BIN into dir from its MCSAVE.BIN, for the disc's
// boot name (SLUS_000.01 style), and drops MCSAVE.BIN (the PS1 doesn't
// read it). False when there is no MCSAVE.BIN.
func MakeMCPS1(dir, boot string) (bool, error) {
	src := findCase(dir, "MCSAVE.BIN")
	if src == "" {
		return false, nil
	}
	raw, err := os.ReadFile(src)
	if err != nil {
		return false, err
	}
	out, err := mcps1FromMCSave(raw, boot)
	if err != nil {
		return false, err
	}
	if err := os.WriteFile(filepath.Join(dir, mcps1Name), out, 0o644); err != nil {
		return false, err
	}
	return true, os.Remove(src)
}

func mcps1FromMCSave(raw []byte, boot string) ([]byte, error) {
	le := binary.LittleEndian
	if len(raw) < 76 || string(raw[:4]) != "MCSV" {
		return nil, fmt.Errorf("MCSAVE.BIN is not an MCSV blob")
	}
	dirName := cString(raw[8:40])
	title := cString(raw[40:72])
	icoLen := int(le.Uint32(raw[72:]))
	var ico []byte
	if 76+icoLen <= len(raw) {
		ico = raw[76 : 76+icoLen]
	}
	out := make([]byte, 8+20+256)
	copy(out, "MCP1")
	le.PutUint16(out[4:], 1)
	copy(out[8:28], mcps1FileName(dirName, boot))
	h := out[28:]
	h[0], h[1] = 'S', 'C'
	h[2] = 0x11 // one icon frame
	h[3] = 1    // one block
	copy(h[4:68], sjisTitle(title))
	pal, pix := mcps1Icon(ico)
	for i, c := range pal {
		le.PutUint16(h[0x60+i*2:], c)
	}
	copy(h[0x80:], pix)
	return out, nil
}

func cString(b []byte) string {
	if i := strings.IndexByte(string(b), 0); i >= 0 {
		b = b[:i]
	}
	return string(b)
}

// mcps1FileName: the region and the product code the BIOS browser groups
// saves by ("BA" + "BLAZ-00001" for BLAZ_000.01; the PS2 name's "BE" for
// a PAL export), then up to 7 letters of the game's name; the guest adds
// the slot digit (20 characters at most).
func mcps1FileName(dirName, boot string) string {
	region := "BA"
	if strings.HasPrefix(dirName, "BE") {
		region = "BE"
	}
	code := strings.ToUpper(boot)
	if len(code) == 11 && code[4] == '_' && code[8] == '.' {
		code = code[:4] + "-" + code[5:8] + code[9:11]
	} else {
		code = "BLAZ-00001"
	}
	var game strings.Builder
	for _, r := range strings.TrimPrefix(strings.TrimPrefix(dirName, "BA"), "BE") {
		if game.Len() >= 7 {
			break
		}
		if (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9') {
			game.WriteRune(r)
		} else if r >= 'a' && r <= 'z' {
			game.WriteRune(r - 32)
		}
	}
	if game.Len() == 0 {
		game.WriteString("SAVE")
	}
	return region + code + game.String()
}

// sjisTitle: the save's title as the BIOS browser shows it, in full-width
// Shift-JIS (32 characters, 64 bytes).
func sjisTitle(t string) []byte {
	out := make([]byte, 0, 64)
	for _, r := range t {
		if len(out) >= 64 {
			break
		}
		var c uint16 = 0x8140 // full-width space
		switch {
		case r >= 'A' && r <= 'Z':
			c = 0x8260 + uint16(r-'A')
		case r >= 'a' && r <= 'z':
			c = 0x8281 + uint16(r-'a')
		case r >= '0' && r <= '9':
			c = 0x824f + uint16(r-'0')
		default:
			if v, ok := sjisPunct[r]; ok {
				c = v
			}
		}
		out = append(out, byte(c>>8), byte(c))
	}
	return out
}

var sjisPunct = map[rune]uint16{
	'!': 0x8149, '?': 0x8148, '-': 0x817c, ':': 0x8146, '.': 0x8144, ',': 0x8143,
	'\'': 0x8166, '&': 0x8195, '(': 0x8169, ')': 0x816a, '/': 0x815e, '+': 0x817b,
	'#': 0x8194, '%': 0x8193, '*': 0x8196, '=': 0x8181, '_': 0x8151,
}

// mcps1Icon: the PS2 icon's texture (the last 128x128 RGB555 of the .icn,
// red lowest, top bit opaque) brought down to the PS1's 16x16 in 16
// colours. A missing or short icon gives a plain blue square.
func mcps1Icon(ico []byte) ([16]uint16, []byte) {
	const tw = 128
	var px [256][3]float64
	if len(ico) >= tw*tw*2 {
		tex := ico[len(ico)-tw*tw*2:]
		for y := 0; y < 16; y++ {
			for x := 0; x < 16; x++ {
				var s [3]float64
				for dy := 0; dy < 8; dy++ {
					for dx := 0; dx < 8; dx++ {
						v := binary.LittleEndian.Uint16(tex[((y*8+dy)*tw+x*8+dx)*2:])
						s[0] += float64(v & 31)
						s[1] += float64((v >> 5) & 31)
						s[2] += float64((v >> 10) & 31)
					}
				}
				for k := range s {
					px[y*16+x][k] = s[k] / 64
				}
			}
		}
	} else {
		for i := range px {
			px[i] = [3]float64{9, 14, 25}
		}
	}
	pal := quantize16(px[:])
	var out16 [16]uint16
	for i, c := range pal {
		r, g, b := uint16(c[0]+0.5), uint16(c[1]+0.5), uint16(c[2]+0.5)
		// 0x0000 is see-through: an all-black colour keeps its STP bit
		out16[i] = 0x8000 | b<<10 | g<<5 | r
	}
	pix := make([]byte, 128)
	for i := 0; i < 256; i += 2 {
		a := nearest(pal, px[i])
		b := nearest(pal, px[i+1])
		pix[i/2] = byte(a) | byte(b)<<4 // the left pixel in the low nibble
	}
	return out16, pix
}

// quantize16: 16 colours for the pixels, by k-means from a spread start.
func quantize16(px [][3]float64) [16][3]float64 {
	var c [16][3]float64
	for i := range c {
		c[i] = px[(i*len(px))/16]
	}
	for it := 0; it < 12; it++ {
		var sum [16][3]float64
		var n [16]float64
		for _, p := range px {
			k := nearest(c, p)
			for j := 0; j < 3; j++ {
				sum[k][j] += p[j]
			}
			n[k]++
		}
		for k := range c {
			if n[k] > 0 {
				for j := 0; j < 3; j++ {
					c[k][j] = sum[k][j] / n[k]
				}
			}
		}
	}
	return c
}

func nearest(c [16][3]float64, p [3]float64) int {
	best, bd := 0, 1e18
	for k := range c {
		d := (c[k][0]-p[0])*(c[k][0]-p[0]) + (c[k][1]-p[1])*(c[k][1]-p[1]) + (c[k][2]-p[2])*(c[k][2]-p[2])
		if d < bd {
			best, bd = k, d
		}
	}
	return best
}
