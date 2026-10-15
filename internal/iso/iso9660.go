package iso

import (
	"encoding/binary"
	"fmt"
	"io"
	"strings"
	"time"
	"unicode/utf16"
)

const (
	isoPVDSector  = 16
	isoSVDSector  = 17
	isoTermSector = 18
	isoMetaStart  = 22
	isoMetaLimit  = 256
)

type isoDir struct {
	node   *node
	rel    string
	sector uint32
	size   uint32
	parent int
	ident  string
	joliet []byte
}

func writeISO9660(out io.WriterAt, root *node, props VolumeProps, totalSectors uint32) error {
	now := props.Created
	if now.IsZero() {
		now = time.Now().UTC()
	}
	dirs := flattenDirs(root)
	for i := range dirs {
		if dirs[i].rel == "" {
			dirs[i].ident = "\x00"
			dirs[i].joliet = utf16BE("")
			continue
		}
		base := dirs[i].node.name
		ident := dirs[i].node.isoName
		if ident == "" {
			ident = mangle83(base, true, nil)
		}
		dirs[i].ident = ident
		dirs[i].joliet = utf16BE(base)
	}

	isoSizes := make([]uint32, len(dirs))
	jolietSizes := make([]uint32, len(dirs))
	for i, d := range dirs {
		isoSizes[i] = dirRecordBytes(d, dirs, false)
		jolietSizes[i] = dirRecordBytes(d, dirs, true)
	}
	isoPT := pathTableBytes(dirs, false)
	jolietPT := pathTableBytes(dirs, true)

	sec := uint32(isoMetaStart)
	isoPTL, isoPTM := sec, sec+1
	sec += 2
	if blocks32(uint64(len(isoPT))) > 1 {
		isoPTM = isoPTL + blocks32(uint64(len(isoPT)))
		sec = isoPTM + blocks32(uint64(len(isoPT)))
	}
	jolietPTL, jolietPTM := sec, sec+1
	sec += 2
	if blocks32(uint64(len(jolietPT))) > 1 {
		jolietPTM = jolietPTL + blocks32(uint64(len(jolietPT)))
		sec = jolietPTM + blocks32(uint64(len(jolietPT)))
	}
	for i := range dirs {
		dirs[i].sector = sec
		dirs[i].size = isoSizes[i]
		sec += blocks32(uint64(isoSizes[i]))
	}
	jolietSectors := make([]uint32, len(dirs))
	for i := range dirs {
		jolietSectors[i] = sec
		sec += blocks32(uint64(jolietSizes[i]))
	}
	if sec > isoMetaLimit {
		return fmt.Errorf("iso: ISO9660 metadata needs %d sectors; limit is %d", sec-isoMetaStart, isoMetaLimit-isoMetaStart)
	}

	writeAt := func(sector uint64, data []byte) error {
		_, err := out.WriteAt(data, int64(sector*SectorSize))
		return err
	}
	pvd := primaryVolume(props, totalSectors, now, uint32(len(isoPT)), isoPTL, isoPTM, dirs[0])
	if err := writeAt(isoPVDSector, pvd); err != nil {
		return err
	}
	svd := jolietVolume(props, totalSectors, now, uint32(len(jolietPT)), jolietPTL, jolietPTM, jolietSectors[0], jolietSizes[0])
	if err := writeAt(isoSVDSector, svd); err != nil {
		return err
	}
	if err := writeAt(isoTermSector, terminatorDescriptor()); err != nil {
		return err
	}
	if err := writePathTables(out, isoPTL, isoPTM, dirs, false, nil); err != nil {
		return err
	}
	if err := writePathTables(out, jolietPTL, jolietPTM, dirs, true, jolietSectors); err != nil {
		return err
	}
	for i, d := range dirs {
		data := buildDirExtent(d, dirs, false, nil, now)
		if err := writePadded(out, uint64(d.sector), data); err != nil {
			return err
		}
		jdata := buildDirExtent(d, dirs, true, jolietSectors, now)
		if err := writePadded(out, uint64(jolietSectors[i]), jdata); err != nil {
			return err
		}
	}
	return nil
}

