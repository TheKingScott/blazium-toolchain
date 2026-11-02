// Package nswitch is the Nintendo Switch platform ("switch" on the command
// line; switch is a Go keyword): the GDIR guest (guest/switch) with a PS2
// GDIR export, whose packs the Switch reads as they are, made into a
// homebrew .nro (switch/NAME/NAME.nro: the program, its icon and names, and
// the game's DATA in its RomFS) and the same as a .zip to unpack onto an SD
// card, for a Switch running homebrew (the Homebrew Menu) or an emulator.
//
// A build needs no Switch SDK: the guest comes compiled with this CLI
// (guest/switch/prebuilt/gdir.nro, built with devkitA64, libnx and deko3d),
// and the CLI writes the .nro's assets itself (nro.go), so switch builds on
// Windows, Linux and macOS alike. Only a guest with code of your own
// (--overlay, GDIR_EXTRA_FLAGS) is compiled, with devkitPro's devkitA64,
// libnx, deko3d and switch-tools (DEVKITPRO).
package nswitch

import (
	"archive/zip"
	"context"
	"fmt"
	"image"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strings"
	"time"

	guest "github.com/blazium-games/blazium-toolchain/guest/switch"
	"github.com/blazium-games/blazium-toolchain/internal/cache"
	"github.com/blazium-games/blazium-toolchain/internal/execx"
	"github.com/blazium-games/blazium-toolchain/internal/platforms"
	"github.com/blazium-games/blazium-toolchain/internal/report"
)

const ID = "switch"

// Tool is the Switch implementation of platforms.Platform.
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
		Name:     "Nintendo Switch",
		Status:   platforms.StatusSupported,
		Commands: []string{"setup", "env", "status", "build", "run"},
		Description: "Nintendo Switch (homebrew: the Homebrew Menu, or an emulator). build --sample gdir makes a PS2 export " +
			"into an .nro (switch/NAME/NAME.nro, and a .zip for the SD card) with the bundled guest, on Windows, Linux and macOS, no SDK needed. Run: SWITCH_EMU (Ryujinx).",
	}
}

// envKeys are what discover finds and Env reports.
var envKeys = []string{"DEVKITPRO", "SWITCH_MAKE", "SWITCH_EMU"}

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
			if fileExists(filepath.Join(c, "devkitA64", "bin", exeName("aarch64-none-elf-g++"))) {
				env["DEVKITPRO"] = c
				break
			}
		}
	}
	if env["SWITCH_MAKE"] == "" {
		if p, err := exec.LookPath("make"); err == nil {
			env["SWITCH_MAKE"] = p
		}
	}
	if env["SWITCH_EMU"] == "" {
		for _, n := range []string{"Ryujinx", "ryujinx", "citron", "eden", "Eden"} {
			if p, err := exec.LookPath(n); err == nil {
				env["SWITCH_EMU"] = p
				break
			}
		}
	}
	return env
}

// compileReady: devkitA64, libnx with deko3d and switch-tools' elf2nro are
// there to compile a guest of your own.
func compileReady(env map[string]string) bool {
	d := env["DEVKITPRO"]
	if d == "" {
		return false
	}
	return fileExists(filepath.Join(d, "devkitA64", "bin", exeName("aarch64-none-elf-g++"))) &&
		fileExists(filepath.Join(d, "libnx", "include", "deko3d.h")) &&
		fileExists(filepath.Join(d, "tools", "bin", exeName("elf2nro")))
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
			return report.Missing("devkitA64, libnx, deko3d and switch-tools (only a guest with code of your own needs them)",
				"install devkitPro's switch-dev and set DEVKITPRO, or leave out --overlay")
		}
	default:
		return report.Usage("switch setup --profile prebuilt|compile")
	}
	if err := guest.InstallGDIR(GDIRGuestDir(opts.Prefix)); err != nil {
		return err
	}
	st := cache.State{Platform: ID, Profile: profile, Env: env}
	if err := cache.WriteState(opts.Prefix, ID, st); err != nil {
		return err
	}
	report.Setup(log, ID, profile, cache.PlatformDir(opts.Prefix, ID), env)
	if env["SWITCH_EMU"] == "" {
		report.Line(log, report.Note, "no SWITCH_EMU: switch run will not find an emulator")
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
		"run_ready":     fileExists(env["SWITCH_EMU"]),
		"ready":         true,
		"product":       "nro",
		"guest_dir":     GDIRGuestDir(opts.Prefix),
		"host_os":       runtime.GOOS,
	}, nil
}

