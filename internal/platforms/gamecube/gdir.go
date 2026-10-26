package gamecube

// gamecube build --sample gdir: the GDIR guest (guest/gamecube/gdir/runtime)
// with a PS2 GDIR export in its game folder. The export (--pack-dir, or the
// folder --out is in) is what the PS2 exporter writes; the GameCube reads
// its packs as they are (the guest's GX renderer takes the PS2's MESH and
// GTEX), so the cooked files go, under upper-case names, into the disc's
// DATA folder.

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/blazium-games/blazium-toolchain/internal/report"
)

// gdirDataName: the export's cooked files (.bin), not the builds, the PS2
// disc's boot logo or the notes beside them.
func gdirDataName(name string) bool {
	return strings.EqualFold(filepath.Ext(name), ".bin")
}

// gdirFiles lists the export's cooked files as the disc's DATA/ files.
func gdirFiles(export string, log io.Writer) ([]GCMFile, error) {
	ents, err := os.ReadDir(export)
	if err != nil {
		return nil, report.Missing("no GDIR export at "+export, "export the game with the PlayStation 2 preset, then gamecube build --sample gdir --pack-dir DIR")
	}
	var files []GCMFile
	var bytes int64
	mesh := false
	for _, e := range ents {
		if e.IsDir() || !gdirDataName(e.Name()) {
			continue
		}
		info, err := e.Info()
		if err != nil {
			return nil, err
		}
		name := strings.ToUpper(e.Name())
		if name == "MESH00.BIN" {
			mesh = true
		}
		files = append(files, GCMFile{Path: "DATA/" + name, Src: filepath.Join(export, e.Name())})
		bytes += info.Size()
	}
	if !mesh {
		return nil, report.Missing("no MESH00.bin in "+export, "export the game with the PlayStation 2 preset first")
	}
	sort.Slice(files, func(i, j int) bool { return files[i].Path < files[j].Path })
	if log != nil {
		report.Linef(log, report.Staged, "%d file(s), %d KiB, in DATA", len(files), bytes/1024)
	}
	return files, nil
}

func gdirExportDir(packDir, out string) (string, error) {
	dir := strings.TrimSpace(packDir)
	if dir == "" {
		if out == "" {
			return "", report.Usage("build --sample gdir needs --out (the export folder is where it goes) or --pack-dir")
		}
		dir = filepath.Dir(out)
	}
	abs, err := filepath.Abs(dir)
	if err != nil {
		return "", fmt.Errorf("export dir: %w", err)
	}
	return abs, nil
}

// exportIcon: the export's banner picture, if it has one (BANNER.PNG, else
// the PS3's ICON0.PNG).
func exportIcon(export string) string {
	for _, name := range []string{"BANNER.PNG", "banner.png", "ICON0.PNG", "icon0.png"} {
		if fileExists(filepath.Join(export, name)) {
			return filepath.Join(export, name)
		}
	}
	return ""
}
