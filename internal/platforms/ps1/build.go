package ps1

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"

	guest "github.com/blazium-games/blazium-toolchain/guest/ps1"
	"github.com/blazium-games/blazium-toolchain/internal/cache"
	"github.com/blazium-games/blazium-toolchain/internal/platforms"
	"github.com/blazium-games/blazium-toolchain/internal/report"
)

// GuestDir is <prefix>/ps1/guest/runtime — the bundled MIT stub.
func GuestDir(prefix string) string {
	return filepath.Join(cache.PlatformDir(prefix, ID), "guest", "runtime")
}

func installGuestRuntime(prefix string) error {
	return guest.Install(GuestDir(prefix))
}

// GDIRGuestDir is <prefix>/ps1/guest/gdir — the bundled GDIR runtime
// (build --sample gdir), for scenes cooked by the PS2 exporter.
func GDIRGuestDir(prefix string) string {
	return filepath.Join(cache.PlatformDir(prefix, ID), "guest", "gdir")
}

const psxMagic = "PS-X EXE"

type envRunner interface {
	RunEnv(ctx context.Context, name string, args []string, extraPath []string, extraEnv map[string]string, stdout, stderr io.Writer) error
}

func (t *Tool) Build(ctx context.Context, opts platforms.BuildOptions) error {
	if err := requireHost(); err != nil {
		return err
	}
	env, err := t.compileEnv(opts.CommonOptions)
	if err != nil {
		return err
	}
	if !compileReady(env) {
		return report.Missing("ps1 compile tools not ready", "blazium-toolchain ps1 setup --profile compile")
	}

	src := opts.Src
	sample := strings.ToLower(strings.TrimSpace(opts.Sample))
	overlay := strings.TrimSpace(opts.Overlay)
	if overlay == "" {
		overlay = strings.TrimSpace(os.Getenv("BLAZIUM_PS1_OVERLAY"))
	}
	if src != "" && !fileExists(filepath.Join(src, "CMakeLists.txt")) {
		if overlay == "" {
			overlay = src
		}
		src = ""
	}
	if src == "" && sample == "gdir" {
		src = GDIRGuestDir(opts.Prefix)
		if err := os.RemoveAll(src); err != nil {
			return err
		}
		if err := guest.InstallGDIR(src); err != nil {
			return err
		}
	} else if src == "" && sample != "" {
		src, err = resolveSample(opts.Prefix, sample)
		if err != nil {
			return err
		}
	}
	if overlay != "" && (sample == "" || sample == "gdir") {
		merge := filepath.Join(cache.PlatformDir(opts.Prefix, ID), "work", "guest-src")
		if err := os.RemoveAll(merge); err != nil {
			return err
		}
		if src != "" {
			if err := guest.Overlay(merge, src); err != nil {
				return err
			}
		} else {
			if err := guest.Install(merge); err != nil {
				return err
			}
		}
		if err := guest.Overlay(merge, overlay); err != nil {
			return err
		}
		src = merge
	}
	if src == "" {
		if err := installGuestRuntime(opts.Prefix); err != nil {
			return err
		}
		src = GuestDir(opts.Prefix)
	}
	if src == "" || opts.Out == "" {
		return report.Usage("build requires --out FILE")
	}
	if dest := strings.TrimSpace(opts.ExportSrc); dest != "" {
		if err := os.RemoveAll(dest); err != nil {
			return err
		}
		if err := guest.Overlay(dest, src); err != nil {
			return report.Fail("export-src: "+err.Error(), "")
		}
		if opts.Stdout != nil {
			report.Line(opts.Stdout, report.Wrote, dest)
		}
	}

	target := cmakeTarget(sample, src)
	if sample == "gte" {
		if walkNamed(src, "texture.tim") == "" && !fileExists(filepath.Join(src, "texture.tim")) {
			return report.Missing("gte sample is missing texture.tim", "use --sample template or --src with the asset")
		}
	}

	cmake, ninja, err := t.ensureHostBuildTools(ctx, opts.Prefix, opts.Stdout)
	if err != nil {
		return err
	}

	workName := or(sample, filepath.Base(src))
	if opts.Src == "" && sample == "" {
		workName = "blazium-guest"
	}
	buildDir := filepath.Join(cache.PlatformDir(opts.Prefix, ID), "work", workName)
	if err := os.MkdirAll(buildDir, 0o755); err != nil {
		return err
	}

	sdkFile := filepath.Join(env["PSN00BSDK_LIBS"], "cmake", "sdk.cmake")
	if !fileExists(sdkFile) {
		return report.Missing("sdk.cmake not under PSN00BSDK_LIBS ("+env["PSN00BSDK_LIBS"]+")", "blazium-toolchain ps1 setup --profile compile")
	}

	extraPath := []string{
		filepath.Dir(env["MIPS_GCC"]),
		filepath.Dir(env["ELF2X"]),
		filepath.Dir(cmake),
		filepath.Dir(ninja),
	}
	extraEnv := map[string]string{
		"PSN00BSDK_LIBS": env["PSN00BSDK_LIBS"],
		"PSN00BSDK_TC":   env["PSN00BSDK_TC"],
		"MIPS_GCC":       env["MIPS_GCC"],
		"ELF2X":          env["ELF2X"],
	}

	cfg := []string{
		"-S", src,
		"-B", buildDir,
		"-G", "Ninja",
		"-DCMAKE_TOOLCHAIN_FILE=" + sdkFile,
		"-DPSN00BSDK_TC=" + env["PSN00BSDK_TC"],
		"-DCMAKE_BUILD_TYPE=" + buildType(sample),
		"-DCMAKE_MAKE_PROGRAM=" + ninja,
	}
	if opts.Tim != "" {
		cfg = append(cfg, "-DBLAZIUM_PS1_TIM="+filepath.ToSlash(opts.Tim))
	}
	if opts.Mesh != "" {
		cfg = append(cfg, "-DBLAZIUM_PS1_MESH="+filepath.ToSlash(opts.Mesh))
	}
	if opts.Vag != "" {
		cfg = append(cfg, "-DBLAZIUM_PS1_VAG="+filepath.ToSlash(opts.Vag))
	}
	if opts.Sprite != "" {
		cfg = append(cfg, "-DBLAZIUM_PS1_SPRITE="+filepath.ToSlash(opts.Sprite))
	}
	if opts.Script != "" {
		cfg = append(cfg, "-DBLAZIUM_PS1_SCRIPT="+filepath.ToSlash(opts.Script))
	}
	if opts.Gdbc != "" {
		cfg = append(cfg, "-DBLAZIUM_PS1_GDBC="+filepath.ToSlash(opts.Gdbc))
	}
	if opts.Luau != "" {
		cfg = append(cfg, "-DBLAZIUM_PS1_LUAU="+filepath.ToSlash(opts.Luau))
	}
	if opts.Str != "" {
		cfg = append(cfg, "-DBLAZIUM_PS1_STR="+filepath.ToSlash(opts.Str))
	}
	if opts.Xa != "" {
		cfg = append(cfg, "-DBLAZIUM_PS1_XA="+filepath.ToSlash(opts.Xa))
	}
	if opts.Node != "" {
		cfg = append(cfg, "-DBLAZIUM_PS1_NODE="+filepath.ToSlash(opts.Node))
	}
	if opts.Hud != "" {
		cfg = append(cfg, "-DBLAZIUM_PS1_HUD="+filepath.ToSlash(opts.Hud))
	}
	if opts.Tile != "" {
		cfg = append(cfg, "-DBLAZIUM_PS1_TILE="+filepath.ToSlash(opts.Tile))
	}
	if opts.Scene != "" {
		cfg = append(cfg, "-DBLAZIUM_PS1_SCENE="+filepath.ToSlash(opts.Scene))
	}
	if opts.Anim != "" {
		cfg = append(cfg, "-DBLAZIUM_PS1_ANIM="+filepath.ToSlash(opts.Anim))
	}
	if opts.Cam != "" {
		cfg = append(cfg, "-DBLAZIUM_PS1_CAM="+filepath.ToSlash(opts.Cam))
	}
	if opts.Hit != "" {
		cfg = append(cfg, "-DBLAZIUM_PS1_HIT="+filepath.ToSlash(opts.Hit))
	}
	if opts.Nav != "" {
		cfg = append(cfg, "-DBLAZIUM_PS1_NAV="+filepath.ToSlash(opts.Nav))
	}
	if opts.Path != "" {
		cfg = append(cfg, "-DBLAZIUM_PS1_PATH="+filepath.ToSlash(opts.Path))
	}
	if opts.Way != "" {
		cfg = append(cfg, "-DBLAZIUM_PS1_WAY="+filepath.ToSlash(opts.Way))
	}
	if err := t.runEnv(ctx, cmake, cfg, extraPath, extraEnv, opts.Stdout, opts.Stderr); err != nil {
		return report.Fail("cmake configure: "+err.Error(), "blazium-toolchain ps1 setup --profile compile")
	}
	if err := t.runEnv(ctx, cmake, []string{"--build", buildDir, "--target", target}, extraPath, extraEnv, opts.Stdout, opts.Stderr); err != nil {
		return report.Fail("cmake build: "+err.Error(), "blazium-toolchain ps1 setup --profile compile")
	}

	guest, err := findPSXEXE(buildDir, target)
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(opts.Out), 0o755); err != nil {
		return err
	}
	raw, err := os.ReadFile(guest)
	if err != nil {
		return err
	}
	if err := os.WriteFile(opts.Out, raw, 0o644); err != nil {
		return err
	}
	if err := verifyPSXEXE(opts.Out); err != nil {
		return err
	}
	if opts.Stdout != nil {
		report.Line(opts.Stdout, report.Wrote, opts.Out)
	}
	return nil
}