func flattenDirs(root *node) []isoDir {
	var out []isoDir
	var walk func(n *node, rel string, parent int)
	walk = func(n *node, rel string, parent int) {
		idx := len(out)
		out = append(out, isoDir{node: n, rel: rel, parent: parent})
		for _, c := range n.children {
			if c.isDir {
				childRel := c.name
				if rel != "" {
					childRel = rel + "/" + c.name
				}
				walk(c, childRel, idx)
			}
		}
	}
	walk(root, "", 0)
	return out
}

func dirRecordBytes(d isoDir, dirs []isoDir, joliet bool) uint32 {
	// . and .. (34 bytes each) always fit in the first sector.
	n := uint32(34 + 34)
	for _, c := range d.node.children {
		l := recLen(len(childIdent(c, dirs, joliet)))
		// Records never cross a logical sector (ECMA-119 6.8.1.1); the
		// writer pads to the next sector, so the size must count that pad.
		if room := SectorSize - n%SectorSize; l > room {
			n += room
		}
		n += l
	}
	if n < SectorSize {
		return SectorSize
	}
	return uint32(blocks32(uint64(n))) * SectorSize
}

// childIdent is the identifier buildDirExtent writes for c. dirRecordBytes
// sizes the extent from the same bytes so the declared size cannot drift
// (Joliet file names are UTF-16BE "name;1", 2*(len+2) bytes).
func childIdent(c *node, dirs []isoDir, joliet bool) []byte {
	if c.isDir {
		for _, dd := range dirs {
			if dd.node == c {
				if joliet {
					return dd.joliet
				}
				return []byte(dd.ident)
			}
		}
		return nil
	}
	if joliet {
		return utf16BE(c.name + ";1")
	}
	id := c.isoName
	if id == "" {
		id = mangle83(c.name, false, nil)
	}
	return []byte(id + ";1")
}

// appendDirRecord appends rec, zero-padding to the next sector first when
// rec would otherwise cross a logical sector boundary (ECMA-119 6.8.1.1).
func appendDirRecord(b, rec []byte) []byte {
	if room := SectorSize - len(b)%SectorSize; len(rec) > room {
		b = append(b, make([]byte, room)...)
	}
	return append(b, rec...)
}

func recLen(identLen int) uint32 {
	l := 33 + identLen
	if l%2 == 1 {
		l++
	}
	return uint32(l)
}

func pathTableBytes(dirs []isoDir, joliet bool) []byte {
	var b []byte
	for i, d := range dirs {
		ident := []byte(d.ident)
		if joliet {
			ident = d.joliet
		}
		if i == 0 {
			ident = []byte{0}
			if joliet {
				ident = []byte{0, 0}
			}
		}
		rec := make([]byte, 8+len(ident))
		rec[0] = byte(len(ident))
		binary.LittleEndian.PutUint32(rec[2:], 0) // filled later
		binary.LittleEndian.PutUint16(rec[6:], uint16(d.parent+1))
		copy(rec[8:], ident)
		if len(rec)%2 == 1 {
			rec = append(rec, 0)
		}
		b = append(b, rec...)
	}
	return b
}

func writePathTables(out io.WriterAt, lsec, msec uint32, dirs []isoDir, joliet bool, jsec []uint32) error {
	write := func(sector uint32, big bool) error {
		var buf []byte
		for i, d := range dirs {
			ident := []byte(d.ident)
			if joliet {
				ident = d.joliet
			}
			if i == 0 {
				ident = []byte{0}
				if joliet {
					ident = []byte{0, 0}
				}
			}
			ext := d.sector
			if joliet {
				ext = jsec[i]
			}
			rec := make([]byte, 8+len(ident))
			rec[0] = byte(len(ident))
			if big {
				binary.BigEndian.PutUint32(rec[2:], ext)
				binary.BigEndian.PutUint16(rec[6:], uint16(d.parent+1))
			} else {
				binary.LittleEndian.PutUint32(rec[2:], ext)
				binary.LittleEndian.PutUint16(rec[6:], uint16(d.parent+1))
			}
			copy(rec[8:], ident)
			if len(rec)%2 == 1 {
				rec = append(rec, 0)
			}
			buf = append(buf, rec...)
		}
		return writePadded(out, uint64(sector), buf)
	}
	if err := write(lsec, false); err != nil {
		return err
	}
	return write(msec, true)
}

