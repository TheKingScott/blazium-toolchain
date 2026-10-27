// Package guest embeds the MIT Blazium Wii GDIR guest: the PS2 GDIR
// runtime's VM, packs and system natives over the Wii's GX, Wii Remotes and
// GameCube controllers, audio interface and SD card, built with devkitPPC,
// libogc and libfat (wii build --sample gdir).
package guest

import (
	"crypto/sha256"
	"embed"
	"encoding/hex"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

//go:embed gdir
var gdirFiles embed.FS

// GDIRShared are the Wii GDIR guest files kept byte-identical to
// guest/ps2/runtime (guest_test checks): one VM for every console.
var GDIRShared = []string{
	"gdir_ops.h",
	"gdir_vm.cpp",
	"gdir_vm.h",
	"mc_io.h",
	"pack_io.cpp",
	"pack_io.h",
	"pad_io.h",
	"sfx_io.h",
	"sys_io.cpp",
	"sys_io.h",
}

// AppDirMarker starts the 64 bytes of the DOL that hold the app folder's
// name (data_io.cpp's blazium_app_dir): the marker, then the name,
// zero-padded. wii build writes the name there.
const AppDirMarker = "BLZAPPDIR:"

// AppDirBytes is the size of that field, the marker included.
const AppDirBytes = 64

// InstallGDIR writes the GDIR guest into destDir.
func InstallGDIR(destDir string) error {
	if err := os.MkdirAll(destDir, 0o755); err != nil {
		return err
	}
	return fs.WalkDir(gdirFiles, "gdir/runtime", func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if path == "gdir/runtime" {
			return nil
		}
		rel := strings.TrimPrefix(path, "gdir/runtime/")
		target := filepath.Join(destDir, filepath.FromSlash(rel))
		if d.IsDir() {
			return os.MkdirAll(target, 0o755)
		}
		data, readErr := gdirFiles.ReadFile(path)
		if readErr != nil {
			return readErr
		}
		return os.WriteFile(target, data, 0o644)
	})
}

// Overlay copies the files of overlayDir onto destDir (replacements or extra
// sources for the guest).
func Overlay(destDir, overlayDir string) error {
	st, err := os.Stat(overlayDir)
	if err != nil {
		return fmt.Errorf("overlay: %w", err)
	}
	if !st.IsDir() {
		return fmt.Errorf("overlay is not a directory: %s", overlayDir)
	}
	return filepath.WalkDir(overlayDir, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(overlayDir, path)
		if err != nil || rel == "." {
			return err
		}
		target := filepath.Join(destDir, rel)
		if d.IsDir() {
			return os.MkdirAll(target, 0o755)
		}
		data, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		return os.WriteFile(target, data, 0o644)
	})
}

//go:embed notices
var noticeFiles embed.FS

// Notice is a third-party licence text the game's boot.dol carries code
// under: libogc.txt (libogc's libraries and start-up code), wiiuse.txt
// (the Wii Remote library, GPLv3 with a linking exception), libfat.txt
// (the FAT file system) and newlib.txt (the C library).
func Notice(name string) ([]byte, error) {
	return noticeFiles.ReadFile("notices/" + name)
}

//go:embed prebuilt
var prebuiltFiles embed.FS

// PrebuiltDOL is the guest compiled with devkitPPC, libogc and libfat:
// the boot.dol wii build puts in the app folder when the game adds no code
// of its own. scripts/wii-guest-prebuilt.sh rebuilds it.
func PrebuiltDOL() ([]byte, error) {
	return prebuiltFiles.ReadFile("prebuilt/boot.dol")
}

// PrebuiltVersion is SourcesHash as it was when boot.dol was built.
func PrebuiltVersion() string {
	raw, err := prebuiltFiles.ReadFile("prebuilt/SOURCES")
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(raw))
}

// SourcesHash identifies the guest's sources (gdir/runtime, line endings
// aside): the first 16 hex digits of a SHA-256 over each file's path and
// bytes.
func SourcesHash() (string, error) {
	var names []string
	err := fs.WalkDir(gdirFiles, "gdir/runtime", func(path string, d fs.DirEntry, err error) error {
		if err == nil && !d.IsDir() {
			names = append(names, path)
		}
		return err
	})
	if err != nil {
		return "", err
	}
	sort.Strings(names)
	h := sha256.New()
	for _, n := range names {
		raw, err := gdirFiles.ReadFile(n)
		if err != nil {
			return "", err
		}
		fmt.Fprintf(h, "%s\n", n)
		h.Write([]byte(strings.ReplaceAll(string(raw), "\r\n", "\n")))
	}
	return hex.EncodeToString(h.Sum(nil))[:16], nil
}
