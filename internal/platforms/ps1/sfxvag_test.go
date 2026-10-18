package ps1

import (
	"encoding/binary"
	"math"
	"os"
	"path/filepath"
	"testing"
)

// decodeVAG is the SPU's decoder, for the tests.
func decodeVAG(v []byte, n int) []int {
	out := make([]int, 0, n)
	h1, h2 := 0, 0
	for b := 0; len(out) < n && b*16+16 <= len(v); b++ {
		blk := v[b*16:]
		f, sh := int(blk[0]>>4), int(blk[0]&15)
		for i := 0; i < 28 && len(out) < n; i++ {
			nib := int(blk[2+i/2]>>(uint(i&1)*4)) & 15
			if nib >= 8 {
				nib -= 16
			}
			s := clamp16((nib << 12 >> sh) + (h1*vagF0[f]+h2*vagF1[f]+32)>>6)
			out = append(out, s)
			h2, h1 = h1, s
		}
	}
	return out
}

func snr(a, b []int) float64 {
	var s, e float64
	for i := range a {
		s += float64(a[i]) * float64(a[i])
		d := float64(a[i] - b[i])
		e += d * d
	}
	return 10 * math.Log10(s/math.Max(e, 1))
}

func TestEncodeVAG(t *testing.T) {
	pcm := make([]int, 5000)
	for i := range pcm {
		pcm[i] = int(12000*math.Sin(float64(i)*0.05) + 3000*math.Sin(float64(i)*0.9))
	}
	v := encodeVAG(pcm, false)
	if len(v) != ((5000+27)/28+1)*16 || v[len(v)-15] != 7 {
		t.Fatalf("size %d or end flag %d", len(v), v[len(v)-15])
	}
	if r := snr(pcm, decodeVAG(v, len(pcm))); r < 30 {
		t.Fatalf("SNR %.1f dB", r)
	}
	l := encodeVAG(pcm, true)
	if l[1]&4 == 0 || l[len(l)-15]&3 != 3 {
		t.Fatalf("loop flags %d %d", l[1], l[len(l)-15])
	}
}

func TestConvertSound(t *testing.T) {
	dir := t.TempDir()
	ima := make([]byte, 400)
	for i := range ima {
		ima[i] = byte(i*7) ^ 0x31
	}
	le := binary.LittleEndian
	bank := []byte("SFXB")
	bank = le.AppendUint16(bank, 1)
	bank = le.AppendUint16(bank, 2)
	for _, rate := range []uint16{11025, 22050} {
		bank = le.AppendUint16(bank, rate)
		bank = le.AppendUint32(bank, uint32(len(ima)))
		bank = append(bank, ima...)
	}
	mus := []byte("MUS ")
	mus = le.AppendUint16(mus, 1)
	mus = le.AppendUint16(mus, 22050)
	mus = le.AppendUint32(mus, uint32(len(ima)))
	mus = append(mus, ima...)
	os.WriteFile(filepath.Join(dir, "SFX00.BIN"), bank, 0o644)
	os.WriteFile(filepath.Join(dir, "MUSIC00.BIN"), mus, 0o644)
	n, err := ConvertSound(dir)
	if err != nil || n != 2 {
		t.Fatalf("ConvertSound: %d %v", n, err)
	}
	out, _ := os.ReadFile(filepath.Join(dir, "SFX00.BIN"))
	if string(out[:4]) != "SFXV" || le.Uint16(out[6:]) != 2 || le.Uint16(out[8:]) != 11025 {
		t.Fatalf("SFXV header % x", out[:12])
	}
	m, _ := os.ReadFile(filepath.Join(dir, "MUSIC00.BIN"))
	if string(m[:4]) != "MUSV" || le.Uint16(m[6:]) != 22050 {
		t.Fatalf("MUSV header % x", m[:12])
	}
	// converted files are left alone the second time
	if n, err := ConvertSound(dir); err != nil || n != 0 {
		t.Fatalf("second pass: %d %v", n, err)
	}
}
