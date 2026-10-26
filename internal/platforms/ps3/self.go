package ps3

// An NPDRM SELF (EBOOT.BIN) from a PS3 ELF, as PSL1GHT's make_self_npdrm
// (geohot's ps3publictools) makes it: each program segment deflated and
// encrypted with a key of its own, the SELF headers, the NPDRM block for
// the content ID ("free" licence, no RAP), the metadata signed with the
// public homebrew key pair and encrypted. It needs no SDK tools, so a game
// builds on any host. The constants are make_self_npdrm's (PSL1GHT,
// tools/geohot, GPLv3).

import (
	"bytes"
	"compress/zlib"
	"crypto/aes"
	"crypto/cipher"
	"crypto/hmac"
	"crypto/sha1"
	"encoding/binary"
	"fmt"
	"math/big"
	"path"
	"strings"
)

var (
	npdrmR  = []byte{0xA3, 0x8B, 0xCB, 0x3E, 0x4E, 0x73, 0x09, 0x90, 0x4A, 0xEF, 0xDF, 0xC5, 0x04, 0x7D, 0x0F, 0xDF, 0x06, 0xE3, 0x5C, 0x0D}
	npdrmN  = []byte{0xB0, 0xE7, 0xCA, 0xFF, 0xC8, 0xDE, 0xEE, 0x8A, 0x55, 0xA3, 0x05, 0x0D, 0x80, 0x9A, 0xDF, 0xE3, 0x8F, 0xA0, 0x1D, 0xAB}
	npdrmK  = []byte{0x97, 0x4E, 0x90, 0x62, 0x79, 0x70, 0x5E, 0x58, 0xD9, 0xDA, 0xB4, 0xBF, 0xDA, 0x8B, 0xDA, 0x93, 0x13, 0x51, 0x17, 0xE2}
	npdrmDa = []byte{0x04, 0x0A, 0xB4, 0x75, 0x09, 0xBE, 0xD0, 0x4B, 0xD9, 0x65, 0x21, 0xAD, 0x1B, 0x36, 0x5B, 0x86, 0xBF, 0x62, 0x0A, 0x98}

	npdrmKeypairE = []byte{
		0xA1, 0xC0, 0x13, 0xAB, 0xCE, 0x98, 0xA7, 0xE3, 0xDC, 0x69, 0x92, 0x3B, 0x07, 0xC0, 0x28, 0x5F,
		0x75, 0x54, 0xC5, 0x12, 0xB0, 0xB0, 0xA9, 0x6F, 0x24, 0x52, 0x40, 0xF2, 0xFD, 0x43, 0x3A, 0xF2,
		0x3F, 0x4E, 0xFE, 0xC6, 0xC1, 0x83, 0xEA, 0x37, 0x8D, 0x1B, 0xEC, 0xB0, 0x9D, 0x88, 0xDB, 0x32,
		0x8F, 0x2C, 0x86, 0x37, 0xB7, 0xAC, 0x72, 0x05, 0x9B, 0x15, 0x56, 0xB0, 0xD9, 0x5B, 0x5B, 0xE0}
	npdrmKeypairD = []byte{
		0x87, 0xC7, 0x4F, 0xFE, 0x66, 0x93, 0x0B, 0xAA, 0xA1, 0x6F, 0x86, 0x40, 0x91, 0xC5, 0x66, 0xFB,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x08, 0x28, 0xB5, 0x8F, 0xAC, 0xF9, 0xDE, 0xC8, 0xD7, 0x0D, 0xFE, 0xF0, 0xF3, 0x76, 0x63, 0xAE,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}

	npdrmOmacKey1 = []byte{0x72, 0xF9, 0x90, 0x78, 0x8F, 0x9C, 0xFF, 0x74, 0x57, 0x25, 0xF0, 0x8E, 0x4C, 0x12, 0x83, 0x87}
	npdrmOmacKey2 = []byte{0x6B, 0xA5, 0x29, 0x76, 0xEF, 0xDA, 0x16, 0xEF, 0x3C, 0x33, 0x9F, 0xB2, 0x97, 0x1E, 0x25, 0x6B}
	npdrmOmacKey3 = []byte{0x9B, 0x51, 0x5F, 0xEA, 0xCF, 0x75, 0x06, 0x49, 0x81, 0xAA, 0x60, 0x4D, 0x91, 0xA5, 0x4E, 0x97}

	nubPadding = []byte{
		0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3B, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x20, 0x00}
	cflagsNPDRM = []byte{
		0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
		0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
		0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
		0x62, 0x7C, 0xB1, 0x80, 0x8A, 0xB9, 0x38, 0xE3, 0x2C, 0x8C, 0x09, 0x17, 0x08, 0x72, 0x6A, 0x57,
		0x9E, 0x25, 0x86, 0xE4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5D, 0xC0}
	sdkVersion = []byte{0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00}
)

