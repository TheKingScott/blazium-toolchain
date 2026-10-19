// Package guest embeds the MIT Blazium N64 Makefile stub shipped with this CLI.
// Game developers never need the editor source tree; n64 setup / export-guest
// writes these files, and n64 build uses them when --src is omitted. --overlay
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
// and guest/n64/runtime (BLAZIUM_N64_COOK_ABI). Do not reuse PS1/PS2 ABI numbers
// as a wire format — this is a new N64_COOK_ABI.
const CookABI = 1

// RuntimeNames are the files Install writes from the embed.
var RuntimeNames = []string{
	"CMakeLists.txt",
	"Makefile",
	"main.cpp",
	"rdpq_draw.cpp",
	"rdpq_draw.h",
	"pad_io.cpp",
	"pad_io.h",
	"sfx_io.cpp",
	"sfx_io.h",
	"pack_io.cpp",
	"pack_io.h",
	"script_vm.cpp",
	"script_vm.h",
	"sys_io.cpp",
	"sys_io.h",
	"dfs_io.cpp",
	"dfs_io.h",
	"guest_hooks.h",
	"extra/user_hooks.cpp",
}

//go:embed runtime
var files embed.FS

// The GDIR guest (guest/n64/gdir/runtime): the PS2 GDIR runtime's VM, packs
// and system natives over a Tiny3D renderer, for scenes cooked by the PS2
// exporter and converted to the PS1 forms (n64 build --sample gdir).
//
//go:embed gdir
var gdirFiles embed.FS

// GDIRShared are the N64 GDIR guest files kept byte-identical to
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

// Install writes the guest stub into destDir (prefix/n64/guest/runtime).
func Install(destDir string) error {
	if err := os.MkdirAll(destDir, 0o755); err != nil {
		return err
	}
	return fs.WalkDir(files, "runtime", func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if path == "runtime" {
			return nil
		}
		rel := strings.TrimPrefix(path, "runtime/")
		target := filepath.Join(destDir, filepath.FromSlash(rel))
		if d.IsDir() {
			return os.MkdirAll(target, 0o755)
		}
		data, readErr := files.ReadFile(path)
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
// stub; extra *.cpp / *.h / *.c / *.S are added.
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
	case ".exe", ".elf", ".z64", ".n64", ".v64", ".import", ".obj", ".o", ".a":
		return true
	}
	return false
}
