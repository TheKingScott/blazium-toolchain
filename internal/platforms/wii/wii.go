// Package wii is the Nintendo Wii platform: the GDIR guest (guest/wii)
// with a PS2 GDIR export, whose packs the Wii reads as they are, made into
// a Homebrew Channel app (apps/NAME/: boot.dol, meta.xml, icon.png and the
// game's DATA/) and the same as a .zip to unpack onto an SD card, for a Wii
// with the Homebrew Channel or for Dolphin.
//
// A build needs no Wii SDK: the guest comes compiled with this CLI
// (guest/wii/prebuilt: boot.dol, built with devkitPPC, libogc and libfat),
// and the CLI writes the app itself (hbc.go), so wii builds on Windows,
// Linux and macOS alike. Only a guest with code of your own (--overlay,
// GDIR_EXTRA_FLAGS) is compiled, with devkitPPC (DEVKITPPC).
package wii

import (
	"context"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"time"

	guest "github.com/blazium-games/blazium-toolchain/guest/wii"
	"github.com/blazium-games/blazium-toolchain/internal/cache"
	"github.com/blazium-games/blazium-toolchain/internal/execx"
	"github.com/blazium-games/blazium-toolchain/internal/platforms"
	"github.com/blazium-games/blazium-toolchain/internal/report"
)

const ID = "wii"

// Tool is the Wii implementation of platforms.Platform.
type Tool struct {
	Runner execx.Runner
}

func New() *Tool {
	return &Tool{Runner: execx.Host{}}
}

func (t *Tool) runner() execx.Runner {
	if t.Runner == nil {
		return execx.Host{}
	}
	return t.Runner
}

func (t *Tool) Info() platforms.Info {
	return platforms.Info{
		ID:       ID,
		Name:     "Nintendo Wii",
		Status:   platforms.StatusSupported,
		Commands: []string{"setup", "env", "status", "build", "run"},
		Description: "Nintendo Wii (homebrew: the Homebrew Channel, or Dolphin). build --sample gdir makes a PS2 export " +
			"into a Homebrew Channel app (apps/NAME and a .zip for the SD card) with the bundled guest, on Windows, Linux and macOS, no SDK needed. Run: Dolphin.",
	}
}

// envKeys are what discover finds and Env reports.
var envKeys = []string{"DEVKITPRO", "DEVKITPPC", "GC_MAKE", "ELF2DOL", "DOLPHIN_EXE"}

func exeName(n string) string {
	if runtime.GOOS == "windows" {
		return n + ".exe"
	}
	return n
}

func (t *Tool) discover() map[string]string {
	env := map[string]string{}
	for _, k := range envKeys {
		if v := strings.TrimSpace(os.Getenv(k)); v != "" {
			env[k] = v
		}
	}
	if env["DEVKITPRO"] == "" {
		for _, c := range []string{"/opt/devkitpro", `C:\devkitPro`} {
			if st, err := os.Stat(c); err == nil && st.IsDir() {
				env["DEVKITPRO"] = c
				break
			}
		}
	}
	if env["DEVKITPPC"] == "" && env["DEVKITPRO"] != "" {
		env["DEVKITPPC"] = filepath.Join(env["DEVKITPRO"], "devkitPPC")
	}
	if env["ELF2DOL"] == "" {
		if env["DEVKITPRO"] != "" && fileExists(filepath.Join(env["DEVKITPRO"], "tools", "bin", exeName("elf2dol"))) {
			env["ELF2DOL"] = filepath.Join(env["DEVKITPRO"], "tools", "bin", exeName("elf2dol"))
		} else if p, err := exec.LookPath("elf2dol"); err == nil {
			env["ELF2DOL"] = p
		}
	}
	if env["GC_MAKE"] == "" {
		if p, err := exec.LookPath("make"); err == nil {
			env["GC_MAKE"] = p
		}
	}
	if env["DOLPHIN_EXE"] == "" {
		for _, n := range []string{"dolphin-emu", "Dolphin", "dolphin-emu-nogui", "DolphinNoGUI"} {
			if p, err := exec.LookPath(n); err == nil {
				env["DOLPHIN_EXE"] = p
				break
			}
		}
	}
	return env
}

// libogc is where the libogc headers and cube libraries are.
func libogc(env map[string]string) string {
	if env["DEVKITPRO"] != "" {
		return filepath.Join(env["DEVKITPRO"], "libogc")
	}
	if env["DEVKITPPC"] != "" {
		return filepath.Join(filepath.Dir(env["DEVKITPPC"]), "libogc")
	}
	return ""
}