func (t *Tool) compileEnv(opts platforms.CommonOptions) (map[string]string, error) {
	env, err := t.Env(opts)
	if err != nil {
		return nil, err
	}
	if compileReady(env) {
		return env, nil
	}
	env, _ = t.discover(opts.Prefix)
	return env, nil
}

func (t *Tool) runEnv(ctx context.Context, name string, args, extraPath []string, extraEnv map[string]string, stdout, stderr io.Writer) error {
	r := t.runner()
	if er, ok := r.(envRunner); ok {
		return er.RunEnv(ctx, name, args, extraPath, extraEnv, writerOrDiscard(stdout), writerOrDiscard(stderr))
	}
	return r.Run(ctx, name, args, writerOrDiscard(stdout), writerOrDiscard(stderr))
}

func (t *Tool) ensureHostBuildTools(ctx context.Context, prefix string, log io.Writer) (cmake, ninja string, err error) {
	plat := cache.PlatformDir(prefix, ID)
	cmake = t.findHostTool(prefix, "cmake", "cmake")
	ninja = t.findHostTool(prefix, "ninja", "ninja")
	if cmake != "" && ninja != "" {
		return cmake, ninja, nil
	}
	for _, a := range hostBuildAssets() {
		if a.ID == "cmake" && cmake != "" {
			continue
		}
		if a.ID == "ninja" && ninja != "" {
			continue
		}
		dest := filepath.Join(plat, a.Dest)
		if compilePiecePresent(dest, a.ID) {
			continue
		}
		report.Linef(log, report.Fetching, "%s  %s", a.ID, a.URL)
		if err := t.fetcher().FetchZip(ctx, a.URL, a.SHA256, dest, log); err != nil {
			return "", "", report.Missing(a.ID+" fetch failed: "+err.Error(), "check network or install cmake/ninja on PATH")
		}
	}
	cmake = t.findHostTool(prefix, "cmake", "cmake")
	ninja = t.findHostTool(prefix, "ninja", "ninja")
	if cmake == "" {
		return "", "", report.Missing("cmake not found", "install CMake 3.21+ or run blazium-toolchain ps1 setup --profile compile")
	}
	if ninja == "" {
		return "", "", report.Missing("ninja not found", "install Ninja or run blazium-toolchain ps1 setup --profile compile")
	}
	return cmake, ninja, nil
}

