package ps1

// ps1 iso --dir: a PS1 disc from a GDIR export folder. The folder is what
// the PS2 exporter writes (MESHnn/NODEnn/SCRPnn... packs and sidecars) plus
// the PS1 GDIR guest's PS-X EXE (ps1 build --sample gdir). The packs are
// converted to the PS1 forms (ConvertPacks) in a staging copy, laid out like
// the PS2 disc (DATA/, each pack's files in DATA/Pnn/), and the EXE boots as
// a product-code name through SYSTEM.CNF. mkpsxiso writes the image.

import (
	"context"
	"encoding/xml"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/blazium-games/blazium-toolchain/internal/cache"
	"github.com/blazium-games/blazium-toolchain/internal/platforms"
	"github.com/blazium-games/blazium-toolchain/internal/report"
)

// GDIRDataDir is the disc folder for cooked data. Keep in sync with
// guest/ps1/gdir/runtime/iso_path.h (shared with the PS2 guest).
const GDIRDataDir = "DATA"

// GDIRBootName is the boot EXE's name on a GDIR disc when --boot-elf is
// not given. The PS1 BIOS boots any name SYSTEM.CNF gives, but emulators
// take the name as the disc's serial and look it up in their game lists
// (DuckStation shows SLUS_000.01 as "Air Combat" and applies that game's
// settings), so the default is a prefix no retail disc uses.
const GDIRBootName = "BLAZ_000.01"

func (t *Tool) gdirISO(ctx context.Context, opts platforms.ISOOptions) error {
	if opts.Out == "" {
		return report.Usage("iso --dir requires --out (the .bin; the .cue is written beside it)")
	}
	dir, err := filepath.Abs(opts.Dir)
	if err != nil {
		return err
	}
	out, err := filepath.Abs(opts.Out)
	if err != nil {
		return err
	}
	boot := strings.ToUpper(strings.TrimSpace(opts.BootELF))
	if boot == "" {
		boot = GDIRBootName
	}
	if !isBootName(boot) {
		return report.Usage("--boot-elf must be a product code like SLUS_000.01 (four letters, _, three digits, ., two digits)")
	}
	exe, files, err := gdirDiscFiles(dir, out)
	if err != nil {
		return err
	}
	if exe == "" {
		return report.Missing("no PS-X EXE in "+dir, "blazium-toolchain ps1 build --sample gdir --out "+filepath.Join(dir, "GAME.EXE"))
	}

	work := filepath.Join(cache.PlatformDir(opts.Prefix, ID), "work")
	if err := os.MkdirAll(work, 0o755); err != nil {
		return err
	}
	stage, err := os.MkdirTemp(work, "gdir-disc-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(stage)
	data := filepath.Join(stage, "data")
	if err := os.MkdirAll(data, 0o755); err != nil {
		return err
	}
	for _, name := range files {
		raw, err := os.ReadFile(filepath.Join(dir, name))
		if err != nil {
			return err
		}
		if err := os.WriteFile(filepath.Join(data, strings.ToUpper(name)), raw, 0o644); err != nil {
			return err
		}
	}
	n, err := ConvertPacks(data)
	if err != nil {
		return report.Fail("convert packs: "+err.Error(), "re-export the scenes with the PlayStation 2 cook")
	}
	if opts.Stdout != nil {
		report.Linef(opts.Stdout, report.Note, "%d pack mesh(es) converted for the PS1", n)
	}
	moved, err := SplitPackNodes(data)
	if err != nil {
		return report.Fail("split pack nodes: "+err.Error(), "re-export the scenes with the PlayStation 2 cook")
	}
	if moved > 0 && opts.Stdout != nil {
		report.Linef(opts.Stdout, report.Note, "%d bytes of scene node records moved from SCRP00 to PNOD files", moved)
	}
	if made, err := MakeMCPS1(data, boot); err != nil {
		return report.Fail("memory card save header: "+err.Error(), "re-export the scenes with the PlayStation 2 cook")
	} else if made && opts.Stdout != nil {
		report.Linef(opts.Stdout, report.Note, "MCPS1.BIN: the memory card save's name, title and icon from MCSAVE.BIN")
	}
	cnf := filepath.Join(stage, "SYSTEM.CNF")
	if err := os.WriteFile(cnf, []byte(systemCNF(boot)), 0o644); err != nil {
		return err
	}
	staged, err := os.ReadDir(data)
	if err != nil {
		return err
	}
	var names []string
	for _, e := range staged {
		names = append(names, e.Name())
	}
	vol := strings.ToUpper(strings.TrimSpace(opts.VolumeID))
	if vol == "" {
		vol = "BLAZIUM"
	}
	cue := strings.TrimSuffix(out, filepath.Ext(out)) + ".cue"
	project := gdirISOXML(out, cue, vol, cnf, exe, boot, data, names)
	xmlPath := filepath.Join(stage, "iso.xml")
	if err := os.WriteFile(xmlPath, project, 0o644); err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(out), 0o755); err != nil {
		return err
	}
	mk, err := t.mkpsxiso(opts.Prefix)
	if err != nil {
		return err
	}
	if err := t.runner().Run(ctx, mk, []string{"-y", xmlPath}, writerOrDiscard(opts.Stdout), writerOrDiscard(opts.Stderr)); err != nil {
		return report.Fail("mkpsxiso: "+err.Error(), "")
	}
	if opts.Stdout != nil {
		report.Line(opts.Stdout, report.Wrote, out)
		report.Line(opts.Stdout, report.Wrote, cue)
	}
	return nil
}

