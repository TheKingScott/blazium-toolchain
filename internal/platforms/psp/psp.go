// Package psp is the PlayStation Portable platform: the GDIR guest
// (guest/psp) with a PS2 GDIR export, whose packs the PSP reads as they
// are, made into a homebrew game folder (PSP/GAME/NAME/: EBOOT.PBP and the
// game's DATA/) and the same as a .zip to unpack onto a Memory Stick, for a
// PSP with custom firmware or for PPSSPP.
//
// A build needs no PSP SDK: the guest comes compiled with this CLI
// (guest/psp/prebuilt/gdir.prx, built with pspdev and PSPSDK), and the CLI
// writes PARAM.SFO and EBOOT.PBP itself (pbp.go), so psp builds on Windows,
// Linux and macOS alike. Only a guest with code of your own (--overlay,
// GDIR_EXTRA_FLAGS) is compiled, with pspdev (PSPDEV).
package psp

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

	guest "github.com/blazium-games/blazium-toolchain/guest/psp"
	"github.com/blazium-games/blazium-toolchain/internal/cache"
	"github.com/blazium-games/blazium-toolchain/internal/execx"
	"github.com/blazium-games/blazium-toolchain/internal/platforms"
	"github.com/blazium-games/blazium-toolchain/internal/report"
)

const ID = "psp"

// Tool is the PSP implementation of platforms.Platform.
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
		Name:     "PlayStation Portable",
		Status:   platforms.StatusSupported,
		Commands: []string{"setup", "env", "status", "build", "run"},
		Description: "PlayStation Portable (homebrew: a PSP with custom firmware, or PPSSPP). build --sample gdir makes a PS2 export " +
			"into a game folder (PSP/GAME/NAME with EBOOT.PBP, and a .zip for the Memory Stick) with the bundled guest, on Windows, Linux and macOS, no SDK needed. Run: PPSSPP.",
	}
}

// envKeys are what discover finds and Env reports.
var envKeys = []string{"PSPDEV", "PSP_MAKE", "PPSSPP_EXE"}

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
	if env["PSPDEV"] == "" {
		for _, c := range []string{"/usr/local/pspdev", "/opt/pspdev"} {
			if fileExists(filepath.Join(c, "bin", "psp-gcc")) {
				env["PSPDEV"] = c
				break
			}
		}
	}
	if env["PSP_MAKE"] == "" {
		if p, err := exec.LookPath("make"); err == nil {
			env["PSP_MAKE"] = p
		}
	}
	if env["PPSSPP_EXE"] == "" {
		for _, n := range []string{"PPSSPPSDL", "ppsspp", "PPSSPPWindows64", "PPSSPPWindows", "PPSSPPQt"} {
			if p, err := exec.LookPath(n); err == nil {
				env["PPSSPP_EXE"] = p
				break
			}
		}
	}
	return env
}

// compileReady: pspdev (the toolchain and PSPSDK) is there to compile a
// guest of your own.
func compileReady(env map[string]string) bool {
	d := env["PSPDEV"]
	if d == "" {
		return false
	}
	return fileExists(filepath.Join(d, "bin", exeName("psp-g++"))) &&
		fileExists(filepath.Join(d, "psp", "sdk", "include", "pspgu.h")) &&
		fileExists(filepath.Join(d, "bin", exeName("psp-prxgen")))
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
			return report.Missing("pspdev (only a guest with code of your own needs it)",
				"install pspdev (github.com/pspdev/pspdev) and set PSPDEV, or leave out --overlay")
		}
	default:
		return report.Usage("psp setup --profile prebuilt|compile")
	}
	if err := guest.InstallGDIR(GDIRGuestDir(opts.Prefix)); err != nil {
		return err
	}
	st := cache.State{Platform: ID, Profile: profile, Env: env}
	if err := cache.WriteState(opts.Prefix, ID, st); err != nil {
		return err
	}
	report.Setup(log, ID, profile, cache.PlatformDir(opts.Prefix, ID), env)
	if env["PPSSPP_EXE"] == "" {
		report.Line(log, report.Note, "no PPSSPP_EXE: psp run will not find an emulator")
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
		"run_ready":     fileExists(env["PPSSPP_EXE"]),
		"ready":         true,
		"product":       "pbp",
		"guest_dir":     GDIRGuestDir(opts.Prefix),
		"host_os":       runtime.GOOS,
	}, nil
}

// GDIRGuestDir is <prefix>/psp/guest/gdir: the GDIR guest, staged per build.
func GDIRGuestDir(prefix string) string {
	return filepath.Join(cache.PlatformDir(prefix, ID), "guest", "gdir")
}

// outputs: --out GAME.zip is the zip, with the game folder PSP/GAME/NAME/
// beside it; --out DIR is DIR/PSP/GAME/NAME/ and DIR/NAME.zip.
func outputs(out, name string) (zipPath, root string) {
	if strings.EqualFold(filepath.Ext(out), ".zip") {
		return out, filepath.Dir(out)
	}
	return filepath.Join(out, name+".zip"), out
}

