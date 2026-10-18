package ps1

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"strings"

	guest "github.com/blazium-games/blazium-toolchain/guest/ps1"
	"github.com/blazium-games/blazium-toolchain/internal/cache"
	"github.com/blazium-games/blazium-toolchain/internal/execx"
	"github.com/blazium-games/blazium-toolchain/internal/fetch"
	"github.com/blazium-games/blazium-toolchain/internal/platforms"
	"github.com/blazium-games/blazium-toolchain/internal/report"
	"github.com/blazium-games/blazium-toolchain/internal/settings"
)

const ID = "ps1"

// Tool is the PS1 implementation of platforms.Platform.
type Tool struct {
	Runner  execx.Runner
	Fetcher ZipFetcher
	Assets  []ZipAsset
	JSON    jsonGetter
	CLIURL  string // tests pin a zip URL and skip AppDistrib
}

func New() *Tool {
	return &Tool{Runner: execx.Host{}, Fetcher: fetch.HTTP{}}
}

func (t *Tool) fetcher() ZipFetcher {
	if t.Fetcher != nil {
		return t.Fetcher
	}
	return fetch.HTTP{}
}

func (t *Tool) compileAssets() []ZipAsset {
	if len(t.Assets) > 0 {
		return t.Assets
	}
	return defaultCompileAssets()
}

func (t *Tool) runner() execx.Runner {
	if t.Runner == nil {
		return execx.Host{}
	}
	return t.Runner
}

func (t *Tool) Info() platforms.Info {
	return platforms.Info{
		ID:          ID,
		Name:        "PlayStation 1",
		Status:      platforms.StatusSupported,
		Commands:    []string{"setup", "env", "status", "build", "export-guest", "run", "iso", "fmv"},
		Description: "PSn00bSDK guest build + pcsx-redux. Official Blazium PS1 toolchain (host: Windows and Linux).",
	}
}

func (t *Tool) Setup(ctx context.Context, opts platforms.SetupOptions) error {
	if err := requireHost(); err != nil {
		return err
	}
	_ = ctx
	profile := opts.Profile
	if profile == "" {
		profile = "compile"
	}
	if profile != "compile" && profile != "dev" && profile != "iso" {
		return report.Usage("profile must be compile, dev, or iso")
	}

	env, notes := t.discover(opts.Prefix)
	if !compileReady(env) && !opts.Offline {
		if err := t.ensureCompile(ctx, opts.Prefix, opts.Stdout); err != nil {
			return err
		}
		env, notes = t.discover(opts.Prefix)
	}
	if opts.Offline && env["MIPS_GCC"] == "" && lookFile("mipsel-none-elf-gcc") == "" {
		return report.Offline("no mipsel-none-elf-gcc in prefix, env, or PATH", "drop --offline or run blazium-toolchain ps1 setup --profile compile")
	}
	if !opts.Offline && !compileReady(env) {
		return report.Missing("ps1 compile needs mipsel-none-elf-gcc, elf2x, and PSN00BSDK_LIBS", "blazium-toolchain ps1 setup --profile compile")
	}
	if needsDev(profile) && !opts.Offline {
		if err := t.ensureDev(ctx, opts.Prefix, opts.Stdout); err != nil {
			return err
		}
		env, notes = t.discover(opts.Prefix)
	}
	if err := installGuestRuntime(opts.Prefix); err != nil {
		return err
	}

	if needsDev(profile) && !destReady(env) {
		if opts.Offline {
			return report.Offline("ps1 dev needs OPENBIOS and PCSX_EXE", "drop --offline or set OPENBIOS and PCSX_EXE")
		}
		return report.Missing("ps1 dev needs OPENBIOS and PCSX_EXE", "blazium-toolchain ps1 setup --profile dev")
	}

	st := cache.State{
		Platform: ID,
		Profile:  profile,
		Env:      env,
		Notes:    append(notes, "components: "+joinIDs(componentsForProfile(profile))),
	}
	if err := cache.WriteState(opts.Prefix, ID, st); err != nil {
		return err
	}

	manifestPath := filepath.Join(cache.PlatformDir(opts.Prefix, ID), "components.json")
	raw, err := json.MarshalIndent(componentsForProfile(profile), "", "  ")
	if err != nil {
		return err
	}
	if err := os.WriteFile(manifestPath, append(raw, '\n'), 0o644); err != nil {
		return err
	}

	report.Setup(opts.Stdout, ID, profile, cache.PlatformDir(opts.Prefix, ID), env)
	if !compileReady(env) && !opts.Offline {
		report.Line(opts.Stdout, report.Note, "compile tools incomplete after fetch")
	}
	return nil
}