func (t *Tool) findHostTool(prefix, id, command string) string {
	plat := cache.PlatformDir(prefix, ID)
	if p := walkNamed(filepath.Join(plat, id), hostNames(command)...); p != "" {
		return p
	}
	if p, err := t.runner().LookPath(command); err == nil {
		return p
	}
	return lookFile(command)
}

// buildType is the CMake configuration. PSn00bSDK's Debug adds -Og after a
// project's own flags and links the debug libraries; the GDIR guest (a
// script interpreter and a renderer on a 33 MHz CPU) builds as Release
// (-O2). BLAZIUM_PS1_BUILD_TYPE overrides either.
func buildType(sample string) string {
	if v := strings.TrimSpace(os.Getenv("BLAZIUM_PS1_BUILD_TYPE")); v != "" {
		return v
	}
	if sample == "gdir" {
		return "Release"
	}
	return "Debug"
}

func resolveSample(prefix, sample string) (string, error) {
	plat := cache.PlatformDir(prefix, ID)
	var marker string
	switch sample {
	case "template":
		marker = filepath.Join("share", "psn00bsdk", "template", "CMakeLists.txt")
	case "gte":
		marker = filepath.Join("share", "psn00bsdk", "examples", "graphics", "gte", "CMakeLists.txt")
	default:
		return "", report.Usage("--sample must be template, gte or gdir")
	}
	if hit := findRelFile(plat, marker); hit != "" {
		return filepath.Dir(hit), nil
	}
	return "", report.Missing("official "+sample+" sample not in the SDK cache", "blazium-toolchain ps1 setup --profile compile")
}

