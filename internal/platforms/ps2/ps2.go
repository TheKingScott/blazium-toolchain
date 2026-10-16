package ps2

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"strings"

	guest "github.com/blazium-games/blazium-toolchain/guest/ps2"
	"github.com/blazium-games/blazium-toolchain/internal/cache"
	"github.com/blazium-games/blazium-toolchain/internal/execx"
	"github.com/blazium-games/blazium-toolchain/internal/fetch"
	"github.com/blazium-games/blazium-toolchain/internal/iso"
	"github.com/blazium-games/blazium-toolchain/internal/platforms"
	"github.com/blazium-games/blazium-toolchain/internal/report"
	"github.com/blazium-games/blazium-toolchain/internal/settings"
)

const ID = "ps2"

// Tool is the PS2 implementation of platforms.Platform.
type Tool struct {
	Runner  execx.Runner
	Fetcher zipFetcher
}

type zipFetcher interface {
	FetchZip(ctx context.Context, url, sha256, destDir string, log io.Writer) error
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

func (t *Tool) fetcher() zipFetcher {
	if t.Fetcher != nil {
		return t.Fetcher
	}
	return fetch.HTTP{}
}

func (t *Tool) Info() platforms.Info {
	return platforms.Info{
		ID:          ID,
		Name:        "PlayStation 2",
		Status:      platforms.StatusSupported,
		Commands:    []string{"setup", "env", "status", "build", "export-guest", "run", "iso", "elf-info", "chd"},
		Description: ps2InfoDescription(),
	}
}

func ps2InfoDescription() string {
	base := "PlayStation 2 EE guest is bundled in this CLI (ps2sdk graph/draw/dma/packet + PCSX2). Compile/run/iso: Windows and Linux only."
	if HostSupported() {
		return base
	}
	return base + " This host cannot compile or run; env/status/export-guest still work."
}

func (t *Tool) Setup(ctx context.Context, opts platforms.SetupOptions) error {
	if err := requireHost(); err != nil {
		return err
	}
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
	if opts.Offline && env["EE_GCC"] == "" && lookFile("mips64r5900el-ps2-elf-gcc") == "" {
		return report.Offline("no mips64r5900el-ps2-elf-gcc in prefix, env, or PATH", "drop --offline or run blazium-toolchain ps2 setup --profile compile")
	}
	if !opts.Offline && !compileReady(env) {
		return report.Missing("ps2 compile needs EE_GCC, PS2SDK, and PS2DEV", "blazium-toolchain ps2 setup --profile compile")
	}
	if err := ensureWindowsHostDLLs(env["EE_GCC"], opts.Stdout); err != nil {
		return err
	}
	if err := installGuestRuntime(opts.Prefix); err != nil {
		return err
	}

	if needsDev(profile) {
		env, notes = t.discover(opts.Prefix)
		if env["PCSX2_EXE"] == "" && !opts.Offline {
			return report.Missing("ps2 dev needs PCSX2_EXE (BIOS is never fetched)", "install pcsx2-qt or set PCSX2_EXE")
		}
	}

	st := cache.State{
		Platform: ID,
		Profile:  profile,
		Env:      env,
		Notes:    append(notes, "never fetches a PS2 BIOS"),
	}
	if err := cache.WriteState(opts.Prefix, ID, st); err != nil {
		return err
	}
	report.Setup(opts.Stdout, ID, profile, cache.PlatformDir(opts.Prefix, ID), env)
	if env["PS2_BIOS_DIR"] == "" {
		report.Line(opts.Stdout, report.Note, "PS2_BIOS_DIR unset; ps2 run needs a user-supplied BIOS")
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
	if st.Env["PS2_BIOS_DIR"] == "" || !biosReady(st.Env) || st.Env["PCSX2_EXE"] == "" || !fileExists(st.Env["PCSX2_EXE"]) {
		fresh, _ := t.discover(opts.Prefix)
		if st.Env["PS2_BIOS_DIR"] == "" || !biosReady(st.Env) {
			if fresh["PS2_BIOS_DIR"] != "" {
				st.Env["PS2_BIOS_DIR"] = fresh["PS2_BIOS_DIR"]
			}
		}
		if st.Env["PCSX2_EXE"] == "" || !fileExists(st.Env["PCSX2_EXE"]) {
			if fresh["PCSX2_EXE"] != "" {
				st.Env["PCSX2_EXE"] = fresh["PCSX2_EXE"]
			}
		}
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
	biosOK := biosReady(env)
	out := map[string]any{
		"platform":       ID,
		"profile":        st.Profile,
		"env":            env,
		"compile_ready":  compileReady(env) && supported,
		"dev_ready":      compileReady(env) && fileExists(env["PCSX2_EXE"]) && supported,
		"run_ready":      compileReady(env) && fileExists(env["PCSX2_EXE"]) && biosOK && supported,
		"ready":          supported && compileReady(env),
		"guest_abi":      guest.CookABI,
		"guest_dir":      GuestDir(opts.Prefix),
		"host_os":        runtime.GOOS,
		"host_supported": supported,
		"bios_ready":     biosOK,
	}
	_ = profile
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
	if !fileExists(env["PCSX2_EXE"]) {
		env, _ = t.discover(opts.Prefix)
	}
	pcsx := env["PCSX2_EXE"]
	if !fileExists(pcsx) {
		return report.Missing("PCSX2 not found", "set PCSX2_EXE or install pcsx2-qt")
	}
	if !biosReady(env) {
		return report.Missing("PS2 BIOS not found (never fetched)", "set PS2_BIOS_DIR to a folder with a legal dump")
	}
	if opts.ISO == "" && opts.Exe == "" {
		return report.Usage("run requires an elf or --iso")
	}
	timeout := opts.Timeout
	if timeout <= 0 {
		timeout = settings.Current().Smoke()
	}
	runCtx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()

	args := []string{"-batch"}
	if !opts.UI {
		args = append([]string{"-nogui"}, args...)
	}
	if opts.ISO != "" {
		args = append(args, opts.ISO)
	} else {
		args = append(args, "-elf", opts.Exe)
		hostDir := hostFsDir(opts.Exe, opts.ISO, opts.Pcdrv)
		if hostDir != "" {
			prev, err := os.Getwd()
			if err == nil {
				if chdirErr := os.Chdir(hostDir); chdirErr == nil {
					defer func() { _ = os.Chdir(prev) }()
				}
			}
		}
	}
	err = t.runEnv(runCtx, pcsx, args, []string{filepath.Dir(pcsx)}, map[string]string{"PCSX2_EXE": pcsx}, opts.Stdout, opts.Stderr)
	if errors.Is(runCtx.Err(), context.DeadlineExceeded) {
		if opts.Stdout != nil {
			report.Linef(opts.Stdout, report.Ready, "PCSX2 smoke timeout %s", timeout)
		}
		return nil
	}
	return err
}

func (t *Tool) ISO(ctx context.Context, opts platforms.ISOOptions) error {
	_ = ctx
	if err := requireHost(); err != nil {
		return err
	}
	if opts.Dir == "" || opts.Out == "" {
		return report.Usage("iso requires --dir and --out")
	}
	if err := ensureSystemCNF(opts.Dir); err != nil {
		return err
	}
	vol := iso.VolumeOr(opts.VolumeID, settings.Current().PS2.ISOVolume)
	boot := bootELFName(opts.Dir)
	keep := func(name string) bool {
		return strings.EqualFold(name, "SYSTEM.CNF") || (boot != "" && strings.EqualFold(name, boot))
	}
	// Everything but SYSTEM.CNF and the boot ELF goes under DATA/: the guest
	// runtime tries cdrom0:\DATA\ first, then the root.
	if err := iso.WriteDataISOSubdir(opts.Dir, opts.Out, vol, keep, ISODataDir, skipOnDisc); err != nil {
		return err
	}
	if n := iso.LargestDirRecords(opts.Out); n > 1 && opts.Stdout != nil {
		report.Line(opts.Stdout, report.Note, fmt.Sprintf("a directory on %s spans %d sectors; the PS2 boot ROM reads only the first", opts.Out, n))
	}
	if opts.Stdout != nil {
		report.Line(opts.Stdout, report.Wrote, opts.Out)
	}
	return nil
}

func (t *Tool) discover(prefix string) (map[string]string, []string) {
	env := map[string]string{}
	var notes []string
	copyEnv := func(key string) {
		if v := os.Getenv(key); v != "" {
			env[key] = v
		}
	}
	copyEnv("PS2SDK")
	copyEnv("PS2DEV")
	copyEnv("EE_GCC")
	copyEnv("IOP_GCC")
	copyEnv("PCSX2_EXE")
	copyEnv("PS2_BIOS_DIR")

	plat := cache.PlatformDir(prefix, ID)

	if env["PS2SDK"] == "" || !dirExists(env["PS2SDK"]) {
		for _, root := range vendorRoots(prefix) {
			if p := findSDKRoot(root); p != "" {
				env["PS2SDK"] = p
				notes = append(notes, "found ps2sdk under prefix")
				break
			}
		}
		if env["PS2SDK"] == "" {
			for _, stuff := range siblingPS2Stuff() {
				if p := findSDKRoot(stuff); p != "" {
					env["PS2SDK"] = p
					notes = append(notes, "found sibling ps2_stuff/ps2sdk")
					break
				}
			}
		}
	}
	if env["PS2DEV"] == "" {
		if env["PS2SDK"] != "" {
			parent := filepath.Dir(env["PS2SDK"])
			if walkNamed(parent, hostNames("mips64r5900el-ps2-elf-gcc")...) != "" {
				env["PS2DEV"] = absOr(parent)
				notes = append(notes, "PS2DEV inferred from PS2SDK parent")
			}
		}
		if env["PS2DEV"] == "" {
			if p := os.Getenv("PS2DEV"); p != "" && dirExists(p) {
				env["PS2DEV"] = p
			}
		}
		if env["PS2DEV"] == "" {
			for _, root := range vendorRoots(prefix) {
				if dirExists(filepath.Join(root, "ps2dev")) {
					env["PS2DEV"] = absOr(filepath.Join(root, "ps2dev"))
					break
				}
			}
		}
	}
	if env["EE_GCC"] == "" {
		if env["PS2DEV"] != "" {
			if p := walkNamed(filepath.Join(env["PS2DEV"], "ee"), hostNames("mips64r5900el-ps2-elf-gcc")...); p != "" {
				env["EE_GCC"] = p
			}
		}
		if env["EE_GCC"] == "" {
			if p := walkNamed(plat, hostNames("mips64r5900el-ps2-elf-gcc")...); p != "" {
				env["EE_GCC"] = p
				notes = append(notes, "found EE gcc under prefix")
			} else if p := lookFile("mips64r5900el-ps2-elf-gcc"); p != "" {
				env["EE_GCC"] = p
				notes = append(notes, "discovered EE gcc on PATH")
			}
		}
	}
	if env["IOP_GCC"] == "" {
		if env["PS2DEV"] != "" {
			if p := walkNamed(filepath.Join(env["PS2DEV"], "iop"), hostNames("mipsel-none-elf-gcc")...); p != "" {
				env["IOP_GCC"] = p
			}
		}
		if env["IOP_GCC"] == "" {
			if p := lookFile("mipsel-none-elf-gcc"); p != "" {
				env["IOP_GCC"] = p
			}
		}
	}
	if env["PCSX2_EXE"] == "" {
		if p := walkNamed(plat, hostNames("pcsx2-qt")...); p != "" {
			env["PCSX2_EXE"] = p
		} else if p := lookFile("pcsx2-qt"); p != "" {
			env["PCSX2_EXE"] = p
		} else if p := lookFile("pcsx2"); p != "" {
			env["PCSX2_EXE"] = p
		}
	}
	if env["PS2_BIOS_DIR"] == "" {
		canon := filepath.Join(plat, "bios")
		if hit := findBiosDir(canon); hit != "" {
			env["PS2_BIOS_DIR"] = hit
		} else {
			names := settings.Current().PS2.BIOSSearchNames
			if len(names) == 0 {
				names = []string{"ps2 bios usa", "bios"}
			}
			for _, stuff := range siblingPS2Stuff() {
				for _, name := range names {
					if hit := findBiosDir(filepath.Join(stuff, name)); hit != "" {
						env["PS2_BIOS_DIR"] = hit
						notes = append(notes, "found sibling BIOS dump")
						break
					}
				}
				if env["PS2_BIOS_DIR"] != "" {
					break
				}
			}
		}
	} else if hit := findBiosDir(env["PS2_BIOS_DIR"]); hit != "" {
		env["PS2_BIOS_DIR"] = hit
	}
	return env, notes
}

func (t *Tool) ensureCompile(ctx context.Context, prefix string, log io.Writer) error {
	url := officialTarballURL()
	if url == "" {
		return nil
	}
	dest := filepath.Join(cache.PlatformDir(prefix, ID), "ps2dev")
	if compileTreeComplete(dest) {
		return ensureWindowsHostDLLs(walkNamed(dest, hostNames("mips64r5900el-ps2-elf-gcc")...), log)
	}
	if walkNamed(dest, hostNames("mips64r5900el-ps2-elf-gcc")...) != "" {
		report.Line(log, report.Installing, "ps2dev (re-extract; cc1 missing)")
		_ = os.RemoveAll(dest)
	}
	report.Linef(log, report.Fetching, "ps2dev  %s", url)
	if err := t.fetcher().FetchZip(ctx, url, "", dest, log); err != nil {
		if compileTreeComplete(dest) {
			return ensureWindowsHostDLLs(walkNamed(dest, hostNames("mips64r5900el-ps2-elf-gcc")...), log)
		}
		return err
	}
	return ensureWindowsHostDLLs(walkNamed(dest, hostNames("mips64r5900el-ps2-elf-gcc")...), log)
}

func compileTreeComplete(dest string) bool {
	if walkNamed(dest, hostNames("mips64r5900el-ps2-elf-gcc")...) == "" {
		return false
	}
	if findSDKRoot(dest) == "" {
		return false
	}
	return walkNamed(dest, hostNames("cc1")...) != ""
}

func officialTarballURL() string {
	return officialTarballURLFor(runtime.GOOS)
}

func officialTarballURLFor(goos string) string {
	f := settings.Current().PS2.Fetch
	switch goos {
	case "windows":
		return f.Windows
	case "linux":
		return f.Linux
	default:
		return ""
	}
}

func compileReady(env map[string]string) bool {
	if !fileExists(env["EE_GCC"]) || !dirExists(env["PS2SDK"]) || !dirExists(filepath.Join(env["PS2SDK"], "ee", "include")) {
		return false
	}
	root := env["PS2DEV"]
	if root == "" {
		root = filepath.Dir(env["EE_GCC"])
	}
	return walkNamed(root, hostNames("cc1")...) != "" || walkNamed(filepath.Dir(env["EE_GCC"]), hostNames("cc1")...) != ""
}

func needsDev(profile string) bool {
	return profile == "dev" || profile == "iso"
}

func isBiosFile(name string) bool {
	n := strings.ToLower(name)
	return strings.HasSuffix(n, ".bin") || strings.HasSuffix(n, ".rom")
}

func dirHasBiosFile(dir string) bool {
	ents, err := os.ReadDir(dir)
	if err != nil {
		return false
	}
	for _, e := range ents {
		if !e.IsDir() && isBiosFile(e.Name()) {
			return true
		}
	}
	return false
}

func findBiosDir(dir string) string {
	if !dirExists(dir) {
		return ""
	}
	if dirHasBiosFile(dir) {
		return absOr(dir)
	}
	ents, err := os.ReadDir(dir)
	if err != nil {
		return ""
	}
	fallback := ""
	for _, e := range ents {
		if !e.IsDir() {
			continue
		}
		child := filepath.Join(dir, e.Name())
		if !dirHasBiosFile(child) {
			continue
		}
		if prefer := strings.ToLower(strings.TrimSpace(settings.Current().PS2.BIOSPrefer)); prefer != "" && strings.Contains(strings.ToLower(e.Name()), prefer) {
			return absOr(child)
		}
		if fallback == "" {
			fallback = absOr(child)
		}
	}
	return fallback
}

func biosReady(env map[string]string) bool {
	return findBiosDir(env["PS2_BIOS_DIR"]) != ""
}

func writerOrDiscard(w io.Writer) io.Writer {
	if w == nil {
		return io.Discard
	}
	return w
}

func or(a, b string) string {
	if a != "" {
		return a
	}
	return b
}

// hostFsDir is the folder PCSX2 host: should resolve to for ELF runs (not ISO).
func hostFsDir(exe, iso, override string) string {
	if strings.TrimSpace(iso) != "" {
		return ""
	}
	if strings.TrimSpace(override) != "" {
		return override
	}
	if strings.TrimSpace(exe) == "" {
		return ""
	}
	return filepath.Dir(exe)
}

// ISODataDir is the ISO folder that holds cooked data (MESH/NODE/... and
// sidecars). Keep in sync with guest/ps2/runtime/iso_path.h.
const ISODataDir = "DATA"

// skipOnDisc leaves off the ISO what the guest never reads from cdrom0:
// build notes, the guest source tree, host-only saves, and sidecars the
// cooker wrote empty (header only: the guest treats a missing one the same).
func skipOnDisc(name string, size int64, isDir bool) bool {
	up := strings.ToUpper(name)
	if isDir {
		return up == "GUEST" || up == "EXTRA"
	}
	switch filepath.Ext(up) {
	case ".TXT", ".MAP", ".LOG":
		return true
	}
	if strings.HasPrefix(up, "SAVE") {
		return true
	}
	if size <= 10 {
		for _, stem := range []string{"ANIM", "PART", "PATH", "SPRT", "VEHL", "NAVM", "NAV", "TILE", "FMV"} {
			if strings.HasPrefix(up, stem) {
				return true
			}
		}
	}
	return false
}

// bootELFName returns the file SYSTEM.CNF's BOOT2 line points at ("" if none).
func bootELFName(dir string) string {
	b, err := os.ReadFile(filepath.Join(dir, "SYSTEM.CNF"))
	if err != nil {
		return ""
	}
	for _, line := range strings.Split(string(b), "\n") {
		line = strings.TrimSpace(line)
		if !strings.HasPrefix(strings.ToUpper(line), "BOOT2") {
			continue
		}
		v := line[strings.Index(line, "=")+1:]
		v = strings.TrimSpace(v)
		v = strings.TrimPrefix(v, "cdrom0:")
		v = strings.TrimLeft(v, "\\/")
		if i := strings.Index(v, ";"); i >= 0 {
			v = v[:i]
		}
		return v
	}
	return ""
}

func ensureSystemCNF(dir string) error {
	cnf := filepath.Join(dir, "SYSTEM.CNF")
	if fileExists(cnf) {
		return nil
	}
	s := settings.Current().PS2
	elfName := strings.TrimSpace(s.DefaultELF)
	if elfName == "" {
		elfName = "GAME.ELF"
	}
	ents, _ := os.ReadDir(dir)
	for _, e := range ents {
		if strings.EqualFold(filepath.Ext(e.Name()), ".elf") {
			elfName = strings.ToUpper(e.Name())
			break
		}
	}
	ver := strings.TrimSpace(s.CNFVer)
	if ver == "" {
		ver = "1.00"
	}
	vmode := strings.TrimSpace(s.CNFVMode)
	if vmode == "" {
		vmode = "NTSC"
	}
	body := fmt.Sprintf("BOOT2 = cdrom0:\\%s;1\nVER = %s\nVMODE = %s\n", elfName, ver, vmode)
	return os.WriteFile(cnf, []byte(body), 0o644)
}