func (t *Tool) Env(opts platforms.CommonOptions) (platforms.EnvMap, error) {
	st, err := cache.ReadState(opts.Prefix, ID)
	if err != nil {
		if os.IsNotExist(err) {
			env, _ := t.discover(opts.Prefix)
			return env, nil
		}
		return nil, err
	}
	if st.Env == nil {
		st.Env = map[string]string{}
	}
	return st.Env, nil
}

func (t *Tool) Status(opts platforms.CommonOptions) (map[string]any, error) {
	env, err := t.Env(opts)
	if err != nil {
		return nil, err
	}
	st, _ := cache.ReadState(opts.Prefix, ID)
	profile := or(st.Profile, "compile")
	supported := HostSupported()
	out := map[string]any{
		"platform":       ID,
		"profile":        st.Profile,
		"env":            env,
		"components":     componentsForProfile(profile),
		"compile_ready":  compileReady(env) && supported,
		"dev_ready":      destReady(env) && supported,
		"ready":          supported && profileReady(profile, env),
		"guest_abi":      guest.CookABI,
		"guest_dir":      GuestDir(opts.Prefix),
		"host_os":        runtime.GOOS,
		"host_supported": supported,
	}
	return out, nil
}

func (t *Tool) Run(ctx context.Context, opts platforms.RunOptions) error {
	if err := requireHost(); err != nil {
		return err
	}
	env, err := t.Env(opts.CommonOptions)
	if err != nil {
		return err
	}
	if !fileExists(env["PCSX_EXE"]) || !fileExists(env["OPENBIOS"]) {
		env, _ = t.discover(opts.Prefix)
	}
	pcsx := env["PCSX_EXE"]
	bios := env["OPENBIOS"]
	if !fileExists(pcsx) {
		return report.Missing("pcsx-redux CLI not found", "blazium-toolchain ps1 setup --profile dev")
	}
	if !fileExists(bios) {
		return report.Missing("OpenBIOS not found", "blazium-toolchain ps1 setup --profile dev")
	}
	if opts.ISO == "" && opts.Exe == "" {
		return report.Usage("run requires an exe or --iso")
	}
	timeout := opts.Timeout
	if timeout <= 0 {
		timeout = settings.Current().Smoke()
	}
	runCtx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()

	args := []string{"-run", "-noupdate", "-safe", "-interpreter", "-softgpu", "-webserver", "-webserver-port", "8080", "-bios", bios, "-stdout"}
	if !opts.UI {
		args = append([]string{"-no-ui", "-testmode"}, args...)
	}
	pcdrv := opts.Pcdrv
	if pcdrv == "" && opts.Exe != "" {
		pcdrv = filepath.Dir(opts.Exe)
	}
	if pcdrv != "" {
		args = append(args, "-pcdrv", "-pcdrvbase", pcdrv)
	}
	if opts.ISO != "" {
		args = append(args, "-iso", opts.ISO, "-fastboot")
	} else {
		args = append(args, "-loadexe", opts.Exe)
	}
	extraPath := []string{filepath.Dir(pcsx)}
	err = t.runEnv(runCtx, pcsx, args, extraPath, map[string]string{"PCSX_EXE": pcsx, "OPENBIOS": bios}, opts.Stdout, opts.Stderr)
	if errors.Is(runCtx.Err(), context.DeadlineExceeded) {
		if opts.Stdout != nil {
			report.Linef(opts.Stdout, report.Ready, "pcsx-redux smoke timeout %s", timeout)
		}
		return nil
	}
	return err
}

