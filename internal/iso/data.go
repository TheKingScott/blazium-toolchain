package iso

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/blazium-games/blazium-toolchain/internal/report"
	"github.com/blazium-games/blazium-toolchain/internal/settings"
)

// WriteDataISO writes a simple ISO9660 data disc (no UDF, no VIDEO_TS) from srcDir.
// Used for PS2 BOOT2 images. Files keep 8.3-mangled names.
func WriteDataISO(srcDir, outPath, volumeID string) error {
	return WriteDataISOSubdir(srcDir, outPath, volumeID, nil, "", nil)
}

// WriteDataISOSubdir is WriteDataISO with a short root: entries for which
// keepRoot returns false are moved under subdir. The PS2 boot ROM's file
// search only reads the first sector of a directory, so a root holding more
// than ~40 records can hide SYSTEM.CNF and the disc falls through to the
// browser; keeping just SYSTEM.CNF and the ELF in the root avoids that.
// skip, when set, leaves top-level entries off the disc (build notes,
// sources, empty sidecars).
func WriteDataISOSubdir(srcDir, outPath, volumeID string, keepRoot func(name string) bool, subdir string,
	skip func(name string, size int64, isDir bool) bool) error {
	if srcDir == "" || outPath == "" {
		return report.Usage("iso requires --dir and --out")
	}
	st, err := os.Stat(srcDir)
	if err != nil {
		return report.Fail("iso: stat "+srcDir+": "+err.Error(), "pass --dir to an existing folder")
	}
	if !st.IsDir() {
		return report.Fail(srcDir+" is not a directory", "pass --dir to a folder")
	}
	now := time.Now().UTC()
	root := &node{name: "", isDir: true, modTime: st.ModTime()}
	if err := addDirChildren(root, srcDir, true); err != nil {
		return err
	}
	if len(root.children) == 0 {
		return report.Fail(srcDir+" has no files", "put GAME.ELF / SYSTEM.CNF in the tree")
	}
	if skip != nil {
		var left []*node
		for _, c := range root.children {
			if !skip(c.name, c.size, c.isDir) {
				left = append(left, c)
			}
		}
		root.children = left
	}
	if keepRoot != nil && subdir != "" {
		var kept, moved []*node
		for _, c := range root.children {
			if keepRoot(c.name) {
				kept = append(kept, c)
			} else {
				moved = append(moved, c)
			}
		}
		if len(moved) > 0 {
			root.children = kept
			d := root.mkdir(subdir, st.ModTime())
			d.children = append(d.children, moved...)
			// ISO9660 wants directory records in name order
			sort.SliceStable(root.children, func(i, j int) bool {
				return strings.ToUpper(root.children[i].name) < strings.ToUpper(root.children[j].name)
			})
		}
	}
	assignISONames(root)

	volumeID = VolumeOr(volumeID, settings.Current().PS2.ISOVolume)
	s := settings.Current().PS2
	props := VolumeProps{
		VolumeID:    volumeID,
		System:      s.ISOSystem,
		Application: s.ISOApp,
		Created:     now,
		Disc:        1,
		Discs:       1,
	}

	if err := os.MkdirAll(filepath.Dir(outPath), 0o755); err != nil {
		return err
	}
	f, err := os.Create(outPath)
	if err != nil {
		return report.Fail("iso: create "+outPath+": "+err.Error(), "")
	}
	defer f.Close()

	total, err := layoutDataISO(root)
	if err != nil {
		return err
	}
	if err := writeISO9660(f, root, props, total); err != nil {
		return err
	}
	if err := writeDataFileExtents(f, root); err != nil {
		return err
	}
	end := int64(total) * int64(SectorSize)
	if err := f.Truncate(end); err != nil {
		return err
	}
	return nil
}

func layoutDataISO(root *node) (uint32, error) {
	// path tables (L+M ISO + L+M Joliet) — writeISO9660 allocates from isoMetaStart
	// File data must start after the metadata window so PVD directory records
	// can point at extents. Place files after isoMetaLimit.
	next := uint32(isoMetaLimit)
	var walk func(n *node)
	walk = func(n *node) {
		for _, c := range n.children {
			if c.isDir {
				walk(c)
				continue
			}
			c.absSector = uint64(next)
			c.dataLen = uint32(c.size)
			next += blocks32(uint64(c.size))
		}
	}
	walk(root)
	if next < isoMetaLimit+1 {
		next = isoMetaLimit + 1
	}
	return next, nil
}

func writeDataFileExtents(out io.WriterAt, root *node) error {
	var walk func(n *node) error
	walk = func(n *node) error {
		for _, c := range n.children {
			if c.isDir {
				if err := walk(c); err != nil {
					return err
				}
				continue
			}
			var data []byte
			var err error
			if c.data != nil {
				data = c.data
			} else if c.srcPath != "" {
				data, err = os.ReadFile(c.srcPath)
				if err != nil {
					return err
				}
			}
			if c.absSector == 0 {
				return fmt.Errorf("iso: file %s has no sector", c.name)
			}
			if _, err := out.WriteAt(data, int64(c.absSector)*int64(SectorSize)); err != nil {
				return err
			}
		}
		return nil
	}
	return walk(root)
}

// HasISO9660PVD reports whether path looks like an ISO9660 image (CD001 at sector 16).
func HasISO9660PVD(path string) bool {
	f, err := os.Open(path)
	if err != nil {
		return false
	}
	defer f.Close()
	buf := make([]byte, 8)
	if _, err := f.ReadAt(buf, int64(isoPVDSector)*int64(SectorSize)+1); err != nil {
		return false
	}
	return string(buf[:5]) == "CD001"
}

// LargestDirRecords walks an ISO9660 image's directories and returns the
// most sectors any one directory's records take (0 if the image can't be read).
func LargestDirRecords(path string) int {
	b, err := os.ReadFile(path)
	if err != nil || len(b) < (isoPVDSector+1)*SectorSize {
		return 0
	}
	pvd := b[isoPVDSector*SectorSize:]
	le32 := func(p []byte) uint32 { return uint32(p[0]) | uint32(p[1])<<8 | uint32(p[2])<<16 | uint32(p[3])<<24 }
	most := 0
	seen := map[uint32]bool{}
	var walk func(ext, size uint32)
	walk = func(ext, size uint32) {
		if seen[ext] || int64(ext)*SectorSize+int64(size) > int64(len(b)) {
			return
		}
		seen[ext] = true
		n := int((size + SectorSize - 1) / SectorSize)
		if n > most {
			most = n
		}
		off := int(ext) * SectorSize
		end := off + int(size)
		for off < end {
			l := int(b[off])
			if l == 0 {
				off = (off/SectorSize + 1) * SectorSize
				continue
			}
			rec := b[off : off+l]
			nl := int(rec[32])
			name := rec[33 : 33+nl]
			if rec[25]&2 != 0 && !(nl == 1 && (name[0] == 0 || name[0] == 1)) {
				walk(le32(rec[2:]), le32(rec[10:]))
			}
			off += l
		}
	}
	walk(le32(pvd[156+2:]), le32(pvd[156+10:]))
	return most
}
