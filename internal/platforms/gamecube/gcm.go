package gamecube

// The GameCube disc image (GCM, the .iso Dolphin, Swiss and a modded
// console's disc loaders read): the disc header (boot.bin), the second
// header (bi2.bin), the apploader the boot ROM runs, the program (a DOL)
// and the file system table (FST) with the files after it.
//
//	0x0000  boot.bin  game ID, title, magic; DOL, FST offsets and size
//	0x0440  bi2.bin   0x2000 bytes: memory size, region
//	0x2440  apploader 0x20-byte header and its code
//	        DOL, FST, files, each on a 32 KiB boundary
//
// Offsets in the FST are bytes (the Wii's are words).

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"path"
	"sort"
	"strings"
)

const (
	gcmMagic     = 0xC2339F3D
	gcmAlign     = 0x8000
	apploaderOff = 0x2440
)

// GCMFile is one file of the disc's file system: Path in it ("opening.bnr",
// "DATA/MESH00.BIN") and either its bytes or the host file they come from.
type GCMFile struct {
	Path string
	Data []byte
	Src  string
}

// GCM is what a disc image holds.
type GCM struct {
	GameID    string // six letters or digits: game code (4) and maker (2)
	Title     string // the disc header's name
	DOL       []byte
	Apploader []byte // its header and code (guest.Apploader)
	Files     []GCMFile
}

// RegionOf is the region bi2.bin gives for a game ID: its fourth letter
// (J Japan, E America, P/D/F/S/I/U Europe...).
func RegionOf(gameID string) uint32 {
	if len(gameID) < 4 {
		return 1
	}
	switch gameID[3] {
	case 'J':
		return 0
	case 'E':
		return 1
	case 'K', 'W':
		return 4
	default:
		return 2
	}
}

type fstNode struct {
	name  string
	dir   bool
	file  *GCMFile
	kids  []*fstNode
	index int
	off   uint32
	size  uint32
}

func (n *fstNode) child(name string, dir bool) *fstNode {
	for _, k := range n.kids {
		if strings.EqualFold(k.name, name) {
			return k
		}
	}
	k := &fstNode{name: name, dir: dir}
	n.kids = append(n.kids, k)
	return k
}

func sortTree(n *fstNode) {
	sort.Slice(n.kids, func(i, j int) bool { return strings.ToUpper(n.kids[i].name) < strings.ToUpper(n.kids[j].name) })
	for _, k := range n.kids {
		sortTree(k)
	}
}

// flatten lists the tree depth first, as the FST orders its entries.
func flatten(n *fstNode, out []*fstNode) []*fstNode {
	n.index = len(out)
	out = append(out, n)
	for _, k := range n.kids {
		out = flatten(k, out)
	}
	return out
}

func alignUp(v, a int64) int64 {
	return (v + a - 1) / a * a
}

func fileSize(f *GCMFile) (int64, error) {
	if f.Src == "" {
		return int64(len(f.Data)), nil
	}
	st, err := os.Stat(f.Src)
	if err != nil {
		return 0, err
	}
	return st.Size(), nil
}