func findRelFile(root, rel string) string {
	if root == "" {
		return ""
	}
	var found string
	_ = filepath.WalkDir(root, func(path string, d os.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return nil
		}
		if strings.HasSuffix(filepath.ToSlash(path), filepath.ToSlash(rel)) {
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

func cmakeTarget(sample, src string) string {
	if sample == "gte" || strings.EqualFold(filepath.Base(src), "gte") {
		return "gte"
	}
	if sample == "template" || strings.EqualFold(filepath.Base(src), "template") {
		return "template"
	}
	if raw, err := os.ReadFile(filepath.Join(src, "CMakeLists.txt")); err == nil {
		text := string(raw)
		if strings.Contains(text, "psn00bsdk_add_executable(runtime") {
			return "runtime"
		}
	}
	base := filepath.Base(src)
	if base == "" || base == "." || base == string(filepath.Separator) {
		return "template"
	}
	return base
}

func findPSXEXE(buildDir, target string) (string, error) {
	prefer := hostNames(target + ".exe")
	if p := walkNamed(buildDir, prefer...); p != "" && verifyPSXEXE(p) == nil {
		return p, nil
	}
	var found string
	_ = filepath.WalkDir(buildDir, func(path string, d os.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return nil
		}
		name := strings.ToLower(d.Name())
		if !strings.HasSuffix(name, ".exe") && !strings.HasSuffix(name, ".elf") {
			return nil
		}
		if verifyPSXEXE(path) == nil {
			found = path
			return filepath.SkipAll
		}
		return nil
	})
	if found == "" {
		return "", report.Fail(fmt.Sprintf("no PS-X EXE in %s after cmake --target %s", buildDir, target), "check the guest CMakeLists")
	}
	return found, nil
}

func verifyPSXEXE(path string) error {
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	buf := make([]byte, 8)
	n, err := f.Read(buf)
	if err != nil && err != io.EOF {
		return err
	}
	if !bytes.Equal(buf[:n], []byte(psxMagic)) {
		return fmt.Errorf("%s is not a PS-X EXE (got %q)", path, buf[:n])
	}
	return nil
}