// compileReady: devkitPPC and libogc are there to compile a guest of your own.
func compileReady(env map[string]string) bool {
	ppc := env["DEVKITPPC"]
	if ppc == "" {
		return false
	}
	return fileExists(filepath.Join(ppc, "bin", exeName("powerpc-eabi-g++"))) &&
		fileExists(filepath.Join(libogc(env), "include", "gccore.h")) &&
		fileExists(filepath.Join(libogc(env), "lib", "wii", "libogc.a")) && env["ELF2DOL"] != ""
}

func (t *Tool) Setup(ctx context.Context, opts platforms.SetupOptions) error {
	_ = ctx
	env := t.discover()
	profile := opts.Profile
	if profile == "" {
		profile = "prebuilt"
	}
	log := opts.Stdout
	switch profile {
	case "prebuilt", "dev":
	case "compile":
		if !compileReady(env) {
			return report.Missing("devkitPPC and libogc (only a guest with code of your own needs them)",
				"install devkitPro's wii-dev and libfat (devkitpro.org/wiki/Getting_Started) and set DEVKITPRO, or leave out --overlay")
		}
	default:
		return report.Usage("wii setup --profile prebuilt|compile")
	}
	if err := guest.InstallGDIR(GDIRGuestDir(opts.Prefix)); err != nil {
		return err
	}
	st := cache.State{Platform: ID, Profile: profile, Env: env}
	if err := cache.WriteState(opts.Prefix, ID, st); err != nil {
		return err
	}
	report.Setup(log, ID, profile, cache.PlatformDir(opts.Prefix, ID), env)
	if env["DOLPHIN_EXE"] == "" {
		report.Line(log, report.Note, "no DOLPHIN_EXE: wii run will not find an emulator")
	}
	return nil
}

func (t *Tool) Env(opts platforms.CommonOptions) (platforms.EnvMap, error) {
	fresh := t.discover()
	st, err := cache.ReadState(opts.Prefix, ID)
	if err != nil || st.Env == nil {
		return fresh, nil
	}
	for _, k := range envKeys {
		if fresh[k] != "" {
			st.Env[k] = fresh[k]
		}
	}
	return st.Env, nil
}

func (t *Tool) Status(opts platforms.CommonOptions) (map[string]any, error) {
	env, err := t.Env(opts)
	if err != nil {
		return nil, err
	}
	return map[string]any{
		"platform":      ID,
		"env":           env,
		"compile_ready": true, // the bundled guest needs no SDK
		"custom_guest":  compileReady(env),
		"run_ready":     fileExists(env["DOLPHIN_EXE"]),
		"ready":         true,
		"product":       "hbc",
		"guest_dir":     GDIRGuestDir(opts.Prefix),
		"host_os":       runtime.GOOS,
	}, nil
}

// GDIRGuestDir is <prefix>/wii/guest/gdir: the GDIR guest, staged per build.
func GDIRGuestDir(prefix string) string {
	return filepath.Join(cache.PlatformDir(prefix, ID), "guest", "gdir")
}

// outputs: --out GAME.zip is the zip, with the app folder apps/NAME/
// beside it; --out DIR is DIR/apps/NAME/ and DIR/NAME.zip.
func outputs(out, name string) (zipPath, root string) {
	if strings.EqualFold(filepath.Ext(out), ".zip") {
		return out, filepath.Dir(out)
	}
	return filepath.Join(out, name+".zip"), out
}