func buildDirExtent(d isoDir, dirs []isoDir, joliet bool, jsec []uint32, now time.Time) []byte {
	selfSec := d.sector
	selfSize := d.size
	if joliet {
		for i := range dirs {
			if dirs[i].rel == d.rel {
				selfSec = jsec[i]
				selfSize = dirRecordBytes(d, dirs, true)
				break
			}
		}
	}
	parent := dirs[d.parent]
	parentSec := parent.sector
	parentSize := parent.size
	if joliet {
		parentSec = jsec[d.parent]
		parentSize = dirRecordBytes(parent, dirs, true)
	}
	var b []byte
	b = append(b, dirRecord(selfSec, selfSize, now, true, []byte{0})...)
	b = append(b, dirRecord(parentSec, parentSize, now, true, []byte{1})...)
	for _, c := range d.node.children {
		sec := uint32(c.absSector)
		sz := uint32(c.size)
		if c.isDir {
			for i, dd := range dirs {
				if dd.node == c {
					sec = dd.sector
					sz = dd.size
					if joliet {
						sec = jsec[i]
						sz = dirRecordBytes(dd, dirs, true)
					}
					break
				}
			}
		}
		b = appendDirRecord(b, dirRecord(sec, sz, c.modTime, c.isDir, childIdent(c, dirs, joliet)))
	}
	return b
}

func dirRecord(sector, size uint32, t time.Time, isDir bool, ident []byte) []byte {
	l := 33 + len(ident)
	if l%2 == 1 {
		l++
	}
	r := make([]byte, l)
	r[0] = byte(l)
	copy(r[2:10], both32(sector))
	copy(r[10:18], both32(size))
	copy(r[18:25], isoDate7(t))
	if isDir {
		r[25] = 0x02
	}
	copy(r[28:32], both16(1))
	r[32] = byte(len(ident))
	copy(r[33:], ident)
	return r
}

func primaryVolume(props VolumeProps, total uint32, now time.Time, ptSize, ptL, ptM uint32, root isoDir) []byte {
	b := make([]byte, SectorSize)
	b[0] = 1
	copy(b[1:6], "CD001")
	b[6] = 1
	copy(b[8:40], padUpper(props.System, 32))
	copy(b[40:72], padUpper(props.VolumeID, 32))
	copy(b[80:88], both32(total))
	copy(b[120:124], both16(uint16(props.Discs)))
	copy(b[124:128], both16(uint16(props.Disc)))
	copy(b[128:132], both16(SectorSize))
	copy(b[132:140], both32(ptSize))
	binary.LittleEndian.PutUint32(b[140:], ptL)
	binary.BigEndian.PutUint32(b[148:], ptM)
	copy(b[156:190], dirRecord(root.sector, root.size, now, true, []byte{0})[:34])
	copy(b[190:318], padSpace(props.Title, 128))
	copy(b[318:446], padSpace(props.Publisher, 128))
	copy(b[446:574], padSpace(props.Preparer, 128))
	copy(b[574:702], padSpace(props.Application, 128))
	copy(b[702:739], padUpper(isoFileID(props.CopyrightFile), 37))
	copy(b[739:776], padUpper(isoFileID(props.AbstractFile), 37))
	copy(b[776:813], padUpper(isoFileID(props.BibliographicFile), 37))
	copy(b[813:830], isoDate17(now))
	copy(b[830:847], isoDate17(now))
	copy(b[847:864], isoDate17Empty())
	copy(b[864:881], isoDate17Empty())
	b[881] = 1
	return b
}

func jolietVolume(props VolumeProps, total uint32, now time.Time, ptSize, ptL, ptM, rootSec, rootSize uint32) []byte {
	b := make([]byte, SectorSize)
	b[0] = 2
	copy(b[1:6], "CD001")
	b[6] = 1
	copy(b[8:40], ucs2pad(props.System, 32))
	copy(b[40:72], ucs2pad(props.VolumeID, 32))
	copy(b[80:88], both32(total))
	b[88], b[89], b[90] = 0x25, 0x2F, 0x45
	copy(b[120:124], both16(uint16(props.Discs)))
	copy(b[124:128], both16(uint16(props.Disc)))
	copy(b[128:132], both16(SectorSize))
	copy(b[132:140], both32(ptSize))
	binary.LittleEndian.PutUint32(b[140:], ptL)
	binary.BigEndian.PutUint32(b[148:], ptM)
	copy(b[156:190], dirRecord(rootSec, rootSize, now, true, []byte{0})[:34])
	copy(b[190:318], ucs2pad(props.Title, 128))
	copy(b[318:446], ucs2pad(props.Publisher, 128))
	copy(b[446:574], ucs2pad(props.Preparer, 128))
	copy(b[574:702], ucs2pad(props.Application, 128))
	copy(b[813:830], isoDate17(now))
	copy(b[830:847], isoDate17(now))
	copy(b[847:864], isoDate17Empty())
	copy(b[864:881], isoDate17Empty())
	b[881] = 1
	return b
}

