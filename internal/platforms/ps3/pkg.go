package ps3

// PARAM.SFO and the .pkg, as PSL1GHT's sfo and pkg tools write them
// (tools/sfo_pkg), so a game packs on any host.

import (
	"bytes"
	"crypto/sha1"
	"encoding/binary"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// sfoEntry: a PARAM.SFO value, a string or an integer.
type sfoEntry struct {
	key string
	str string
	num uint32
	isN bool
}

// paramSFO is the hard drive game's PARAM.SFO (PSL1GHT's sfo.xml with the
// title and title ID), keys in order.
func paramSFO(title, titleID, appVer string) []byte {
	es := []sfoEntry{
		{key: "APP_VER", str: appVer},
		{key: "ATTRIBUTE", isN: true},
		{key: "BOOTABLE", num: 1, isN: true},
		{key: "CATEGORY", str: "HG"},
		{key: "LICENSE", str: "Made with Blazium. Built with PSL1GHT; not made, endorsed or supported by Sony Interactive Entertainment."},
		{key: "PARENTAL_LEVEL", isN: true},
		{key: "PS3_SYSTEM_VER", str: "01.8000"},
		{key: "RESOLUTION", num: 63, isN: true},
		{key: "SOUND_FORMAT", num: 279, isN: true},
		{key: "TITLE", str: title},
		{key: "TITLE_ID", str: titleID},
		{key: "VERSION", str: appVer},
	}
	le := binary.LittleEndian
	align := func(n, a uint32) uint32 { return (n + a - 1) / a * a }
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
		a := uint32(4)
		switch e.key {
		case "TITLE":
			a = 0x80
		case "LICENSE":
			a = 0x200
		case "TITLE_ID":
			a = 0x10
		}
		n := uint32(len(e.str)) + 1
		p := align(n, a)
		le.PutUint32(ent[4:], n)
		le.PutUint32(ent[8:], p)
		vals.WriteString(e.str)
		vals.Write(make([]byte, p-n+1))
	}
	keyOff := uint32(0x14 + len(idx))
	valOff := align(keyOff+uint32(keys.Len()), 4)
	hdr := make([]byte, 0x14)
	le.PutUint32(hdr[0:], 0x46535000) // "\0PSF"
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

// the pkg's stream cipher: SHA-1 of a 64-byte context with a counter
func pkgContext(key []byte) []byte {
	c := make([]byte, 64)
	copy(c[0:], key[0:8])
	copy(c[8:], key[0:8])
	copy(c[16:], key[8:16])
	copy(c[24:], key[8:16])
	return c
}

func pkgCrypt(ctx, data []byte) []byte {
	out := make([]byte, len(data))
	for off := 0; off < len(data); off += 16 {
		h := sha1.Sum(ctx)
		for i := 0; i < 16 && off+i < len(data); i++ {
			out[off+i] = data[off+i] ^ h[i]
		}
		binary.BigEndian.PutUint64(ctx[0x38:], binary.BigEndian.Uint64(ctx[0x38:])+1)
	}
	return out
}

type pkgItem struct {
	name   string
	src    string
	size   uint64
	flags  uint32
	nameAt uint32
	dataAt uint64
}

// pkgItems: the folder's files then its folders, each sorted, files before
// their folder's sub-folders (pkg.c's walk).
func pkgItems(root, dir string, items *[]pkgItem) error {
	ents, err := os.ReadDir(dir)
	if err != nil {
		return err
	}
	var files, dirs []string
	for _, e := range ents {
		if strings.HasPrefix(e.Name(), ".") {
			continue
		}
		if e.IsDir() {
			dirs = append(dirs, e.Name())
		} else {
			files = append(files, e.Name())
		}
	}
	sort.Strings(files)
	sort.Strings(dirs)
	rel := func(name string) string {
		r, _ := filepath.Rel(root, filepath.Join(dir, name))
		return filepath.ToSlash(r)
	}
	for _, f := range files {
		st, err := os.Stat(filepath.Join(dir, f))
		if err != nil {
			return err
		}
		it := pkgItem{name: rel(f), src: filepath.Join(dir, f), size: uint64(st.Size()), flags: 0x80000003}
		if it.name == "USRDIR/EBOOT.BIN" {
			it.size = ((it.size-0x30+63)&^63 + 0x30)
			it.flags = 0x80000001
		}
		*items = append(*items, it)
	}
	for _, d := range dirs {
		*items = append(*items, pkgItem{name: rel(d), flags: 0x80000004})
		if err := pkgItems(root, filepath.Join(dir, d), items); err != nil {
			return err
		}
	}
	return nil
}

// makePkg packs folder (PARAM.SFO, ICON0.PNG, USRDIR/...) as a debug
// package for contentID, as PSL1GHT's pkg does.
func makePkg(folder, contentID string) ([]byte, error) {
	var items []pkgItem
	if err := pkgItems(folder, folder, &items); err != nil {
		return nil, err
	}
	b := binary.BigEndian
	cur := uint64(0x20 * len(items))
	for i := range items {
		items[i].nameAt = uint32(cur)
		cur += uint64((len(items[i].name) + 15) &^ 15)
	}
	for i := range items {
		items[i].dataAt = cur
		if items[i].flags&0xff != 4 {
			cur += (items[i].size + 15) &^ 15
		}
	}
	var data bytes.Buffer
	for _, it := range items {
		h := make([]byte, 0x20)
		b.PutUint32(h[0:], it.nameAt)
		b.PutUint32(h[4:], uint32(len(it.name)))
		b.PutUint64(h[8:], it.dataAt)
		b.PutUint64(h[16:], it.size)
		b.PutUint32(h[24:], it.flags)
		data.Write(h)
	}
	for _, it := range items {
		data.WriteString(it.name)
		data.Write(make([]byte, ((len(it.name)+15)&^15)-len(it.name)))
	}
	descLen := data.Len()
	qa := sha1.New()
	for _, it := range items {
		if it.flags&0xff == 4 {
			continue
		}
		raw, err := os.ReadFile(it.src)
		if err != nil {
			return nil, err
		}
		qa.Write(raw)
		data.Write(raw)
		if want := (it.size + 15) &^ 15; uint64(len(raw)) < want {
			data.Write(make([]byte, want-uint64(len(raw))))
		}
	}
	body := data.Bytes()

	hdr := make([]byte, 0x80)
	b.PutUint32(hdr[0:], 0x7F504B47)
	b.PutUint32(hdr[4:], 1)
	b.PutUint32(hdr[8:], 0xC0)
	b.PutUint32(hdr[12:], 5)
	b.PutUint32(hdr[16:], 0x80)
	b.PutUint32(hdr[20:], uint32(len(items)))
	b.PutUint64(hdr[24:], uint64(len(body))+0x1A0)
	b.PutUint64(hdr[32:], 0x140)
	b.PutUint64(hdr[40:], uint64(len(body)))
	copy(hdr[48:0x60], contentID)
	// pkg.py's QA digest: the files, then a zeroed content ID's worth of the header
	qa.Write(hdr[:0x30])
	qa.Write(hdr[0x60:0x80])
	qa.Write(hdr[0x60:0x80])
	qa.Write(hdr[0x60:0x70])
	qa.Write(body[:descLen])
	copy(hdr[0x60:0x70], qa.Sum(nil))
	kctx := pkgContext(hdr[0x60:0x70])
	b.PutUint64(kctx[0x38:], ^uint64(0))
	copy(hdr[0x70:0x80], pkgCrypt(kctx, make([]byte, 16)))

	meta := make([]byte, 0x40)
	for i, v := range []uint32{1, 4, 3, 2, 4, 5, 3, 4, 0x0e, 4, 8} {
		b.PutUint32(meta[i*4:], v)
	}
	b.PutUint32(meta[48:], uint32(len(body)))
	b.PutUint32(meta[52:], 5)
	b.PutUint32(meta[56:], 4)
	b.PutUint16(meta[60:], 0x1061)

	var out bytes.Buffer
	out.Write(hdr)
	hs := sha1.Sum(hdr)
	out.Write(hs[3:19])
	ms := sha1.Sum(meta)
	enc1 := pkgCrypt(pkgContext(ms[3:19]), make([]byte, 0x30))
	enc2 := pkgCrypt(pkgContext(hs[3:19]), enc1)
	out.Write(enc2)
	out.Write(meta)
	out.Write(ms[3:19])
	out.Write(enc1)
	out.Write(pkgCrypt(pkgContext(hdr[0x60:0x70]), body))
	out.Write(make([]byte, 0x60))
	return out.Bytes(), nil
}