// gdirDiscFiles finds the folder's PS-X EXE and the data files that go on
// the disc (sorted). out is skipped (it may be written into the folder).
func gdirDiscFiles(dir, out string) (string, []string, error) {
	ents, err := os.ReadDir(dir)
	if err != nil {
		return "", nil, err
	}
	exe := ""
	var files []string
	for _, e := range ents {
		if e.IsDir() {
			continue
		}
		p := filepath.Join(dir, e.Name())
		if p == out {
			continue
		}
		info, err := e.Info()
		if err != nil {
			return "", nil, err
		}
		if verifyPSXEXE(p) == nil {
			if exe == "" || strings.EqualFold(e.Name(), "GAME.EXE") {
				exe = p
			}
			continue
		}
		if skipOnGDIRDisc(e.Name(), info.Size()) || isDiscImage(p) {
			continue
		}
		files = append(files, e.Name())
	}
	sort.Strings(files)
	return exe, files, nil
}

// skipOnGDIRDisc leaves off what the guest never reads: the PS2 boot files,
// build notes, host saves, and sidecars cooked empty (header only:
// the guest treats a missing one the same). As the PS2 disc's skipOnDisc.
func skipOnGDIRDisc(name string, size int64) bool {
	up := strings.ToUpper(name)
	switch filepath.Ext(up) {
	case ".TXT", ".MAP", ".LOG", ".ELF", ".IRX", ".ISO", ".CUE", ".XML", ".CNF", ".RAW", ".EXE":
		return true
	}
	if !strings.HasSuffix(up, ".BIN") {
		return true
	}
	if strings.HasPrefix(up, "SAVE") {
		return true
	}
	if size <= 10 {
		for _, stem := range []string{"ANIM", "PART", "PATH", "SPRT", "VEHL", "NAVM", "NAV", "TILE", "FMV"} {
			if strings.HasPrefix(up, stem) {
				return true
			}
		}
	}
	return false
}

// gdirPackGroup is the DATA/ subfolder for a pack's file (MESH04.BIN ->
// P04), "" for shared files: as the PS2 disc, so one iso_path.h serves both.
func gdirPackGroup(name string) string {
	stem := strings.ToUpper(name)
	if i := strings.IndexByte(stem, '.'); i >= 0 {
		stem = stem[:i]
	}
	if len(stem) < 3 {
		return ""
	}
	d := stem[len(stem)-2:]
	if d[0] < '0' || d[0] > '9' || d[1] < '0' || d[1] > '9' {
		return ""
	}
	return "P" + d
}

// isDiscImage reports a raw CD image (an earlier disc written into the
// folder): its first sector opens with the CD sync pattern.
func isDiscImage(path string) bool {
	f, err := os.Open(path)
	if err != nil {
		return false
	}
	defer f.Close()
	var b [12]byte
	if n, _ := f.Read(b[:]); n < 12 {
		return false
	}
	return b == [12]byte{0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0}
}

func systemCNF(boot string) string {
	return "BOOT = cdrom:\\" + boot + ";1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFFF0\r\n"
}

func isBootName(s string) bool {
	if len(s) != 11 || s[4] != '_' || s[8] != '.' {
		return false
	}
	for i, c := range s {
		switch {
		case i < 4:
			if c < 'A' || c > 'Z' {
				return false
			}
		case i == 4 || i == 8:
		default:
			if c < '0' || c > '9' {
				return false
			}
		}
	}
	return true
}

func xmlAttr(s string) string {
	var b strings.Builder
	_ = xml.EscapeText(&b, []byte(s))
	return strings.ReplaceAll(b.String(), `"`, "&#34;")
}

// gdirISOXML is the mkpsxiso project: SYSTEM.CNF and the boot EXE at the
// root, shared files in DATA/, each pack's in DATA/Pnn/. Paths are absolute.
func gdirISOXML(out, cue, vol, cnf, exe, boot, data string, names []string) []byte {
	var b strings.Builder
	file := func(name, src string) {
		fmt.Fprintf(&b, "      <file name=\"%s\" source=\"%s\" type=\"data\"/>\n", xmlAttr(name), xmlAttr(filepath.ToSlash(src)))
	}
	b.WriteString("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n")
	fmt.Fprintf(&b, "<iso_project image_name=\"%s\" cue_sheet=\"%s\">\n", xmlAttr(filepath.ToSlash(out)), xmlAttr(filepath.ToSlash(cue)))
	b.WriteString("  <track type=\"data\">\n")
	fmt.Fprintf(&b, "    <identifiers system=\"PLAYSTATION\" application=\"PLAYSTATION\" volume=\"%s\" volume_set=\"%s\" publisher=\"BLAZIUM\" data_preparer=\"BLAZIUM TOOLCHAIN\"/>\n", xmlAttr(vol), xmlAttr(vol))
	b.WriteString("    <directory_tree>\n")
	file("SYSTEM.CNF", cnf)
	file(boot, exe)
	groups := map[string][]string{}
	var shared, order []string
	for _, n := range names {
		g := gdirPackGroup(n)
		if g == "" {
			shared = append(shared, n)
			continue
		}
		if _, ok := groups[g]; !ok {
			order = append(order, g)
		}
		groups[g] = append(groups[g], n)
	}
	sort.Strings(order)
	fmt.Fprintf(&b, "      <dir name=\"%s\">\n", GDIRDataDir)
	for _, n := range shared {
		b.WriteString("  ")
		file(n, filepath.Join(data, n))
	}
	for _, g := range order {
		fmt.Fprintf(&b, "        <dir name=\"%s\">\n", g)
		for _, n := range groups[g] {
			b.WriteString("    ")
			file(n, filepath.Join(data, n))
		}
		b.WriteString("        </dir>\n")
	}
	b.WriteString("      </dir>\n")
	b.WriteString("    </directory_tree>\n  </track>\n</iso_project>\n")
	return []byte(b.String())
}
