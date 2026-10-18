// Package guest embeds the MIT Blazium PS1 CMake stub shipped with this CLI.
// Game developers never need the editor source tree; ps1 setup / export-guest
// writes these files, and ps1 build uses them when --src is omitted. --overlay
// copies replacement or extra *.cpp on top of the stub.
package guest

import (
	"embed"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"strings"

	"github.com/blazium-games/blazium-toolchain/internal/safepath"
)

// CookABI is the cooked blob layout version. Bump together with the editor cooker
// and guest/ps1/runtime/CMakeLists.txt (BLAZIUM_PS1_COOK_ABI).
const CookABI = 20

// RuntimeNames are the files Install writes from the embed.
var RuntimeNames = []string{
	"CMakeLists.txt",
	"main.cpp",
	"script_vm.cpp",
	"script_vm.h",
	"fmv_play.cpp",
	"fmv_play.h",
}

//go:embed runtime
var files embed.FS

// The GDIR guest (guest/ps1/gdir/runtime): the PS2 GDIR runtime's VM, packs
// and system natives over a GTE/GPU renderer, for scenes cooked by the PS2
// exporter and converted by internal/platforms/ps1 (ConvertPacks).
//
//go:embed gdir
var gdirFiles embed.FS

// GDIRShared are the GDIR guest files kept byte-identical to
// guest/ps2/runtime (guest_test checks): one VM for both consoles.
var GDIRShared = []string{
	"gdir_ops.h",
	"gdir_vm.cpp",
	"gdir_vm.h",
	"iso_path.h",
	"mc_io.h",
	"pack_io.cpp",
	"pack_io.h",
	"pad_io.h",
	"sfx_io.h",
	"sys_io.cpp",
	"sys_io.h",
}

// InstallGDIR writes the GDIR guest into destDir (prefix/ps1/guest/gdir).
func InstallGDIR(destDir string) error {
	return installFrom(gdirFiles, "gdir/runtime", destDir)
}

// Install writes the guest stub into destDir (prefix/ps1/guest/runtime).
func Install(destDir string) error {
	return installFrom(files, "runtime", destDir)
}

func installFrom(efs embed.FS, root, destDir string) error {
	if err := os.MkdirAll(destDir, 0o755); err != nil {
		return err
	}
	return fs.WalkDir(efs, root, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if path == root {
			return nil
		}
		rel := strings.TrimPrefix(path, root+"/")
		target := filepath.Join(destDir, filepath.FromSlash(rel))
		if d.IsDir() {
			return os.MkdirAll(target, 0o755)
		}
		data, readErr := efs.ReadFile(path)
		if readErr != nil {
			return readErr
		}
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			return err
		}
		return os.WriteFile(target, data, 0o644)
	})
}

// Overlay copies overlayDir onto destDir. Same-relative-path files replace the
// stub; extra *.cpp / *.h / *.c / *.S are added. Build skips .git, build trees,
// and binaries. destDir must already exist (Install first).
func Overlay(destDir, overlayDir string) error {
	if overlayDir == "" {
		return nil
	}
	st, err := os.Stat(overlayDir)
	if err != nil {
		return fmt.Errorf("overlay: %w", err)
	}
	if !st.IsDir() {
		return fmt.Errorf("overlay is not a directory: %s", overlayDir)
	}
	if err := os.MkdirAll(destDir, 0o755); err != nil {
		return err
	}
	destDir, err = filepath.Abs(destDir)
	if err != nil {
		return err
	}
	overlayDir, err = filepath.Abs(overlayDir)
	if err != nil {
		return err
	}
	return filepath.WalkDir(overlayDir, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, relErr := filepath.Rel(overlayDir, path)
		if relErr != nil {
			return relErr
		}
		if rel == "." {
			return nil
		}
		if skipOverlay(rel, d) {
			if d.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		if d.Type()&os.ModeSymlink != 0 {
			if err := safepath.ResolveUnder(overlayDir, path); err != nil {
				return fmt.Errorf("overlay: %w", err)
			}
		}
		target, joinErr := safepath.Join(destDir, rel)
		if joinErr != nil {
			return fmt.Errorf("overlay: %w", joinErr)
		}
		if d.IsDir() {
			return os.MkdirAll(target, 0o755)
		}
		data, readErr := os.ReadFile(path)
		if readErr != nil {
			return readErr
		}
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			return err
		}
		return os.WriteFile(target, data, 0o644)
	})
}

func skipOverlay(rel string, d fs.DirEntry) bool {
	base := filepath.Base(rel)
	switch strings.ToLower(base) {
	case ".git", ".godot", "cmakefiles", "build", "cmakecache.txt":
		return true
	}
	ext := strings.ToLower(filepath.Ext(base))
	switch ext {
	case ".exe", ".elf", ".import", ".obj", ".o", ".a":
		return true
	}
	return false
}
