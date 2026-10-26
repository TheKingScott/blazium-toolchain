// Package ps3 is the PlayStation 3 platform: the GDIR guest (guest/ps3)
// with a PS2 GDIR export, whose packs the PS3 reads as they are, made into
// a hard drive game. The product is a folder named by the title ID
// (PARAM.SFO, ICON0.PNG, USRDIR/EBOOT.BIN and USRDIR/DATA) and the same
// game as an installable .pkg, for a PS3 that runs homebrew or for RPCS3.
//
// A build needs no PS3 SDK: the guest comes compiled with this CLI
// (guest/ps3/prebuilt, built with PSL1GHT), and the CLI itself signs it as
// an NPDRM SELF (self.go) and writes PARAM.SFO and the .pkg (pkg.go), so
// ps3 builds on Windows, Linux and macOS alike. Only a guest with code of
// your own (--overlay, GDIR_EXTRA_FLAGS) is compiled, with ps3dev, which is
// built for Linux: ps3 setup --profile compile fetches it there.
package ps3

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

	guest "github.com/blazium-games/blazium-toolchain/guest/ps3"
	"github.com/blazium-games/blazium-toolchain/internal/cache"
	"github.com/blazium-games/blazium-toolchain/internal/execx"
	"github.com/blazium-games/blazium-toolchain/internal/fetch"
	"github.com/blazium-games/blazium-toolchain/internal/platforms"
	"github.com/blazium-games/blazium-toolchain/internal/report"
)

const ID = "ps3"

// The ps3dev release setup --profile compile fetches (ps3toolchain's CI
// build: GCC 7.2 for the PPU and SPU, PSL1GHT, the portlibs, the host tools).
const (
	ps3devURL    = "https://github.com/ps3dev/ps3toolchain/releases/download/2026-06-09/ps3dev-92534e60-ubuntu-latest.tar.gz"
	ps3devSHA256 = "24a5b1c6d1c5da7a71178c8b4e5c381484fd1ab8a73a89ebfb27264dbb3d4063"
)

// DefaultTitleID is the title ID a build gets without --title-id.
const DefaultTitleID = "BLAZ00001"

// Fetcher downloads and unpacks the ps3dev release (fetch.HTTP).
type Fetcher interface {
	FetchZip(ctx context.Context, url, sha256, destDir string, log io.Writer) error
}

// Tool is the PS3 implementation of platforms.Platform.
type Tool struct {
	Runner  execx.Runner
	Fetcher Fetcher
}

func New() *Tool {
	return &Tool{Runner: execx.Host{}, Fetcher: fetch.HTTP{}}
}

func (t *Tool) runner() execx.Runner {
	if t.Runner == nil {
		return execx.Host{}
	}
	return t.Runner
}

func (t *Tool) fetcher() Fetcher {
	if t.Fetcher == nil {
		return fetch.HTTP{}
	}
	return t.Fetcher
}

func (t *Tool) Info() platforms.Info {
	return platforms.Info{
		ID:       ID,
		Name:     "PlayStation 3",
		Status:   platforms.StatusSupported,
		Commands: []string{"setup", "env", "status", "build", "run"},
		Description: "PlayStation 3 (homebrew: a PS3 with HEN or CFW, or RPCS3). build --sample gdir makes a PS2 export " +
			"into a hard drive game folder and a .pkg with the bundled guest, on Windows, Linux and macOS, no SDK needed. " +
			"Run: RPCS3.",
	}
}

// envKeys are what discover finds and Env reports.
var envKeys = []string{"PS3DEV", "PSL1GHT", "PS3_MAKE", "RPCS3_EXE"}

func (t *Tool) discover(prefix string) map[string]string {
	env := map[string]string{}
	for _, k := range envKeys {
		if v := strings.TrimSpace(os.Getenv(k)); v != "" {
			env[k] = v
		}
	}
	if env["PS3DEV"] == "" {
		for _, c := range []string{
			filepath.Join(cache.InstDir(prefix, ID), "ps3dev"),
			"/usr/local/ps3dev",
			"/opt/ps3dev",
		} {
			if fileExists(filepath.Join(c, "ppu_rules")) {
				env["PS3DEV"] = c
				break
			}
		}
	}
	if env["PSL1GHT"] == "" && env["PS3DEV"] != "" {
		env["PSL1GHT"] = env["PS3DEV"]
	}
	if env["PS3_MAKE"] == "" {
		if p, err := exec.LookPath("make"); err == nil {
			env["PS3_MAKE"] = p
		}
	}
	if env["RPCS3_EXE"] == "" {
		for _, n := range []string{"rpcs3", "rpcs3.exe"} {
			if p, err := exec.LookPath(n); err == nil {
				env["RPCS3_EXE"] = p
				break
			}
		}
	}
	return env
}