func (t *Tool) Build(ctx context.Context, opts platforms.BuildOptions) error {
	if strings.ToLower(strings.TrimSpace(opts.Sample)) != "gdir" {
		return report.Usage("wii build needs --sample gdir (the GDIR guest with a PS2 export)")
	}
	if opts.Out == "" {
		return report.Usage("build requires --out GAME.zip (or a folder)")
	}
	title := strings.TrimSpace(opts.Title)
	if title == "" {
		title = strings.TrimSuffix(filepath.Base(opts.Out), filepath.Ext(opts.Out))
	}
	name := strings.TrimSpace(opts.TitleID)
	if name == "" {
		name = appName(strings.TrimSuffix(filepath.Base(opts.Out), filepath.Ext(opts.Out)))
	}
	if !appNameRe.MatchString(name) {
		return report.Usage("--title-id is the app's folder name on the SD card (apps/NAME): lower-case letters, digits, - and _, up to 40")
	}
	export, err := gdirExportDir(opts.PackDir, opts.Out)
	if err != nil {
		return err
	}
	files, err := gdirFiles(export, opts.Stdout)
	if err != nil {
		return err
	}
	dol, err := t.guestDOL(ctx, opts)
	if err != nil {
		return err
	}
	if dol, err = PatchAppDir(dol, name); err != nil {
		return report.Fail("boot.dol: "+err.Error(), "")
	}
	icon := strings.TrimSpace(opts.Icon)
	if icon == "" {
		icon = exportIcon(export)
	}
	iconPNG, err := Icon(icon)
	if err != nil {
		return report.Fail("icon: "+err.Error(), "--icon is a PNG or JPEG (128x48 shows best)")
	}
	now := time.Now()

	zipPath, root := outputs(opts.Out, name)
	app := filepath.Join(root, "apps", name)
	// a new DATA, the rest written over: saves (BLAZIUM.SAV) stay
	if err := os.RemoveAll(filepath.Join(app, "DATA")); err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Join(app, "DATA"), 0o755); err != nil {
		return err
	}
	write := func(rel string, data []byte) error {
		return os.WriteFile(filepath.Join(app, filepath.FromSlash(rel)), data, 0o644)
	}
	if err := write("boot.dol", dol); err != nil {
		return err
	}
	if err := write("meta.xml", MetaXML(title, now)); err != nil {
		return err
	}
	if err := write("icon.png", iconPNG); err != nil {
		return err
	}
	for _, f := range files {
		raw, err := os.ReadFile(f.Src)
		if err != nil {
			return err
		}
		if err := write(f.Path, raw); err != nil {
			return err
		}
	}
	notices, err := buildNotices(zipPath)
	if err != nil {
		return report.Fail("third-party notices: "+err.Error(), "")
	}
	if err := write("THIRD-PARTY-NOTICES.txt", notices); err != nil {
		return err
	}
	// the zip holds apps/NAME/ only, whatever else is in root
	stage, err := os.MkdirTemp(filepath.Dir(zipPath), ".wii-zip-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(stage)
	if err := copyTree(app, filepath.Join(stage, "apps", name)); err != nil {
		return err
	}
	// the zip is the game as it ships: no save
	_ = os.Remove(filepath.Join(stage, "apps", name, "BLAZIUM.SAV"))
	if err := writeZip(stage, zipPath, now); err != nil {
		return report.Fail("zip: "+err.Error(), "")
	}
	if opts.Stdout != nil {
		report.Line(opts.Stdout, report.Wrote, zipPath)
		report.Line(opts.Stdout, report.Wrote, app)
	}
	np := noticesPath(zipPath)
	if err := os.WriteFile(np, notices, 0o644); err != nil {
		return err
	}
	if opts.Stdout != nil {
		report.Line(opts.Stdout, report.Wrote, np)
	}
	return nil
}

// guestDOL: the bundled guest, unless an overlay or GDIR_EXTRA_FLAGS asks
// for one compiled here (with devkitPPC).
func (t *Tool) guestDOL(ctx context.Context, opts platforms.BuildOptions) ([]byte, error) {
	ov := strings.TrimSpace(opts.Overlay)
	flags := os.Getenv("GDIR_EXTRA_FLAGS")
	if ov == "" && flags == "" {
		if opts.Stdout != nil {
			report.Line(opts.Stdout, report.Reusing, "the bundled Wii guest ("+guest.PrebuiltVersion()+")")
		}
		return guest.PrebuiltDOL()
	}
	env, _ := t.Env(opts.CommonOptions)
	if !compileReady(env) {
		return nil, report.Missing("devkitPPC and libogc, to compile the guest with --overlay or GDIR_EXTRA_FLAGS",
			"install devkitPro's wii-dev and libfat and set DEVKITPRO, or build without --overlay")
	}
	src := GDIRGuestDir(opts.Prefix)
	if err := os.RemoveAll(src); err != nil {
		return nil, err
	}
	if err := guest.InstallGDIR(src); err != nil {
		return nil, err
	}
	if ov != "" {
		if err := guest.Overlay(src, ov); err != nil {
			return nil, err
		}
	}
	makeProg := env["GC_MAKE"]
	if makeProg == "" {
		makeProg = "make"
	}
	extra := map[string]string{"DEVKITPPC": env["DEVKITPPC"], "LIBOGC": libogc(env), "ELF2DOL": env["ELF2DOL"]}
	if env["DEVKITPRO"] != "" {
		extra["DEVKITPRO"] = env["DEVKITPRO"]
	}
	args := []string{"-C", src, "ELF2DOL=" + env["ELF2DOL"]}
	if flags != "" {
		args = append(args, "GDIR_EXTRA_FLAGS="+flags)
	}
	if err := t.runEnv(ctx, makeProg, args, extra, opts.Stdout, opts.Stderr); err != nil {
		return nil, report.Fail("make: "+err.Error(), "")
	}
	raw, err := os.ReadFile(filepath.Join(src, "boot.dol"))
	if err != nil {
		return nil, report.Fail("no boot.dol was made", "")
	}
	return raw, nil
}