// GDIRGuestDir is <prefix>/switch/guest/gdir: the GDIR guest, staged per build.
func GDIRGuestDir(prefix string) string {
	return filepath.Join(cache.PlatformDir(prefix, ID), "guest", "gdir")
}

var appNameRe = regexp.MustCompile(`^[a-z0-9][a-z0-9_-]{0,31}$`)

// appName is the app folder's name from a title or file name: lower-case
// letters, digits, '-' and '_' ("Penguin Kart" -> "penguinkart").
func appName(s string) string {
	var b strings.Builder
	for _, r := range strings.ToLower(s) {
		if (r >= 'a' && r <= 'z') || (r >= '0' && r <= '9') || r == '-' || r == '_' {
			b.WriteRune(r)
		}
	}
	n := strings.TrimLeft(b.String(), "-_")
	if len(n) > 32 {
		n = n[:32]
	}
	if n == "" {
		n = "blaziumgame"
	}
	return n
}

// outputs: --out GAME.zip is the zip, with the app folder switch/NAME/
// beside it; --out DIR is DIR/switch/NAME/ and DIR/NAME.zip.
func outputs(out, name string) (zipPath, root string) {
	if strings.EqualFold(filepath.Ext(out), ".zip") {
		return out, filepath.Dir(out)
	}
	return filepath.Join(out, name+".zip"), out
}

// Version is the version the NACP gives (the Homebrew Menu shows it).
const Version = "1.0.0"

// defaultAuthor is the NACP's author.
const defaultAuthor = "Built with Blazium"

// Bundle is the game's .nro: the guest's program with its assets, the icon
// (from iconPath, or a plain one), the NACP (title) and a RomFS with DATA/
// and APPDIR.TXT (the app folder's name, where the guest keeps the save).
func Bundle(nro []byte, title, iconPath string, data []dataFile, name string) ([]byte, error) {
	var pic image.Image
	if iconPath != "" {
		var err error
		if pic, err = loadPicture(iconPath); err != nil {
			return nil, fmt.Errorf("icon: %w", err)
		}
	}
	icon, err := Icon(pic)
	if err != nil {
		return nil, fmt.Errorf("icon: %w", err)
	}
	files := []romFile{{"APPDIR.TXT", []byte(name)}}
	for _, f := range data {
		raw, err := os.ReadFile(f.Src)
		if err != nil {
			return nil, err
		}
		files = append(files, romFile{f.Path, raw})
	}
	romfs, err := RomFS(files)
	if err != nil {
		return nil, err
	}
	return WithAssets(nro, icon, NACP(title, defaultAuthor, Version), romfs)
}