// WriteGCM writes the disc image to w.
func WriteGCM(w io.Writer, g GCM) error {
	if len(g.GameID) != 6 {
		return fmt.Errorf("game ID %q is not six characters", g.GameID)
	}
	if len(g.Apploader) < 0x20 {
		return fmt.Errorf("no apploader")
	}
	if len(g.DOL) < 0x100 {
		return fmt.Errorf("no DOL")
	}

	// the file tree
	root := &fstNode{dir: true}
	for i := range g.Files {
		f := &g.Files[i]
		parts := strings.Split(path.Clean(strings.Trim(strings.ReplaceAll(f.Path, "\\", "/"), "/")), "/")
		n := root
		for _, d := range parts[:len(parts)-1] {
			n = n.child(d, true)
			if !n.dir {
				return fmt.Errorf("%s: %s is a file", f.Path, d)
			}
		}
		leaf := n.child(parts[len(parts)-1], false)
		if leaf.dir || leaf.file != nil {
			return fmt.Errorf("%s is on the disc twice", f.Path)
		}
		leaf.file = f
	}
	sortTree(root)
	nodes := flatten(root, nil)

	// the layout
	dolOff := alignUp(int64(apploaderOff+len(g.Apploader)), gcmAlign)
	var names bytes.Buffer
	nameOff := make([]uint32, len(nodes))
	for i, n := range nodes[1:] {
		nameOff[i+1] = uint32(names.Len())
		names.WriteString(n.name)
		names.WriteByte(0)
	}
	fstSize := int64(len(nodes)*12 + names.Len())
	fstOff := alignUp(dolOff+int64(len(g.DOL)), gcmAlign)
	pos := alignUp(fstOff+fstSize, gcmAlign)
	for _, n := range nodes {
		if n.dir {
			continue
		}
		sz, err := fileSize(n.file)
		if err != nil {
			return err
		}
		if sz > 0xffffffff || pos > 0xffffffff {
			return fmt.Errorf("%s does not fit a GameCube disc", n.file.Path)
		}
		n.off, n.size = uint32(pos), uint32(sz)
		pos = alignUp(pos+sz, gcmAlign)
	}
	if pos > 1459978240 {
		return fmt.Errorf("the game is %d MiB, more than a GameCube disc (1392 MiB)", pos>>20)
	}

	// the FST: the root, then each entry (12 bytes), then the names
	parentOf := map[*fstNode]int{}
	for _, n := range nodes {
		for _, k := range n.kids {
			parentOf[k] = n.index
		}
	}
	var fst bytes.Buffer
	be := func(v uint32) { _ = binary.Write(&fst, binary.BigEndian, v) }
	end := func(n *fstNode) int { // the index after a directory's contents
		for len(n.kids) > 0 {
			n = n.kids[len(n.kids)-1]
		}
		return n.index + 1
	}
	for i, n := range nodes {
		if n.dir {
			be(1<<24 | nameOff[i])
			be(uint32(parentOf[n]))
			be(uint32(end(n)))
		} else {
			be(nameOff[i])
			be(n.off)
			be(n.size)
		}
	}
	fst.Write(names.Bytes())

	// boot.bin and bi2.bin
	hdr := make([]byte, apploaderOff)
	copy(hdr, g.GameID)
	binary.BigEndian.PutUint32(hdr[0x1c:], gcmMagic)
	title := g.Title
	if len(title) > 0x3df {
		title = title[:0x3df]
	}
	copy(hdr[0x20:], title)
	binary.BigEndian.PutUint32(hdr[0x420:], uint32(dolOff))
	binary.BigEndian.PutUint32(hdr[0x424:], uint32(fstOff))
	binary.BigEndian.PutUint32(hdr[0x428:], uint32(fstSize))
	binary.BigEndian.PutUint32(hdr[0x42c:], uint32(fstSize))
	bi2 := hdr[0x440:]
	binary.BigEndian.PutUint32(bi2[0x04:], 0x01800000) // 24 MiB
	binary.BigEndian.PutUint32(bi2[0x18:], RegionOf(g.GameID))

	cw := &countWriter{w: w}
	put := func(at int64, b []byte) error {
		if at < cw.n {
			return fmt.Errorf("disc layout overlaps at %#x", at)
		}
		if err := cw.zeros(at - cw.n); err != nil {
			return err
		}
		_, err := cw.Write(b)
		return err
	}
	if err := put(0, hdr); err != nil {
		return err
	}
	if err := put(apploaderOff, g.Apploader); err != nil {
		return err
	}
	if err := put(dolOff, g.DOL); err != nil {
		return err
	}
	if err := put(fstOff, fst.Bytes()); err != nil {
		return err
	}
	for _, n := range nodes {
		if n.dir {
			continue
		}
		data := n.file.Data
		if n.file.Src != "" {
			raw, err := os.ReadFile(n.file.Src)
			if err != nil {
				return err
			}
			data = raw
		}
		if int64(len(data)) != int64(n.size) {
			return fmt.Errorf("%s changed while the disc was written", n.file.Path)
		}
		if err := put(int64(n.off), data); err != nil {
			return err
		}
	}
	return cw.zeros(pos - cw.n)
}

type countWriter struct {
	w io.Writer
	n int64
}

func (c *countWriter) Write(b []byte) (int, error) {
	k, err := c.w.Write(b)
	c.n += int64(k)
	return k, err
}

var zeroBlock [gcmAlign]byte

func (c *countWriter) zeros(n int64) error {
	for n > 0 {
		k := n
		if k > gcmAlign {
			k = gcmAlign
		}
		if _, err := c.Write(zeroBlock[:k]); err != nil {
			return err
		}
		n -= k
	}
	return nil
}
