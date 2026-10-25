package n64

// n64 build --sample gdir: the GDIR guest (guest/n64/gdir/runtime) with a
// PS2 GDIR export in its ROM. The export (--pack-dir, or the folder --out
// is in) is what the PS2 exporter writes; its packs are converted to the PS1
// forms the N64 renderer reads (ps1.ConvertPacks: int16 positions, texel UVs,
// culling chunks; P1TX textures), the scene node records are split out of
// SCRP00 as on the PS1, and everything goes into the DFS under DATA/ in
// upper case. Sound stays IMA ADPCM (the guest decodes it).

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"

	guest "github.com/blazium-games/blazium-toolchain/guest/n64"
	"github.com/blazium-games/blazium-toolchain/internal/cache"
	"github.com/blazium-games/blazium-toolchain/internal/platforms/ps1"
	"github.com/blazium-games/blazium-toolchain/internal/report"
)

// GDIRGuestDir is <prefix>/n64/guest/gdir: the GDIR guest, staged per build.
func GDIRGuestDir(prefix string) string {
	return filepath.Join(cache.PlatformDir(prefix, ID), "guest", "gdir")
}

// gdirDataName: the export's files that go in the ROM (cooked .bin and the
// boot logo), not the builds beside them.
func gdirDataName(name string) bool {
	up := strings.ToUpper(name)
	switch filepath.Ext(up) {
	case ".BIN", ".RAW":
		return true
	}
	return false
}

// stageGDIR installs the guest into src and the export's data into
// src/filesystem/DATA. Returns the number of files staged.
func stageGDIR(src, export string, log io.Writer) (int, error) {
	if err := os.RemoveAll(src); err != nil {
		return 0, err
	}
	if err := guest.InstallGDIR(src); err != nil {
		return 0, err
	}
	ents, err := os.ReadDir(export)
	if err != nil {
		return 0, report.Missing("no GDIR export at "+export, "export the game with the PlayStation 2 preset, then n64 build --sample gdir --pack-dir DIR")
	}
	data := filepath.Join(src, "filesystem", "DATA")
	if err := os.MkdirAll(data, 0o755); err != nil {
		return 0, err
	}
	n := 0
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
	}
	if _, err := os.Stat(filepath.Join(data, "MESH00.BIN")); err != nil {
		return n, report.Missing("no MESH00.bin in "+export, "export the game with the PlayStation 2 preset first")
	}
	conv, err := ps1.ConvertPacks(data)
	if err != nil {
		return n, report.Fail("convert packs: "+err.Error(), "re-export the scenes with the PlayStation 2 cook")
	}
	moved, err := ps1.SplitPackNodes(data)
	if err != nil {
		return n, report.Fail("split pack nodes: "+err.Error(), "re-export the scenes with the PlayStation 2 cook")
	}
	if log != nil {
		report.Linef(log, report.Note, "%d file(s) staged, %d pack mesh(es) converted, %d bytes of node records moved to PNOD files", n, conv, moved)
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