var be = binary.BigEndian

// aesOmac1: AES-CMAC (make_self's aesOmac1Mode).
func aesOmac1(key, input []byte) []byte {
	blk, _ := aes.NewCipher(key)
	sub := make([]byte, 16)
	blk.Encrypt(sub, sub)
	rol1 := func(b []byte) {
		x := byte(0)
		if b[0]&0x80 != 0 {
			x = 0x87
		}
		for i := 0; i < 15; i++ {
			b[i] = b[i]<<1 | b[i+1]>>7
		}
		b[15] = b[15]<<1 ^ x
	}
	rol1(sub)
	running := make([]byte, 16)
	hash := make([]byte, 16)
	i := 0
	if len(input) > 16 {
		for i = 0; i < len(input)-16; i += 16 {
			for j := 0; j < 16; j++ {
				hash[j] = running[j] ^ input[i+j]
			}
			blk.Encrypt(running, hash)
		}
	}
	over := len(input) & 15
	if over == 0 {
		over = 16
	}
	for j := range hash {
		hash[j] = 0
	}
	copy(hash, input[i:i+over])
	if over != 16 {
		hash[over] = 0x80
		rol1(sub)
	}
	for j := 0; j < 16; j++ {
		hash[j] ^= running[j] ^ sub[j]
	}
	out := make([]byte, 16)
	blk.Encrypt(out, hash)
	return out
}

func ctrCrypt(key, iv, data []byte) {
	blk, _ := aes.NewCipher(key)
	cipher.NewCTR(blk, iv).XORKeyStream(data, data)
}

// derived: per-segment keys from the ELF itself, so a build is repeatable
func derived(label string, seed []byte, n int) []byte {
	var out []byte
	for c := 0; len(out) < n; c++ {
		h := sha1.New()
		fmt.Fprintf(h, "%s/%d/", label, c)
		h.Write(seed)
		out = append(out, h.Sum(nil)...)
	}
	return out[:n]
}

type selfSegment struct {
	pm   []byte // Self_PMhdr, 0x20
	enc  []byte // segment_certification_segment, 0x30
	sha1 []byte // 0x20
	hmac []byte // 0x40
	erk  []byte // 0x10
	riv  []byte // 0x10
	data []byte
	pad  int
}

