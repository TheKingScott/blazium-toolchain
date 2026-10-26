// Package gamecube is the Nintendo GameCube platform: the GDIR guest
// (guest/gamecube) with a PS2 GDIR export, whose packs the GameCube reads
// as they are, made into a GameCube disc image (.iso, GCM) for Dolphin,
// Swiss or a console with a disc loader.
//
// A build needs no GameCube SDK: the guest comes compiled with this CLI
// (guest/gamecube/prebuilt: gdir.dol and the disc's apploader, built with
// devkitPPC and libogc), and the CLI itself writes the disc image (gcm.go)
// and its banner (banner.go), so gamecube builds on Windows, Linux and
// macOS alike. Only a guest with code of your own (--overlay,
// GDIR_EXTRA_FLAGS) is compiled, with devkitPPC (DEVKITPPC).
package gamecube

import (
	"context"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strings"
	"time"

	guest "github.com/blazium-games/blazium-toolchain/guest/gamecube"
	"github.com/blazium-games/blazium-toolchain/internal/cache"
	"github.com/blazium-games/blazium-toolchain/internal/execx"
	"github.com/blazium-games/blazium-toolchain/internal/platforms"
	"github.com/blazium-games/blazium-toolchain/internal/report"
)

const ID = "gamecube"

// DefaultGameID is the disc's game ID without --title-id: game code GBZE
// (an American disc) and maker BZ.
const DefaultGameID = "GBZEBZ"

// Tool is the GameCube implementation of platforms.Platform.
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
		Name:     "Nintendo GameCube",
		Status:   platforms.StatusSupported,
		Commands: []string{"setup", "env", "status", "build", "run"},
		Description: "Nintendo GameCube (homebrew: Dolphin, Swiss, or a console with a disc loader). build --sample gdir makes a PS2 export " +
			"into a GameCube disc image (.iso) with the bundled guest, on Windows, Linux and macOS, no SDK needed. Run: Dolphin.",
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
		fileExists(filepath.Join(libogc(env), "lib", "cube", "libogc.a")) && env["ELF2DOL"] != ""
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
				"install devkitPro's gamecube-dev (devkitpro.org/wiki/Getting_Started) and set DEVKITPRO, or leave out --overlay")
		}
	default:
		return report.Usage("gamecube setup --profile prebuilt|compile")
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
		report.Line(log, report.Note, "no DOLPHIN_EXE: gamecube run will not find an emulator")
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
		"product":       "iso",
		"guest_dir":     GDIRGuestDir(opts.Prefix),
		"host_os":       runtime.GOOS,
	}, nil
}

// GDIRGuestDir is <prefix>/gamecube/guest/gdir: the GDIR guest, staged per build.
func GDIRGuestDir(prefix string) string {
	return filepath.Join(cache.PlatformDir(prefix, ID), "guest", "gdir")
}

var gameIDRe = regexp.MustCompile(`^[A-Z0-9]{6}$`)

// isoOut: --out GAME.iso (or .gcm) is the image; --out DIR is DIR/GAMEID.iso.
func isoOut(out, gameID string) string {
	switch strings.ToLower(filepath.Ext(out)) {
	case ".iso", ".gcm":
		return out
	}
	return filepath.Join(out, gameID+".iso")
}

func (t *Tool) Build(ctx context.Context, opts platforms.BuildOptions) error {
	if strings.ToLower(strings.TrimSpace(opts.Sample)) != "gdir" {
		return report.Usage("gamecube build needs --sample gdir (the GDIR guest with a PS2 export)")
	}
	if opts.Out == "" {
		return report.Usage("build requires --out GAME.iso (or a folder)")
	}
	gameID := strings.ToUpper(strings.TrimSpace(opts.TitleID))
	if gameID == "" {
		gameID = DefaultGameID
	}
	if !gameIDRe.MatchString(gameID) {
		return report.Usage("--title-id is the disc's game ID: six letters or digits, game code then maker (GBZEBZ)")
	}
	title := strings.TrimSpace(opts.Title)
	if title == "" {
		title = strings.TrimSuffix(filepath.Base(opts.Out), filepath.Ext(opts.Out))
	}
	export, err := gdirExportDir(opts.PackDir, opts.Out)
	if err != nil {
		return err
	}
	files, err := gdirFiles(export, opts.Stdout)
	if err != nil {
		return err
	}
	icon := strings.TrimSpace(opts.Icon)
	if icon == "" {
		icon = exportIcon(export)
	}
	bnr, err := Banner(title, "Blazium", title+" (Blazium)", icon)
	if err != nil {
		return report.Fail("banner: "+err.Error(), "--icon is a PNG or JPEG (96x32 shows best)")
	}
	files = append(files, GCMFile{Path: "opening.bnr", Data: bnr})

	dol, err := t.guestDOL(ctx, opts)
	if err != nil {
		return err
	}
	app, err := guest.Apploader()
	if err != nil {
		return err
	}

	out := isoOut(opts.Out, gameID)
	if err := os.MkdirAll(filepath.Dir(out), 0o755); err != nil {
		return err
	}
	f, err := os.Create(out)
	if err != nil {
		return err
	}
	werr := WriteGCM(f, GCM{GameID: gameID, Title: title, DOL: dol, Apploader: app, Files: files})
	if cerr := f.Close(); werr == nil {
		werr = cerr
	}
	if werr != nil {
		_ = os.Remove(out)
		return report.Fail("disc image: "+werr.Error(), "")
	}
	if opts.Stdout != nil {
		report.Line(opts.Stdout, report.Wrote, out)
	}
	notices, err := writeNotices(out)
	if err != nil {
		return report.Fail("third-party notices: "+err.Error(), "")
	}
	if opts.Stdout != nil {
		report.Line(opts.Stdout, report.Wrote, notices)
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
			report.Line(opts.Stdout, report.Reusing, "the bundled GameCube guest ("+guest.PrebuiltVersion()+")")
		}
		return guest.PrebuiltDOL()
	}
	env, _ := t.Env(opts.CommonOptions)
	if !compileReady(env) {
		return nil, report.Missing("devkitPPC and libogc, to compile the guest with --overlay or GDIR_EXTRA_FLAGS",
			"install devkitPro's gamecube-dev and set DEVKITPRO, or build without --overlay")
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
	raw, err := os.ReadFile(filepath.Join(src, "gdir.dol"))
	if err != nil {
		return nil, report.Fail("no gdir.dol was made", "")
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
	game := strings.TrimSpace(opts.ISO)
	if game == "" {
		game = strings.TrimSpace(opts.Exe)
	}
	if game == "" {
		return report.Usage("gamecube run needs the disc image (GAME.iso) or a DOL")
	}
	emu := env["DOLPHIN_EXE"]
	if emu == "" {
		return report.Missing("no Dolphin (DOLPHIN_EXE)", "install Dolphin (dolphin-emu.org) and set DOLPHIN_EXE")
	}
	args := []string{"-b", "-e", game}
	if strings.Contains(strings.ToLower(filepath.Base(emu)), "nogui") {
		args = []string{"-e", game}
	}
	timeout := opts.Timeout
	if timeout <= 0 {
		timeout = 120 * time.Second
	}
	cctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	err := t.runner().Run(cctx, emu, args, writerOr(opts.Stdout), writerOr(opts.Stderr))
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
	return report.Usage("gamecube build --sample gdir writes the disc image; there is no separate iso command")
}

func fileExists(p string) bool {
	if p == "" {
		return false
	}
	st, err := os.Stat(p)
	return err == nil && !st.IsDir()
}