func (t *Tool) Build(ctx context.Context, opts platforms.BuildOptions) error {
	if strings.ToLower(strings.TrimSpace(opts.Sample)) != "gdir" {
		return report.Usage("switch build needs --sample gdir (the GDIR guest with a PS2 export)")
	}
	if opts.Out == "" {
		return report.Usage("build requires --out GAME.zip (or a folder)")
	}
	base := strings.TrimSuffix(filepath.Base(opts.Out), filepath.Ext(opts.Out))
	title := strings.TrimSpace(opts.Title)
	if title == "" {
		title = base
	}
	name := strings.TrimSpace(opts.TitleID)
	if name == "" {
		name = appName(base)
	}
	if !appNameRe.MatchString(name) {
		return report.Usage("--title-id is the game's folder on the SD card (switch/NAME): lower-case letters, digits, - and _, up to 32")
	}
	export, err := gdirExportDir(opts.PackDir, opts.Out)
	if err != nil {
		return err
	}
	files, err := gdirFiles(export, opts.Stdout)
	if err != nil {
		return err
	}
	nro, err := t.guestNRO(ctx, opts)
	if err != nil {
		return err
	}
	icon := strings.TrimSpace(opts.Icon)
	if icon == "" {
		icon = exportIcon(export)
	}
	game, err := Bundle(nro, title, icon, files, name)
	if err != nil {
		if strings.HasPrefix(err.Error(), "icon:") {
			return report.Fail(err.Error(), "--icon is a PNG or JPEG (256x256 shows best)")
		}
		return report.Fail("nro: "+err.Error(), "")
	}
	now := time.Now()

	zipPath, root := outputs(opts.Out, name)
	app := filepath.Join(root, "switch", name)
	// written over: saves (BLAZIUM.SAV) stay
	if err := os.MkdirAll(app, 0o755); err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(app, name+".nro"), game, 0o644); err != nil {
		return err
	}
	notices, err := buildNotices(zipPath)
	if err != nil {
		return report.Fail("third-party notices: "+err.Error(), "")
	}
	if err := os.WriteFile(filepath.Join(app, "THIRD-PARTY-NOTICES.txt"), notices, 0o644); err != nil {
		return err
	}
	// the zip holds switch/NAME/: the .nro and the notices, no save
	if err := writeZip(zipPath, now, []zipEntry{
		{"switch/" + name + "/" + name + ".nro", game},
		{"switch/" + name + "/THIRD-PARTY-NOTICES.txt", notices},
	}); err != nil {
		return report.Fail("zip: "+err.Error(), "")
	}
	if opts.Stdout != nil {
		report.Line(opts.Stdout, report.Wrote, zipPath)
		report.Line(opts.Stdout, report.Wrote, filepath.Join(app, name+".nro"))
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

// guestNRO: the bundled guest, unless an overlay or GDIR_EXTRA_FLAGS asks
// for one compiled here (with devkitA64 and libnx).
func (t *Tool) guestNRO(ctx context.Context, opts platforms.BuildOptions) ([]byte, error) {
	ov := strings.TrimSpace(opts.Overlay)
	flags := os.Getenv("GDIR_EXTRA_FLAGS")
	if ov == "" && flags == "" {
		if opts.Stdout != nil {
			report.Line(opts.Stdout, report.Reusing, "the bundled Switch guest ("+guest.PrebuiltVersion()+")")
		}
		return guest.PrebuiltNRO()
	}
	env, _ := t.Env(opts.CommonOptions)
	if !compileReady(env) {
		return nil, report.Missing("devkitA64, libnx, deko3d and switch-tools, to compile the guest with --overlay or GDIR_EXTRA_FLAGS",
			"install devkitPro's switch-dev and set DEVKITPRO, or build without --overlay")
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
	makeProg := env["SWITCH_MAKE"]
	if makeProg == "" {
		makeProg = "make"
	}
	dkp := env["DEVKITPRO"]
	extra := map[string]string{"DEVKITPRO": dkp}
	args := []string{"-C", src, "ELF2NRO=" + filepath.Join(dkp, "tools", "bin", exeName("elf2nro"))}
	if flags != "" {
		args = append(args, "GDIR_EXTRA_FLAGS="+flags)
	}
	if err := t.runEnv(ctx, makeProg, args, extra, opts.Stdout, opts.Stderr); err != nil {
		return nil, report.Fail("make: "+err.Error(), "")
	}
	raw, err := os.ReadFile(filepath.Join(src, "gdir.nro"))
	if err != nil {
		return nil, report.Fail("no gdir.nro was made", "")
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
		return report.Usage("switch run needs the game: its .nro or its switch/NAME folder")
	}
	emu := env["SWITCH_EMU"]
	if emu == "" {
		return report.Missing("no Switch emulator (SWITCH_EMU)", "set SWITCH_EMU to an emulator that opens .nro files from its command line")
	}
	if st, err := os.Stat(game); err == nil && st.IsDir() {
		game = filepath.Join(game, filepath.Base(game)+".nro")
	}
	if !fileExists(game) {
		return report.Missing("no "+game, "switch build --sample gdir writes switch/NAME/NAME.nro")
	}
	timeout := opts.Timeout
	if timeout <= 0 {
		timeout = 120 * time.Second
	}
	cctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	// yuzu's descendants (Eden, Citron) take -g FILE; Ryujinx takes the file
	args := []string{"-g", game}
	if strings.Contains(strings.ToLower(filepath.Base(emu)), "ryujinx") {
		args = []string{game}
	}
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
	return report.Usage("switch has no ISO here; switch build --sample gdir writes the .nro and its .zip")
}

type zipEntry struct {
	Name string
	Data []byte
}

// writeZip writes the entries, sorted, so the same game makes the same zip.
func writeZip(zipPath string, when time.Time, entries []zipEntry) error {
	sort.Slice(entries, func(i, j int) bool { return entries[i].Name < entries[j].Name })
	f, err := os.Create(zipPath)
	if err != nil {
		return err
	}
	zw := zip.NewWriter(f)
	for _, e := range entries {
		w, err := zw.CreateHeader(&zip.FileHeader{Name: e.Name, Method: zip.Deflate, Modified: when})
		if err != nil {
			f.Close()
			return err
		}
		if _, err := w.Write(e.Data); err != nil {
			f.Close()
			return err
		}
	}
	if err := zw.Close(); err != nil {
		f.Close()
		return err
	}
	return f.Close()
}

func fileExists(p string) bool {
	if p == "" {
		return false
	}
	st, err := os.Stat(p)
	return err == nil && !st.IsDir()
}