// MakeSelfNPDRM signs elf (stripped, sprxlinked) as an NPDRM SELF for
// contentID; filename is what it will be called (EBOOT.BIN): the NPDRM
// block hashes it.
func MakeSelfNPDRM(elf []byte, contentID, filename string) ([]byte, error) {
	if len(elf) < 0x40 || !bytes.Equal(elf[:4], []byte{0x7f, 'E', 'L', 'F'}) || elf[4] != 2 || elf[5] != 2 {
		return nil, fmt.Errorf("not a big-endian 64-bit ELF")
	}
	phoff := be.Uint64(elf[0x20:])
	shoff := be.Uint64(elf[0x28:])
	phnum := int(be.Uint16(elf[0x38:]))
	shnum := int(be.Uint16(elf[0x3c:]))
	if phoff+uint64(phnum)*0x38 > uint64(len(elf)) || shoff+uint64(shnum)*0x40 > uint64(len(elf)) {
		return nil, fmt.Errorf("ELF headers out of range")
	}
	seed := sha1.Sum(elf)

	// the segments: deflated, hashed (HMAC-SHA1), encrypted (AES-CTR)
	segs := make([]*selfSegment, phnum)
	for i := 0; i < phnum; i++ {
		ph := elf[phoff+uint64(i)*0x38:]
		off, size := be.Uint64(ph[8:]), be.Uint64(ph[0x20:])
		if off+size > uint64(len(elf)) {
			return nil, fmt.Errorf("segment %d out of range", i)
		}
		s := &selfSegment{
			hmac: derived(fmt.Sprintf("hmac%d", i), seed[:], 0x40),
			erk:  derived(fmt.Sprintf("erk%d", i), seed[:], 0x10),
			riv:  derived(fmt.Sprintf("riv%d", i), seed[:], 0x10),
		}
		compressed := size != 0
		if compressed {
			var b bytes.Buffer
			w, _ := zlib.NewWriterLevel(&b, 9)
			w.Write(elf[off : off+size])
			w.Close()
			s.data = b.Bytes()
		} else {
			s.data = append([]byte(nil), elf[off:off+size]...)
		}
		s.pad = (16 - len(s.data)&15) & 15
		comp := uint32(1)
		if compressed {
			comp = 2
		}
		s.enc = make([]byte, 0x30)
		be.PutUint64(s.enc[8:], uint64(len(s.data)))
		be.PutUint32(s.enc[0x10:], 2) // type
		be.PutUint32(s.enc[0x14:], uint32(i))
		be.PutUint32(s.enc[0x18:], 2) // hashed
		be.PutUint32(s.enc[0x20:], 3) // encrypted
		be.PutUint32(s.enc[0x2c:], comp)
		s.pm = make([]byte, 0x20)
		be.PutUint64(s.pm[8:], uint64(len(s.data)))
		be.PutUint32(s.pm[0x10:], comp)
		be.PutUint32(s.pm[0x1c:], 1)
		mac := hmac.New(sha1.New, s.hmac)
		mac.Write(s.data)
		s.sha1 = make([]byte, 0x20)
		copy(s.sha1, mac.Sum(nil))
		ctrCrypt(s.erk, s.riv, s.data)
		segs[i] = s
	}

	// the crypt data: each segment's hash, HMAC key, key and IV
	var crypt []byte
	for _, s := range segs {
		be.PutUint32(s.enc[0x1c:], uint32(len(crypt)/16))
		crypt = append(crypt, s.sha1...)
		crypt = append(crypt, s.hmac...)
		be.PutUint32(s.enc[0x24:], uint32(len(crypt)/16))
		crypt = append(crypt, s.erk...)
		be.PutUint32(s.enc[0x28:], uint32(len(crypt)/16))
		crypt = append(crypt, s.riv...)
	}

	// the NPDRM block
	npd := make([]byte, 0x90)
	be.PutUint32(npd[0:], 3)
	be.PutUint32(npd[4:], 0x90)
	be.PutUint32(npd[0x10:], 0x4E504400)
	be.PutUint32(npd[0x14:], 1)
	be.PutUint32(npd[0x18:], 3)
	be.PutUint32(npd[0x1c:], 1)
	copy(npd[0x20:0x50], contentID)
	omacKey := make([]byte, 16)
	for i := range omacKey {
		omacKey[i] = npdrmOmacKey1[i] ^ npdrmOmacKey2[i]
	}
	name := path.Base(strings.ReplaceAll(filename, `\`, "/"))
	copy(npd[0x60:0x70], aesOmac1(npdrmOmacKey3, append(append([]byte(nil), npd[0x20:0x50]...), name...)))
	copy(npd[0x70:0x80], aesOmac1(omacKey, npd[0x10:0x70]))

	// the file, in make_self's order
	shdr := make([]byte, 0x20)
	be.PutUint32(shdr[0:], 0x53434500)
	be.PutUint32(shdr[4:], 2)
	be.PutUint16(shdr[8:], 1)
	be.PutUint16(shdr[0xa:], 1)
	be.PutUint64(shdr[0x18:], uint64(len(elf)))
	ehdr := make([]byte, 0x50)
	be.PutUint64(ehdr[0:], 3)
	be.PutUint64(ehdr[0x40:], 0x70+0x90)
	ihdr := make([]byte, 0x20)
	be.PutUint64(ihdr[0:], 0x1010000001000003)
	be.PutUint32(ihdr[8:], 0x01000002)
	be.PutUint32(ihdr[0xc:], 8)
	be.PutUint64(ihdr[0x10:], 0x0001000000000000)
	segHdr := make([]byte, 0x20)
	be.PutUint32(segHdr[8:], 1)
	be.PutUint32(segHdr[0xc:], uint32(len(segs)))
	be.PutUint32(segHdr[0x10:], uint32(len(crypt)/16))
	be.PutUint32(segHdr[0x14:], 0x30)
	sign := make([]byte, 0x30)
	copy(sign[1:], npdrmR)
	md := append([]byte(nil), npdrmKeypairD...)

	var out bytes.Buffer
	out.Write(shdr)
	ehdrAt := out.Len()
	out.Write(ehdr)
	be.PutUint64(ehdr[8:], uint64(out.Len())) // ihoff
	out.Write(ihdr)
	be.PutUint64(ehdr[0x10:], uint64(out.Len()))       // ehoff
	be.PutUint64(ehdr[0x18:], uint64(out.Len())+phoff) // phoff
	out.Write(elf[:phoff+uint64(phnum)*0x38])
	out.Write(make([]byte, (16-out.Len()&15)&15))
	be.PutUint64(ehdr[0x28:], uint64(out.Len())) // pmoff
	pmAt := out.Len()
	for _, s := range segs {
		out.Write(s.pm)
	}
	be.PutUint64(ehdr[0x30:], uint64(out.Len())) // svoff
	out.Write(sdkVersion)
	be.PutUint64(ehdr[0x38:], uint64(out.Len())) // cfoff
	out.Write(cflagsNPDRM)
	out.Write(npd)
	be.PutUint32(shdr[0xc:], uint32(out.Len()-0x20)) // esize
	mdAt := out.Len()
	out.Write(md)
	segHdrAt := out.Len()
	out.Write(segHdr)
	encAt := out.Len()
	for _, s := range segs {
		out.Write(s.enc)
	}
	out.Write(crypt)
	out.Write(nubPadding)
	sigAt := out.Len()
	out.Write(sign)
	if r := out.Len() % 0x80; r != 0 {
		out.Write(make([]byte, 0x80-r))
	}
	be.PutUint64(shdr[0x10:], uint64(out.Len())) // shsize
	for _, s := range segs {
		be.PutUint64(s.enc[0:], uint64(out.Len()))
		be.PutUint64(s.pm[0:], uint64(out.Len()))
		out.Write(s.data)
		out.Write(make([]byte, s.pad))
	}
	be.PutUint64(ehdr[0x20:], uint64(out.Len())) // shoff
	out.Write(elf[shoff : shoff+uint64(shnum)*0x40])
	be.PutUint64(segHdr[0:], uint64(sigAt))

	// patch the headers written before their offsets were known
	b := out.Bytes()
	copy(b[0:], shdr)
	copy(b[ehdrAt:], ehdr)
	for i, s := range segs {
		copy(b[pmAt+i*0x20:], s.pm)
		copy(b[encAt+i*0x30:], s.enc)
	}
	copy(b[segHdrAt:], segHdr)

	// sign: S = (z + r*da) / k mod n over everything up to the signature
	digest := sha1.Sum(b[:sigAt])
	n := new(big.Int).SetBytes(npdrmN)
	k := new(big.Int).SetBytes(npdrmK)
	r := new(big.Int).SetBytes(npdrmR)
	da := new(big.Int).SetBytes(npdrmDa)
	z := new(big.Int).SetBytes(digest[:])
	s := new(big.Int).Mul(r, da)
	s.Mod(s, n)
	s.Add(s, z)
	s.Mod(s, n)
	s.Mul(s, new(big.Int).ModInverse(k, n))
	s.Mod(s, n)
	sb := s.Bytes()
	sig := b[sigAt+0x16 : sigAt+0x16+0x14]
	for i := range sig {
		sig[i] = 0
	}
	copy(sig[0x14-len(sb):], sb)

	// encrypt the metadata after its key block, then put the encrypted key block in
	ctrCrypt(b[mdAt:mdAt+16], append([]byte(nil), b[mdAt+0x20:mdAt+0x30]...), b[mdAt+0x40:be.Uint64(shdr[0x10:])])
	copy(b[mdAt:], npdrmKeypairE)
	return b, nil
}