func (t *Tool) ISO(ctx context.Context, opts platforms.ISOOptions) error {
	if err := requireHost(); err != nil {
		return err
	}
	if opts.XML == "" && opts.Dir != "" {
		return t.gdirISO(ctx, opts)
	}
	if opts.XML == "" {
		return report.Usage("iso requires --xml (an mkpsxiso project) or --dir (a GDIR export folder)")
	}
	mk, err := t.mkpsxiso(opts.Prefix)
	if err != nil {
		return err
	}
	args := []string{opts.XML}
	if opts.Out != "" {
		args = append(args, "-o", opts.Out)
	}
	return t.runner().Run(ctx, mk, args, writerOrDiscard(opts.Stdout), writerOrDiscard(opts.Stderr))
}

func (t *Tool) mkpsxiso(prefix string) (string, error) {
	r := t.runner()
	mk, err := execx.LookPrefersEnv(r, "MKPSXISO", "mkpsxiso")
	if err != nil {
		env, _ := t.discover(prefix)
		if env["MKPSXISO"] != "" && fileExists(env["MKPSXISO"]) {
			mk = env["MKPSXISO"]
			err = nil
		} else if p := walkNamed(cache.PlatformDir(prefix, ID), hostNames("mkpsxiso")...); p != "" {
			mk = p
			err = nil
		}
	}
	if err != nil || mk == "" {
		return "", report.Missing("mkpsxiso not found (GPL, spawn only, never fetched)", "install mkpsxiso and set MKPSXISO or put it on PATH")
	}
	return mk, nil
}

func (t *Tool) discover(prefix string) (map[string]string, []string) {
	env := map[string]string{}
	var notes []string

	copyEnv := func(key string) {
		if v := os.Getenv(key); v != "" {
			env[key] = v
		}
	}
	copyEnv("PSN00BSDK_LIBS")
	copyEnv("PSN00BSDK_TC")
	copyEnv("MIPS_GCC")
	copyEnv("ELF2X")
	copyEnv("OPENBIOS")
	copyEnv("PCSX_EXE")
	copyEnv("MKPSXISO")

	plat := cache.PlatformDir(prefix, ID)

	if env["MIPS_GCC"] == "" {
		if p := findVendorFile(prefix, filepath.Join("gcc", "bin", "mipsel-none-elf-gcc")); p != "" {
			env["MIPS_GCC"] = p
			notes = append(notes, "vendored mipsel-none-elf-gcc")
		} else if p := walkNamed(plat, hostNames("mipsel-none-elf-gcc")...); p != "" {
			env["MIPS_GCC"] = p
			notes = append(notes, "found mipsel-none-elf-gcc under prefix")
		} else if p := lookFile("mipsel-none-elf-gcc"); p != "" {
			env["MIPS_GCC"] = p
			notes = append(notes, "discovered mipsel-none-elf-gcc on PATH")
		}
	}
	if env["ELF2X"] == "" {
		if p := findVendorFile(prefix, filepath.Join("elf2x", "elf2x")); p != "" {
			env["ELF2X"] = p
			notes = append(notes, "vendored elf2x")
		} else if p := walkNamed(plat, hostNames("elf2x")...); p != "" {
			env["ELF2X"] = p
			notes = append(notes, "found elf2x under prefix")
		}
		if env["ELF2X"] == "" {
			if p := lookFile("elf2x"); p != "" {
				env["ELF2X"] = p
				notes = append(notes, "discovered elf2x on PATH")
			}
		}
	}
	if env["PSN00BSDK_LIBS"] == "" {
		if p := findLibpsn00b(plat); p != "" {
			env["PSN00BSDK_LIBS"] = p
			notes = append(notes, "found libpsn00b under prefix")
		} else if p := findVendorDir(prefix, "psn00bsdk"); p != "" {
			if lib := filepath.Join(p, "lib", "libpsn00b"); dirExists(lib) {
				env["PSN00BSDK_LIBS"] = lib
			} else {
				env["PSN00BSDK_LIBS"] = p
			}
			notes = append(notes, "vendored psn00bsdk")
		}
	}
	if env["PSN00BSDK_TC"] == "" && env["MIPS_GCC"] != "" {
		env["PSN00BSDK_TC"] = filepath.Dir(filepath.Dir(env["MIPS_GCC"]))
	}
	if env["PCSX_EXE"] == "" {
		if p := findPCSX(filepath.Join(plat, "pcsx-redux"), plat); p != "" {
			env["PCSX_EXE"] = p
			notes = append(notes, "found pcsx-redux under prefix")
		} else if p := findVendorFile(prefix, filepath.Join("pcsx-redux", "pcsx-redux")); p != "" {
			env["PCSX_EXE"] = p
			notes = append(notes, "vendored pcsx-redux")
		} else if p := lookFile("pcsx-redux"); p != "" {
			env["PCSX_EXE"] = p
		}
	}
	if env["OPENBIOS"] == "" {
		canon := filepath.Join(plat, "openbios", "openbios.bin")
		if fileExists(canon) {
			env["OPENBIOS"] = absOr(canon)
			notes = append(notes, "cached OpenBIOS")
		} else if cand := findOpenBIOSFile(filepath.Join(plat, "openbios"), filepath.Join(plat, "pcsx-redux"), plat); cand != "" {
			env["OPENBIOS"] = cand
			notes = append(notes, "found OpenBIOS under prefix")
		} else if cand := findVendorFile(prefix, filepath.Join("openbios", "openbios.bin")); cand != "" {
			env["OPENBIOS"] = cand
			notes = append(notes, "vendored OpenBIOS")
		}
	}
	if env["MKPSXISO"] == "" {
		if p := findVendorFile(prefix, filepath.Join("mkpsxiso", "mkpsxiso")); p != "" {
			env["MKPSXISO"] = p
			notes = append(notes, "vendored mkpsxiso")
		} else if p := walkNamed(plat, hostNames("mkpsxiso")...); p != "" {
			env["MKPSXISO"] = p
			notes = append(notes, "found mkpsxiso under prefix")
		} else if p := lookFile("mkpsxiso"); p != "" {
			env["MKPSXISO"] = p
			notes = append(notes, "discovered mkpsxiso on PATH")
		}
	}
	return env, notes
}