// compileReady: ps3dev is there to compile a guest of your own.
func compileReady(env map[string]string) bool {
	ps3dev, psl1ght := env["PS3DEV"], env["PSL1GHT"]
	if ps3dev == "" || psl1ght == "" {
		return false
	}
	gxx := "ppu-g++"
	if runtime.GOOS == "windows" {
		gxx += ".exe"
	}
	return fileExists(filepath.Join(ps3dev, "ppu", "bin", gxx)) && fileExists(filepath.Join(psl1ght, "ppu_rules")) &&
		fileExists(filepath.Join(psl1ght, "ppu", "include", "rsx", "rsx.h"))
}

func (t *Tool) Setup(ctx context.Context, opts platforms.SetupOptions) error {
	env := t.discover(opts.Prefix)
	profile := opts.Profile
	if profile == "" {
		profile = "prebuilt"
	}
	log := opts.Stdout
	switch profile {
	case "prebuilt", "dev":
	case "compile":
		if !compileReady(env) {
			if runtime.GOOS != "linux" {
				return report.Missing("ps3dev (only a guest with code of your own needs it)",
					"ps3dev is built for Linux; build it with github.com/ps3dev/ps3toolchain and set PS3DEV, or leave out --overlay")
			}
			if opts.Offline {
				return report.Missing("ps3dev (PSL1GHT and the PPU GCC)", "run ps3 setup --profile compile without --offline, or set PS3DEV")
			}
			dest := cache.InstDir(opts.Prefix, ID)
			if err := t.fetcher().FetchZip(ctx, ps3devURL, ps3devSHA256, dest, log); err != nil {
				return err
			}
			env["PS3DEV"] = filepath.Join(dest, "ps3dev")
			env["PSL1GHT"] = env["PS3DEV"]
			if !compileReady(env) {
				return report.Fail("the ps3dev release did not unpack as expected in "+dest, "delete "+dest+" and run ps3 setup --profile compile again")
			}
		}
	default:
		return report.Usage("ps3 setup --profile prebuilt|compile|dev")
	}
	if err := guest.InstallGDIR(GDIRGuestDir(opts.Prefix)); err != nil {
		return err
	}
	st := cache.State{Platform: ID, Profile: profile, Env: env}
	if err := cache.WriteState(opts.Prefix, ID, st); err != nil {
		return err
	}
	report.Setup(log, ID, profile, cache.PlatformDir(opts.Prefix, ID), env)
	if env["RPCS3_EXE"] == "" {
		report.Line(log, report.Note, "no RPCS3_EXE: ps3 run will not find an emulator")
	}
	return nil
}

func (t *Tool) Env(opts platforms.CommonOptions) (platforms.EnvMap, error) {
	fresh := t.discover(opts.Prefix)
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
		"run_ready":     fileExists(env["RPCS3_EXE"]),
		"ready":         true,
		"product":       "pkg",
		"guest_dir":     GDIRGuestDir(opts.Prefix),
		"host_os":       runtime.GOOS,
	}, nil
}

// GDIRGuestDir is <prefix>/ps3/guest/gdir: the GDIR guest, staged per build.
func GDIRGuestDir(prefix string) string {
	return filepath.Join(cache.PlatformDir(prefix, ID), "guest", "gdir")
}

var titleIDRe = regexp.MustCompile(`^[A-Z]{4}[0-9]{5}$`)

// contentID: UP0001-TITLEID_00-<16 letters and digits from the title>
func contentID(titleID, title string) string {
	var b strings.Builder
	for _, r := range strings.ToUpper(title) {
		if (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9') {
			b.WriteRune(r)
		}
	}
	label := b.String()
	if len(label) > 16 {
		label = label[:16]
	}
	label += strings.Repeat("0", 16-len(label))
	return "UP0001-" + titleID + "_00-" + label
}

// outputs: --out GAME.pkg is the package, the game folder beside it;
// --out DIR is a folder for both (DIR/TITLEID and DIR/TITLEID.pkg).
func outputs(out, titleID string) (pkg, folder string) {
	if strings.EqualFold(filepath.Ext(out), ".pkg") {
		return out, filepath.Join(filepath.Dir(out), titleID)
	}
	return filepath.Join(out, titleID+".pkg"), filepath.Join(out, titleID)
}

