package ps1

import (
	"encoding/binary"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// The export's sound (SFXnn.BIN: "SFXB" bank or one "SFX " clip; MUSICnn.BIN:
// "MUS ") is IMA ADPCM. ConvertSound rewrites it as the PS1 SPU's own ADPCM
// (VAG blocks), so the guest only copies it to SPU RAM (sfx_io.cpp): encoding
// on the PS1 took over 2 s at boot.
//
//	SFXV: "SFXV", u16 1, u16 count, then per clip u16 rate, u32 bytes, the
//	      blocks (a clip ends with a silent block that loops on itself)
//	MUSV: "MUSV", u16 1, u16 rate, u32 bytes, the blocks (looping: the first
//	      block starts the loop, the last ends it)
func ConvertSound(dir string) (int, error) {
	ents, err := os.ReadDir(dir)
	if err != nil {
		return 0, err
	}
	n := 0
	for _, e := range ents {
		up := strings.ToUpper(e.Name())
		if e.IsDir() || !strings.HasSuffix(up, ".BIN") {
			continue
		}
		isSfx := strings.HasPrefix(up, "SFX")
		isMus := strings.HasPrefix(up, "MUSIC")
		if !isSfx && !isMus {
			continue
		}
		path := filepath.Join(dir, e.Name())
		raw, err := os.ReadFile(path)
		if err != nil {
			return n, err
		}
		var out []byte
		if isSfx {
			out, err = sfxToVAG(raw)
		} else {
			out, err = musicToVAG(raw)
		}
		if err != nil {
			return n, fmt.Errorf("%s: %w", e.Name(), err)
		}
		if out == nil {
			continue // already converted, or not ours
		}
		if err := os.WriteFile(path, out, 0o644); err != nil {
			return n, err
		}
		n++
	}
	return n, nil
}

func sfxToVAG(raw []byte) ([]byte, error) {
	le := binary.LittleEndian
	if len(raw) < 8 || string(raw[:3]) != "SFX" || le.Uint16(raw[4:]) != 1 {
		return nil, nil
	}
	type clip struct {
		rate int
		ima  []byte
	}
	var clips []clip
	switch raw[3] {
	case ' ':
		if len(raw) < 12 {
			return nil, fmt.Errorf("short SFX clip")
		}
		nb := int(le.Uint32(raw[8:]))
		if 12+nb > len(raw) {
			return nil, fmt.Errorf("SFX clip overruns the file")
		}
		clips = append(clips, clip{int(le.Uint16(raw[6:])), raw[12 : 12+nb]})
	case 'B':
		count := int(le.Uint16(raw[6:]))
		off := 8
		for i := 0; i < count && off+6 <= len(raw); i++ {
			rate := int(le.Uint16(raw[off:]))
			nb := int(le.Uint32(raw[off+2:]))
			off += 6
			if off+nb > len(raw) {
				return nil, fmt.Errorf("SFX bank clip %d overruns the file", i)
			}
			clips = append(clips, clip{rate, raw[off : off+nb]})
			off += nb
		}
	default:
		return nil, nil // SFXV: done already
	}
	out := []byte("SFXV")
	out = le.AppendUint16(out, 1)
	out = le.AppendUint16(out, uint16(len(clips)))
	for _, c := range clips {
		v := encodeVAG(decodeIMA(c.ima), false)
		out = le.AppendUint16(out, uint16(c.rate))
		out = le.AppendUint32(out, uint32(len(v)))
		out = append(out, v...)
	}
	return out, nil
}

func musicToVAG(raw []byte) ([]byte, error) {
	le := binary.LittleEndian
	if len(raw) < 12 || string(raw[:4]) != "MUS " || le.Uint16(raw[4:]) != 1 {
		return nil, nil
	}
	nb := int(le.Uint32(raw[8:]))
	if 12+nb > len(raw) {
		return nil, fmt.Errorf("music overruns the file")
	}
	v := encodeVAG(decodeIMA(raw[12:12+nb]), true)
	out := []byte("MUSV")
	out = le.AppendUint16(out, 1)
	out = le.AppendUint16(out, le.Uint16(raw[6:]))
	out = le.AppendUint32(out, uint32(len(v)))
	return append(out, v...), nil
}

var imaStep = [89]int{
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
	50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
	337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
	2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
	15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
}
var imaIndex = [16]int{-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8}

// decodeIMA: the export's IMA ADPCM (low nibble first, from 0 and index 0).
func decodeIMA(b []byte) []int {
	out := make([]int, 0, len(b)*2)
	pred, idx := 0, 0
	for _, by := range b {
		for _, nib := range [2]int{int(by & 15), int(by >> 4)} {
			step := imaStep[idx]
			d := step >> 3
			if nib&4 != 0 {
				d += step
			}
			if nib&2 != 0 {
				d += step >> 1
			}
			if nib&1 != 0 {
				d += step >> 2
			}
			if nib&8 != 0 {
				pred -= d
			} else {
				pred += d
			}
			pred = clamp16(pred)
			idx += imaIndex[nib]
			if idx < 0 {
				idx = 0
			} else if idx > 88 {
				idx = 88
			}
			out = append(out, pred)
		}
	}
	return out
}

func clamp16(v int) int {
	if v > 32767 {
		return 32767
	}
	if v < -32768 {
		return -32768
	}
	return v
}

// the SPU's prediction filters (x64)
var vagF0 = [5]int{0, 60, 115, 98, 122}
var vagF1 = [5]int{0, 0, -52, -55, -60}

// encodeVAG: SPU ADPCM, every filter and shift tried for each 28-sample
// block against the decoder's own history (what the SPU will rebuild).
func encodeVAG(pcm []int, loop bool) []byte {
	blocks := (len(pcm) + 27) / 28
	if blocks == 0 {
		blocks = 1
	}
	total := blocks
	if !loop {
		total++ // the silent end block
	}
	out := make([]byte, total*16)
	h1, h2 := 0, 0
	for b := 0; b < blocks; b++ {
		var x [28]int
		for i := range x {
			if j := b*28 + i; j < len(pcm) {
				x[i] = pcm[j]
			}
		}
		bestErr := int64(-1)
		var bestNib [28]int
		bf, bs, b1, b2 := 0, 0, 0, 0
		for f := 0; f < 5; f++ {
			for sh := 0; sh <= 12; sh++ {
				var nib [28]int
				t1, t2 := h1, h2
				var e int64
				for i := 0; i < 28; i++ {
					pred := (t1*vagF0[f] + t2*vagF1[f] + 32) >> 6
					step := 1 << (12 - sh)
					q := (x[i] - pred + step/2) >> (12 - sh)
					if q > 7 {
						q = 7
					} else if q < -8 {
						q = -8
					}
					y := clamp16(q<<(12-sh) + pred)
					d := int64(x[i] - y)
					e += d * d
					nib[i] = q & 15
					t2, t1 = t1, y
				}
				if bestErr < 0 || e < bestErr {
					bestErr, bestNib, bf, bs, b1, b2 = e, nib, f, sh, t1, t2
				}
			}
		}
		h1, h2 = b1, b2
		blk := out[b*16:]
		blk[0] = byte(bf<<4 | bs)
		for i := 0; i < 14; i++ {
			blk[2+i] = byte(bestNib[i*2] | bestNib[i*2+1]<<4)
		}
		if loop {
			if b == 0 {
				blk[1] |= 4
			}
			if b == blocks-1 {
				blk[1] |= 3
			}
		}
	}
	if !loop {
		out[blocks*16+1] = 7 // silent, looping on itself: the voice parks there
	}
	return out
}
