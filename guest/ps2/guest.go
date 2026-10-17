// Package guest embeds the MIT Blazium PS2 CMake/Makefile stub shipped with this CLI.
// Game developers never need the editor source tree; ps2 setup / export-guest
// writes these files, and ps2 build uses them when --src is omitted. --overlay
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
// and guest/ps2/runtime (BLAZIUM_PS2_COOK_ABI).
const CookABI = 1

// RuntimeNames are the files Install writes from the embed.
var RuntimeNames = []string{
	"CMakeLists.txt",
	"Makefile",
	"main.cpp",
	"gs_draw.cpp",
	"gs_draw.h",
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
	"vu1_draw.cpp",
	"vu1_draw.h",
	"gdir_vm.cpp",
	"gdir_vm.h",
	"mc_io.cpp",
	"mc_io.h",
	"gdir_ops.h",
	"iso_path.h",
	"draw_3D.vsm",
	"vu1_scene.vsm",
	"guest_hooks.h",
	"extra/user_hooks.cpp",
}

//go:embed runtime
var files embed.FS

// Install writes the guest stub into destDir (prefix/ps2/guest/runtime).
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
	case ".exe", ".elf", ".import", ".obj", ".o", ".a":
		return true
	}
	return false
}