func (t *Tool) Build(ctx context.Context, opts platforms.BuildOptions) error {
	if strings.ToLower(strings.TrimSpace(opts.Sample)) != "gdir" {
		return report.Usage("ps3 build needs --sample gdir (the GDIR guest with a PS2 export)")
	}
	if opts.Out == "" {
		return report.Usage("build requires --out GAME.pkg (or a folder)")
	}
	titleID := strings.ToUpper(strings.TrimSpace(opts.TitleID))
	if titleID == "" {
		titleID = DefaultTitleID
	}
	if !titleIDRe.MatchString(titleID) {
		return report.Usage("--title-id is four letters and five digits (BLAZ00001)")
	}
	title := strings.TrimSpace(opts.Title)
	if title == "" {
		title = strings.TrimSuffix(filepath.Base(opts.Out), filepath.Ext(opts.Out))
	}
	cid := contentID(titleID, title)
	export, err := gdirExportDir(opts.PackDir, opts.Out)
	if err != nil {
		return err
	}
	src := GDIRGuestDir(opts.Prefix)
	if _, err := stageGDIR(src, export, opts.Icon, opts.Stdout); err != nil {
		return err
	}
	pkgDir := filepath.Join(src, "pkg")

	// the guest: the bundled one, or compiled with code of your own
	elf, err := t.guestELF(ctx, opts, src)
	if err != nil {
		return err
	}
	eboot, err := MakeSelfNPDRM(elf, cid, "EBOOT.BIN")
	if err != nil {
		return report.Fail("EBOOT.BIN: "+err.Error(), "")
	}
	if err := os.WriteFile(filepath.Join(pkgDir, "USRDIR", "EBOOT.BIN"), eboot, 0o644); err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(pkgDir, "PARAM.SFO"), paramSFO(title, titleID, "01.00"), 0o644); err != nil {
		return err
	}
	icon := filepath.Join(pkgDir, "ICON0.PNG")
	if !fileExists(icon) {
		raw, err := guest.DefaultIcon()
		if err != nil {
			return err
		}
		if err := os.WriteFile(icon, raw, 0o644); err != nil {
			return err
		}
	}
	pkg, err := makePkg(pkgDir, cid)
	if err != nil {
		return report.Fail("pkg: "+err.Error(), "")
	}

	pkgOut, folder := outputs(opts.Out, titleID)
	if err := os.MkdirAll(filepath.Dir(pkgOut), 0o755); err != nil {
		return err
	}
	if err := os.WriteFile(pkgOut, pkg, 0o644); err != nil {
		return err
	}
	if err := os.RemoveAll(folder); err != nil {
		return err
	}
	if err := copyTree(pkgDir, folder); err != nil {
		return err
	}
	if opts.Stdout != nil {
		report.Line(opts.Stdout, report.Wrote, pkgOut)
		report.Line(opts.Stdout, report.Wrote, folder)
	}
	notices, err := writeNotices(pkgOut)
	if err != nil {
		return report.Fail("third-party notices: "+err.Error(), "")
	}
	if opts.Stdout != nil {
		report.Line(opts.Stdout, report.Wrote, notices)
	}
	return nil
}

// guestELF: the bundled guest, unless an overlay or GDIR_EXTRA_FLAGS asks
// for one compiled here (with ps3dev).
func (t *Tool) guestELF(ctx context.Context, opts platforms.BuildOptions, src string) ([]byte, error) {
	ov := strings.TrimSpace(opts.Overlay)
	flags := os.Getenv("GDIR_EXTRA_FLAGS")
	if ov == "" && flags == "" {
		if opts.Stdout != nil {
			report.Line(opts.Stdout, report.Reusing, "the bundled PS3 guest ("+guest.PrebuiltVersion()+")")
		}
		return guest.PrebuiltELF()
	}
	env, _ := t.Env(opts.CommonOptions)
	if !compileReady(env) {
		return nil, report.Missing("ps3dev, to compile the guest with --overlay or GDIR_EXTRA_FLAGS",
			"blazium-toolchain ps3 setup --profile compile (Linux), or build without --overlay")
	}
	if ov != "" {
		if err := guest.Overlay(src, ov); err != nil {
			return nil, err
		}
	}
	makeProg := env["PS3_MAKE"]
	if makeProg == "" {
		makeProg = "make"
	}
	extra := map[string]string{"PS3DEV": env["PS3DEV"], "PSL1GHT": env["PSL1GHT"]}
	args := []string{"-C", src}
	if flags != "" {
		args = append(args, "GDIR_EXTRA_FLAGS="+flags)
	}
	if err := t.runEnv(ctx, makeProg, args, extra, opts.Stdout, opts.Stderr); err != nil {
		return nil, report.Fail("make: "+err.Error(), "")
	}
	raw, err := os.ReadFile(filepath.Join(src, "gdir.prx.elf"))
	if err != nil {
		return nil, report.Fail("no gdir.prx.elf was made", "")
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
		return report.Usage("ps3 run needs the game: its folder, its USRDIR/EBOOT.BIN or a .pkg (installed first)")
	}
	emu := env["RPCS3_EXE"]
	if emu == "" {
		return report.Missing("no RPCS3 (RPCS3_EXE)", "install RPCS3 and its PS3 system software, then set RPCS3_EXE")
	}
	args := []string{"--no-gui"}
	if st, err := os.Stat(game); err == nil && st.IsDir() {
		if eboot := filepath.Join(game, "USRDIR", "EBOOT.BIN"); fileExists(eboot) {
			game = eboot
		}
	}
	if strings.EqualFold(filepath.Ext(game), ".pkg") {
		args = []string{"--installpkg"}
	}
	args = append(args, game)
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
	return report.Usage("ps3 has no ISO; ps3 build --sample gdir writes the game folder and the .pkg")
}

func fileExists(p string) bool {
	if p == "" {
		return false
	}
	st, err := os.Stat(p)
	return err == nil && !st.IsDir()
}

func copyFile(from, to string) error {
	raw, err := os.ReadFile(from)
	if err != nil {
		return err
	}
	return os.WriteFile(to, raw, 0o644)
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
		return copyFile(path, target)
	})
}