func terminatorDescriptor() []byte {
	b := make([]byte, SectorSize)
	b[0] = 255
	copy(b[1:6], "CD001")
	b[6] = 1
	return b
}

func isoFileID(name string) string {
	if name == "" {
		return ""
	}
	return strings.ToUpper(name)
}

func both16(v uint16) []byte {
	b := make([]byte, 4)
	binary.LittleEndian.PutUint16(b[0:], v)
	binary.BigEndian.PutUint16(b[2:], v)
	return b
}

func both32(v uint32) []byte {
	b := make([]byte, 8)
	binary.LittleEndian.PutUint32(b[0:], v)
	binary.BigEndian.PutUint32(b[4:], v)
	return b
}

func isoDate7(t time.Time) []byte {
	t = t.UTC()
	return []byte{byte(t.Year() - 1900), byte(t.Month()), byte(t.Day()),
		byte(t.Hour()), byte(t.Minute()), byte(t.Second()), 0}
}

func isoDate17(t time.Time) []byte {
	t = t.UTC()
	s := t.Format("20060102150405") + "00"
	b := make([]byte, 17)
	copy(b, s)
	return b
}

func isoDate17Empty() []byte {
	b := make([]byte, 17)
	for i := 0; i < 16; i++ {
		b[i] = ' '
	}
	return b
}

func utf16BE(s string) []byte {
	u := utf16.Encode([]rune(s))
	out := make([]byte, 2*len(u))
	for i, c := range u {
		binary.BigEndian.PutUint16(out[2*i:], c)
	}
	return out
}

func ucs2pad(s string, n int) []byte {
	b := make([]byte, n)
	for i := 0; i < n; i += 2 {
		b[i+1] = ' '
	}
	u := utf16BE(s)
	if len(u) > n {
		u = u[:n]
	}
	copy(b, u)
	return b
}

func mangle83(name string, isDir bool, used map[string]int) string {
	name = strings.ToUpper(name)
	base, ext := name, ""
	if i := strings.LastIndex(name, "."); i >= 0 && !isDir {
		base, ext = name[:i], name[i+1:]
	}
	clean := func(s string, n int) string {
		var b strings.Builder
		for _, r := range s {
			if (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9') || r == '_' {
				b.WriteRune(r)
			}
			if b.Len() >= n {
				break
			}
		}
		if b.Len() == 0 {
			return "FILE"
		}
		return b.String()
	}
	base, ext = clean(base, 8), clean(ext, 3)
	out := base
	if !isDir && ext != "" {
		out = base + "." + ext
	}
	if used != nil {
		if used[out] > 0 {
			used[out]++
			suf := fmt.Sprintf("~%d", used[out]-1)
			keep := 8 - len(suf)
			if keep < 1 {
				keep = 1
			}
			if len(base) > keep {
				base = base[:keep]
			}
			out = base + suf
			if !isDir && ext != "" {
				out += "." + ext
			}
		} else {
			used[out] = 1
		}
	}
	return out
}

func blocks32(n uint64) uint32 {
	return uint32((n + SectorSize - 1) / SectorSize)
}

func writePadded(out io.WriterAt, sector uint64, data []byte) error {
	n := int(blocks32(uint64(len(data)))) * SectorSize
	buf := make([]byte, n)
	copy(buf, data)
	_, err := out.WriteAt(buf, int64(sector*SectorSize))
	return err
}

func readSector(r io.ReaderAt, sector uint64) ([]byte, error) {
	b := make([]byte, SectorSize)
	_, err := r.ReadAt(b, int64(sector*SectorSize))
	return b, err
}
