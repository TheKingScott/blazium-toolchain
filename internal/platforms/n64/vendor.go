package n64

import (
	"os"
	"path/filepath"
	"runtime"
	"strings"

	"github.com/blazium-games/blazium-toolchain/internal/cache"
	"github.com/blazium-games/blazium-toolchain/internal/settings"
)

func vendorRoots(prefix string) []string {
	var roots []string
	for _, p := range cache.VendorRoots(prefix, ID) {
		if p != "" && !forbiddenUltra(p) {
			roots = append(roots, p)
		}
	}
	return roots
}

func hostNames(base string) []string {
	if runtime.GOOS == "windows" {
		return []string{base, base + ".exe"}
	}
	return []string{base}
}

func walkNamed(root string, names ...string) string {
	if root == "" || forbiddenUltra(root) {
		return ""
	}
	if st, err := os.Stat(root); err != nil || !st.IsDir() {
		return ""
	}
	want := make(map[string]bool, len(names))
	for _, n := range names {
		want[strings.ToLower(n)] = true
	}
	var found string
	_ = filepath.WalkDir(root, func(path string, d os.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return nil
		}
		if forbiddenUltra(path) {
			return nil
		}
		if want[strings.ToLower(d.Name())] {
			found = path
			return filepath.SkipAll
		}
		return nil
	})
	if found == "" {
		return ""
	}
	if abs, err := filepath.Abs(found); err == nil {
		return abs
	}
	return found
}

func fileExists(p string) bool {
	if p == "" || forbiddenUltra(p) {
		return false
	}
	st, err := os.Stat(p)
	return err == nil && !st.IsDir()
}

func dirExists(p string) bool {
	if p == "" || forbiddenUltra(p) {
		return false
	}
	st, err := os.Stat(p)
	return err == nil && st.IsDir()
}

func absOr(p string) string {
	if abs, err := filepath.Abs(p); err == nil {
		return abs
	}
	return p
}

func lookFile(name string) string {
	p, err := lookPath(name)
	if err != nil {
		return ""
	}
	if forbiddenUltra(p) {
		return ""
	}
	return p
}

func lookHostMingw(base string) string {
	if p := lookFile(base); p != "" {
		return p
	}
	for _, dir := range settings.Current().N64.HostMingw {
		for _, n := range hostNames(base) {
			p := filepath.Join(dir, n)
			if fileExists(p) {
				return p
			}
		}
	}
	return ""
}

func forbiddenUltra(p string) bool {
	n := strings.ToLower(filepath.Clean(p))
	slash := filepath.ToSlash(n)
	return strings.Contains(slash, "/ultra/") || strings.HasSuffix(slash, "/ultra")
}

func siblingN64Stuff() []string {
	var out []string
	seen := map[string]bool{}
	add := func(p string) {
		if p == "" || forbiddenUltra(p) {
			return
		}
		if abs, err := filepath.Abs(p); err == nil {
			p = abs
		}
		if seen[p] || !dirExists(p) {
			return
		}
		seen[p] = true
		out = append(out, p)
	}
	roots := settings.Current().N64.SiblingRoots
	if len(roots) == 0 {
		roots = []string{"n64_stuff"}
	}
	if exe, err := os.Executable(); err == nil {
		dir := filepath.Dir(exe)
		for _, name := range roots {
			add(filepath.Join(dir, "..", name))
			add(filepath.Join(dir, name))
			add(filepath.Join(dir, "..", "..", name))
		}
	}
	if wd, err := os.Getwd(); err == nil {
		for _, name := range roots {
			add(filepath.Join(wd, name))
			add(filepath.Join(wd, "..", name))
			add(filepath.Join(wd, "..", "..", name))
		}
	}
	return out
}

func lookBash() string {
	for _, p := range settings.Current().N64.BashPath {
		if fileExists(p) {
			return p
		}
	}
	p := lookFile("bash")
	if p != "" && !strings.Contains(strings.ToLower(p), `system32\bash`) && !strings.Contains(strings.ToLower(p), `/system32/bash`) {
		return p
	}
	return ""
}

func siblingLibdragon() string {
	for _, stuff := range siblingN64Stuff() {
		lib := filepath.Join(stuff, "libdragon")
		if fileExists(filepath.Join(lib, "n64.mk")) {
			return absOr(lib)
		}
	}
	return ""
}

func siblingTiny3d() string {
	for _, stuff := range siblingN64Stuff() {
		t3d := filepath.Join(stuff, "tiny3d")
		if fileExists(filepath.Join(t3d, "t3d.mk")) {
			return absOr(t3d)
		}
	}
	return ""
}

func cachedSrcDir(prefix, name string) string {
	if prefix == "" {
		return ""
	}
	return cache.SrcDir(prefix, ID, name)
}

func findSrcRoot(dir, marker string) string {
	if dir == "" {
		return ""
	}
	if fileExists(filepath.Join(dir, marker)) {
		return absOr(dir)
	}
	if !dirExists(dir) {
		return ""
	}
	entries, err := os.ReadDir(dir)
	if err != nil {
		return ""
	}
	var only string
	for _, e := range entries {
		if !e.IsDir() || strings.HasPrefix(e.Name(), ".") {
			continue
		}
		if only != "" {
			only = ""
			break
		}
		only = filepath.Join(dir, e.Name())
	}
	if only != "" && fileExists(filepath.Join(only, marker)) {
		return absOr(only)
	}
	var found string
	_ = filepath.WalkDir(dir, func(path string, d os.DirEntry, err error) error {
		if err != nil || !d.IsDir() {
			return nil
		}
		if fileExists(filepath.Join(path, marker)) {
			found = path
			return filepath.SkipAll
		}
		return nil
	})
	if found != "" {
		return absOr(found)
	}
	return ""
}

func libdragonRoot(prefix string) string {
	if v := os.Getenv("LIBDRAGON_SRC"); v != "" && fileExists(filepath.Join(v, "n64.mk")) {
		return absOr(v)
	}
	if p := findSrcRoot(cachedSrcDir(prefix, "libdragon"), "n64.mk"); p != "" {
		return p
	}
	return siblingLibdragon()
}

func tiny3dRoot(prefix string) string {
	if v := os.Getenv("T3D_INST"); v != "" && fileExists(filepath.Join(v, "t3d.mk")) {
		return absOr(v)
	}
	if p := findSrcRoot(cachedSrcDir(prefix, "tiny3d"), "t3d.mk"); p != "" {
		return p
	}
	return siblingTiny3d()
}

func toolchainRoot(dest string) string {
	if dest == "" {
		return ""
	}
	gcc := walkNamed(dest, hostNames("mips64-elf-gcc")...)
	if gcc == "" {
		return dest
	}
	return absOr(filepath.Dir(filepath.Dir(gcc)))
}

func t3dLibPath(root string) string {
	if root == "" {
		return ""
	}
	return filepath.Join(root, "build", "libt3d.a")
}
