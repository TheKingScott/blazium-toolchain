package ps3

// ps3 build --sample gdir: the GDIR guest (guest/ps3/gdir/runtime) with a
// PS2 GDIR export in its game folder. The export (--pack-dir, or the folder
// --out is in) is what the PS2 exporter writes; the PS3 reads its packs as
// they are (the guest's RSX renderer takes the PS2's MESH and GTEX), so the
// cooked files are only copied, under upper-case names, to
// pkg/USRDIR/DATA/. The game's icon (320x176 PNG) is --icon, else the
// export's ICON0.PNG, else PSL1GHT's.

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"

	guest "github.com/blazium-games/blazium-toolchain/guest/ps3"
	"github.com/blazium-games/blazium-toolchain/internal/report"
)

// gdirDataName: the export's cooked files (.bin), not the builds, the PS2
// disc's boot logo or the notes beside them.
func gdirDataName(name string) bool {
	return strings.EqualFold(filepath.Ext(name), ".bin")
}

// stageGDIR installs the guest into src and the export's data into
// src/pkg/USRDIR/DATA (and the icon as src/pkg/ICON0.PNG). Returns the
// number of files staged.
func stageGDIR(src, export, icon string, log io.Writer) (int, error) {
	if err := os.RemoveAll(src); err != nil {
		return 0, err
	}
	if err := guest.InstallGDIR(src); err != nil {
		return 0, err
	}
	ents, err := os.ReadDir(export)
	if err != nil {
		return 0, report.Missing("no GDIR export at "+export, "export the game with the PlayStation 2 preset, then ps3 build --sample gdir --pack-dir DIR")
	}
	data := filepath.Join(src, "pkg", "USRDIR", "DATA")
	if err := os.MkdirAll(data, 0o755); err != nil {
		return 0, err
	}
	n := 0
	var bytes int64
	for _, e := range ents {
		if e.IsDir() || !gdirDataName(e.Name()) {
			continue
		}
		raw, err := os.ReadFile(filepath.Join(export, e.Name()))
		if err != nil {
			return n, err
		}
		if err := os.WriteFile(filepath.Join(data, strings.ToUpper(e.Name())), raw, 0o644); err != nil {
			return n, err
		}
		n++
		bytes += int64(len(raw))
	}
	if _, err := os.Stat(filepath.Join(data, "MESH00.BIN")); err != nil {
		return n, report.Missing("no MESH00.bin in "+export, "export the game with the PlayStation 2 preset first")
	}
	if icon == "" {
		for _, name := range []string{"ICON0.PNG", "icon0.png", "ICON0.png"} {
			if fileExists(filepath.Join(export, name)) {
				icon = filepath.Join(export, name)
				break
			}
		}
	}
	if icon != "" {
		if err := copyFile(icon, filepath.Join(src, "pkg", "ICON0.PNG")); err != nil {
			return n, report.Fail("icon: "+err.Error(), "--icon is a 320x176 PNG")
		}
	}
	if log != nil {
		report.Linef(log, report.Staged, "%d file(s), %d KiB, in USRDIR/DATA", n, bytes/1024)
	}
	return n, nil
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