func (t *Tool) ensureCompile(ctx context.Context, prefix string, log io.Writer) error {
	root := cache.PlatformDir(prefix, ID)
	for _, a := range t.compileAssets() {
		dest := filepath.Join(root, a.Dest)
		if compilePiecePresent(dest, a.ID) {
			continue
		}
		report.Linef(log, report.Fetching, "%s  %s", a.ID, a.URL)
		if err := t.fetcher().FetchZip(ctx, a.URL, a.SHA256, dest, log); err != nil {
			return report.Missing(a.ID+" fetch failed: "+err.Error(), "check network or drop --offline")
		}
	}
	return nil
}

func compilePiecePresent(dest, id string) bool {
	switch id {
	case "mipsel-none-elf-gcc":
		return walkNamed(dest, hostNames("mipsel-none-elf-gcc")...) != ""
	case "psn00bsdk":
		return walkNamed(dest, hostNames("elf2x")...) != "" || walkDirNamed(dest, "libpsn00b") != ""
	case "cmake":
		return walkNamed(dest, hostNames("cmake")...) != ""
	case "ninja":
		return walkNamed(dest, hostNames("ninja")...) != ""
	default:
		return false
	}
}

func compileReady(env map[string]string) bool {
	return fileExists(env["MIPS_GCC"]) && fileExists(env["ELF2X"]) && dirExists(env["PSN00BSDK_LIBS"]) && dirExists(env["PSN00BSDK_TC"])
}

func fileExists(p string) bool {
	if p == "" {
		return false
	}
	st, err := os.Stat(p)
	return err == nil && !st.IsDir()
}

func dirExists(p string) bool {
	if p == "" {
		return false
	}
	st, err := os.Stat(p)
	return err == nil && st.IsDir()
}

func lookFile(name string) string {
	p, err := execx.Host{}.LookPath(name)
	if err != nil {
		return ""
	}
	return p
}

func writerOrDiscard(w io.Writer) io.Writer {
	if w == nil {
		return io.Discard
	}
	return w
}

func joinIDs(cs []Component) string {
	ids := make([]string, len(cs))
	for i, c := range cs {
		ids[i] = c.ID
	}
	return strings.Join(ids, ",")
}

func or(a, b string) string {
	if a != "" {
		return a
	}
	return b
}