func (t *Tool) runEnv(ctx context.Context, name string, args []string, extraEnv map[string]string, stdout, stderr io.Writer) error {
	type envRunner interface {
		RunEnv(ctx context.Context, name string, args []string, extraPath []string, extraEnv map[string]string, stdout, stderr io.Writer) error
	}
	if er, ok := t.runner().(envRunner); ok {
		return er.RunEnv(ctx, name, args, nil, extraEnv, writerOr(stdout), writerOr(stderr))
	}
	return t.runner().Run(ctx, name, args, writerOr(stdout), writerOr(stderr))
}

func writerOr(w io.Writer) io.Writer {
	if w == nil {
		return io.Discard
	}
	return w
}

func (t *Tool) Run(ctx context.Context, opts platforms.RunOptions) error {
	env, _ := t.Env(opts.CommonOptions)
	game := strings.TrimSpace(opts.Exe)
	if game == "" {
		game = strings.TrimSpace(opts.ISO)
	}
	if game == "" {
		return report.Usage("wii run needs the app: its boot.dol, its apps/NAME folder, or the folder with apps/ in it")
	}
	emu := env["DOLPHIN_EXE"]
	if emu == "" {
		return report.Missing("no Dolphin (DOLPHIN_EXE)", "install Dolphin (dolphin-emu.org) and set DOLPHIN_EXE")
	}
	dol, sdRoot, err := runPaths(game)
	if err != nil {
		return err
	}
	// Dolphin's virtual SD card, filled from the folder with apps/ in it
	args := []string{"-C", "Dolphin.Core.WiiSDCard=True", "-C", "Dolphin.Core.WiiSDCardEnableFolderSync=True",
		"-C", "Dolphin.General.WiiSDCardSyncFolder=" + sdRoot, "-b", "-e", dol}
	if strings.Contains(strings.ToLower(filepath.Base(emu)), "nogui") {
		args = args[:len(args)-3]
		args = append(args, "-e", dol)
	}
	timeout := opts.Timeout
	if timeout <= 0 {
		timeout = 120 * time.Second
	}
	cctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	err = t.runner().Run(cctx, emu, args, writerOr(opts.Stdout), writerOr(opts.Stderr))
	if cctx.Err() == context.DeadlineExceeded {
		return nil // the smoke run lasted its time
	}
	if err != nil {
		return report.Fail(fmt.Sprintf("%s: %v", filepath.Base(emu), err), "")
	}
	return nil
}

// runPaths: boot.dol and the folder holding apps/ (the SD card's root) for
// a game given as its boot.dol, its apps/NAME folder or that root.
func runPaths(game string) (dol, sdRoot string, err error) {
	abs, err := filepath.Abs(game)
	if err != nil {
		return "", "", err
	}
	st, err := os.Stat(abs)
	if err != nil {
		return "", "", report.Missing("no "+game, "wii build --sample gdir writes apps/NAME/boot.dol")
	}
	app := abs
	if !st.IsDir() {
		app = filepath.Dir(abs)
	} else if !fileExists(filepath.Join(abs, "boot.dol")) {
		ents, _ := os.ReadDir(filepath.Join(abs, "apps"))
		app = ""
		for _, e := range ents {
			if e.IsDir() && fileExists(filepath.Join(abs, "apps", e.Name(), "boot.dol")) {
				app = filepath.Join(abs, "apps", e.Name())
				break
			}
		}
		if app == "" {
			return "", "", report.Missing("no apps/NAME/boot.dol in "+game, "wii build --sample gdir writes it")
		}
	}
	return filepath.Join(app, "boot.dol"), filepath.Dir(filepath.Dir(app)), nil
}

func (t *Tool) ISO(ctx context.Context, opts platforms.ISOOptions) error {
	_ = ctx
	_ = opts
	return report.Usage("wii has no ISO; wii build --sample gdir writes the Homebrew Channel app and its .zip")
}

func copyTree(from, to string) error {
	return filepath.WalkDir(from, func(path string, d os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(from, path)
		if err != nil {
			return err
		}
		target := filepath.Join(to, rel)
		if d.IsDir() {
			return os.MkdirAll(target, 0o755)
		}
		raw, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		return os.WriteFile(target, raw, 0o644)
	})
}

func fileExists(p string) bool {
	if p == "" {
		return false
	}
	st, err := os.Stat(p)
	return err == nil && !st.IsDir()
}