func (t *Tool) Build(ctx context.Context, opts platforms.BuildOptions) error {
	if strings.ToLower(strings.TrimSpace(opts.Sample)) != "gdir" {
		return report.Usage("psp build needs --sample gdir (the GDIR guest with a PS2 export)")
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
		name = gameName(strings.TrimSuffix(filepath.Base(opts.Out), filepath.Ext(opts.Out)))
	}
	if !gameNameRe.MatchString(name) {
		return report.Usage("--title-id is the game's folder on the Memory Stick (PSP/GAME/NAME): upper-case letters, digits, - and _, up to 32")
	}
	export, err := gdirExportDir(opts.PackDir, opts.Out)
	if err != nil {
		return err
	}
	files, err := gdirFiles(export, opts.Stdout)
	if err != nil {
		return err
	}
	prx, err := t.guestPRX(ctx, opts)
	if err != nil {
		return err
	}
	if prx, err = PatchGameDir(prx, name); err != nil {
		return report.Fail("EBOOT: "+err.Error(), "")
	}
	icon := strings.TrimSpace(opts.Icon)
	if icon == "" {
		icon = exportIcon(export)
	}
	iconPNG, err := Icon(icon)
	if err != nil {
		return report.Fail("icon: "+err.Error(), "--icon is a PNG or JPEG (144x80 shows best)")
	}
	eboot := EbootPBP(ParamSFO(title), iconPNG, prx)
	now := time.Now()

	zipPath, root := outputs(opts.Out, name)
	game := filepath.Join(root, "PSP", "GAME", name)
	// a new DATA, the rest written over: saves (BLAZIUM.SAV) stay
	if err := os.RemoveAll(filepath.Join(game, "DATA")); err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Join(game, "DATA"), 0o755); err != nil {
		return err
	}
	write := func(rel string, data []byte) error {
		return os.WriteFile(filepath.Join(game, filepath.FromSlash(rel)), data, 0o644)
	}
	if err := write("EBOOT.PBP", eboot); err != nil {
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
	// the zip holds PSP/GAME/NAME/ only, whatever else is in root, and no save
	stage, err := os.MkdirTemp(filepath.Dir(zipPath), ".psp-zip-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(stage)
	if err := copyTree(game, filepath.Join(stage, "PSP", "GAME", name)); err != nil {
		return err
	}
	_ = os.Remove(filepath.Join(stage, "PSP", "GAME", name, "BLAZIUM.SAV"))
	if err := writeZip(stage, zipPath, now); err != nil {
		return report.Fail("zip: "+err.Error(), "")
	}
	if opts.Stdout != nil {
		report.Line(opts.Stdout, report.Wrote, zipPath)
		report.Line(opts.Stdout, report.Wrote, game)
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

// guestPRX: the bundled guest, unless an overlay or GDIR_EXTRA_FLAGS asks
// for one compiled here (with pspdev).
func (t *Tool) guestPRX(ctx context.Context, opts platforms.BuildOptions) ([]byte, error) {
	ov := strings.TrimSpace(opts.Overlay)
	flags := os.Getenv("GDIR_EXTRA_FLAGS")
	if ov == "" && flags == "" {
		if opts.Stdout != nil {
			report.Line(opts.Stdout, report.Reusing, "the bundled PSP guest ("+guest.PrebuiltVersion()+")")
		}
		return guest.PrebuiltPRX()
	}
	env, _ := t.Env(opts.CommonOptions)
	if !compileReady(env) {
		return nil, report.Missing("pspdev, to compile the guest with --overlay or GDIR_EXTRA_FLAGS",
			"install pspdev and set PSPDEV, or build without --overlay")
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
	makeProg := env["PSP_MAKE"]
	if makeProg == "" {
		makeProg = "make"
	}
	extra := map[string]string{"PSPDEV": env["PSPDEV"]}
	args := []string{"-C", src}
	if flags != "" {
		args = append(args, "GDIR_EXTRA_FLAGS="+flags)
	}
	if err := t.runEnv(ctx, makeProg, args, extra, opts.Stdout, opts.Stderr); err != nil {
		return nil, report.Fail("make: "+err.Error(), "")
	}
	raw, err := os.ReadFile(filepath.Join(src, "gdir.prx"))
	if err != nil {
		return nil, report.Fail("no gdir.prx was made", "")
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
		return report.Usage("psp run needs the game: its EBOOT.PBP or its PSP/GAME/NAME folder")
	}
	emu := env["PPSSPP_EXE"]
	if emu == "" {
		return report.Missing("no PPSSPP (PPSSPP_EXE)", "install PPSSPP (ppsspp.org) and set PPSSPP_EXE")
	}
	if st, err := os.Stat(game); err == nil && st.IsDir() {
		game = filepath.Join(game, "EBOOT.PBP")
	}
	if !fileExists(game) {
		return report.Missing("no "+game, "psp build --sample gdir writes PSP/GAME/NAME/EBOOT.PBP")
	}
	timeout := opts.Timeout
	if timeout <= 0 {
		timeout = 120 * time.Second
	}
	cctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	err := t.runner().Run(cctx, emu, []string{game}, writerOr(opts.Stdout), writerOr(opts.Stderr))
	if cctx.Err() == context.DeadlineExceeded {
		return nil // the smoke run lasted its time
	}
	if err != nil {
		return report.Fail(fmt.Sprintf("%s: %v", filepath.Base(emu), err), "")
	}
	return nil
}

func (t *Tool) ISO(ctx context.Context, opts platforms.ISOOptions) error {
	_ = ctx
	_ = opts
	return report.Usage("psp has no ISO here; psp build --sample gdir writes the game folder (EBOOT.PBP) and its .zip")
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
