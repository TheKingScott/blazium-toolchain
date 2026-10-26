package app

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"os"
	"runtime"
	"sort"
	"strings"

	guestn64 "github.com/blazium-games/blazium-toolchain/guest/n64"
	guestps1 "github.com/blazium-games/blazium-toolchain/guest/ps1"
	guestps2 "github.com/blazium-games/blazium-toolchain/guest/ps2"
	"github.com/blazium-games/blazium-toolchain/internal/cache"
	"github.com/blazium-games/blazium-toolchain/internal/embedfs"
	"github.com/blazium-games/blazium-toolchain/internal/iso"
	"github.com/blazium-games/blazium-toolchain/internal/platforms"
	"github.com/blazium-games/blazium-toolchain/internal/platforms/future"
	"github.com/blazium-games/blazium-toolchain/internal/platforms/interdvd"
	n64plat "github.com/blazium-games/blazium-toolchain/internal/platforms/n64"
	"github.com/blazium-games/blazium-toolchain/internal/platforms/nds"
	"github.com/blazium-games/blazium-toolchain/internal/platforms/ps1"
	"github.com/blazium-games/blazium-toolchain/internal/platforms/ps2"
	ps3plat "github.com/blazium-games/blazium-toolchain/internal/platforms/ps3"
	"github.com/blazium-games/blazium-toolchain/internal/platforms/steam"
	"github.com/blazium-games/blazium-toolchain/internal/report"
	"github.com/blazium-games/blazium-toolchain/internal/settings"
)

// Version and License come from the embedded manifest.json. CI may override
// Version with -ldflags "-X github.com/blazium-games/blazium-toolchain/internal/app.Version=vX.Y.Z".
var (
	Version = embedfs.MustManifest().Version
	License = embedfs.MustManifest().License
)

const (
	ExitOK      = 0
	ExitUsage   = 1
	ExitPlanned = 2
	ExitTool    = 3
	ExitFail    = 4
)

func init() {
	platforms.Register(ps1.New())
	platforms.Register(ps2.New())
	platforms.Register(n64plat.New())
	platforms.Register(nds.New())
	platforms.Register(nds.NewDSi())
	platforms.Register(ps3plat.New())
	platforms.Register(interdvd.New())
	platforms.Register(steam.New())
	future.Register()
}

// Run is the CLI entry. argv is os.Args[1:].
func Run(ctx context.Context, argv []string, stdout, stderr io.Writer) int {
	if stdout == nil {
		stdout = os.Stdout
	}
	if stderr == nil {
		stderr = os.Stderr
	}

	fs := flag.NewFlagSet("blazium-toolchain", flag.ContinueOnError)
	fs.SetOutput(stderr)
	jsonOut := fs.Bool("json", false, "machine-readable output (for the installer / editor)")
	prefix := fs.String("prefix", "", "toolchain cache root (default: $BLAZIUM_TOOLCHAIN_PREFIX or OS cache)")
	settingsPath := fs.String("settings", "", "project YAML (default: $BLAZIUM_TOOLCHAIN_SETTINGS or ./blazium-toolchain.yml)")
	if err := fs.Parse(argv); err != nil {
		return ExitUsage
	}
	if err := settings.LoadFromCWD(*settingsPath); err != nil {
		return mapErr(report.Fail(err.Error(), "fix blazium-toolchain.yml or pass --settings FILE"), stderr, report.Context{Command: "settings", Version: Version, JSON: *jsonOut})
	}
	settings.ApplyEnv(settings.Current())
	args := fs.Args()
	if *prefix == "" {
		*prefix = settings.Current().Prefix
	}
	if *prefix == "" {
		*prefix = cache.DefaultPrefix()
	}

	if len(args) == 0 {
		usage(stderr)
		return ExitUsage
	}

	switch args[0] {
	case "help", "-h", "--help":
		usage(stdout)
		return ExitOK
	case "version":
		return cmdVersion(*jsonOut, stdout)
	case "list":
		return cmdList(*jsonOut, stdout)
	case "settings":
		return cmdSettings(*jsonOut, stdout)
	}

	// Platform-first: blazium-toolchain ps1 setup
	platID, rest := args[0], args[1:]
	base := common(*prefix, *jsonOut, stdout, stderr)
	p, err := platforms.Lookup(platID)
	if err != nil {
		return mapErr(err, stderr, report.Context{Command: platID, Prefix: *prefix, Version: Version, Settings: settings.LoadedPath(), JSON: *jsonOut})
	}
	if len(rest) == 0 {
		return mapErr(report.Usage(platID+" needs a command (setup, env, status, build, …)"), stderr, report.Context{
			Command: platID, Prefix: *prefix, Version: Version, Settings: settings.LoadedPath(), JSON: *jsonOut,
		})
	}
	return dispatch(ctx, p, rest, base, stdout, stderr)
}

func common(prefix string, jsonOut bool, stdout, stderr io.Writer) platforms.CommonOptions {
	return platforms.CommonOptions{Prefix: prefix, JSON: jsonOut, Stdout: stdout, Stderr: stderr}
}

func cmdVersion(jsonOut bool, w io.Writer) int {
	if jsonOut {
		_ = json.NewEncoder(w).Encode(map[string]string{
			"name":    "blazium-toolchain",
			"version": Version,
			"license": License,
		})
		return ExitOK
	}
	fmt.Fprintf(w, "blazium-toolchain %s\n", Version)
	return ExitOK
}

func cmdList(jsonOut bool, w io.Writer) int {
	list := platforms.List()
	if jsonOut {
		_ = json.NewEncoder(w).Encode(list)
		return ExitOK
	}
	for _, info := range list {
		note := ""
		switch info.ID {
		case "ps1", "ps2", "n64", "nds", "dsi", "ps3":
			if ps2.HostSupported() {
				note = "  Windows/Linux; guest bundled in this CLI"
			} else {
				note = "  compile/run unavailable on this host (env/status/export-guest only)"
			}
		}
		fmt.Fprintf(w, "%-6s %-12s %s%s\n", info.ID, info.Status, info.Name, note)
	}
	return ExitOK
}

func cmdSettings(jsonOut bool, w io.Writer) int {
	s := settings.Current()
	if jsonOut {
		_ = json.NewEncoder(w).Encode(map[string]any{
			"path":     settings.LoadedPath(),
			"settings": s,
		})
		return ExitOK
	}
	if p := settings.LoadedPath(); p != "" {
		fmt.Fprintf(w, "# loaded from %s\n", p)
	} else {
		fmt.Fprintln(w, "# built-in defaults (no project YAML found)")
	}
	raw, err := settings.Marshal(s)
	if err != nil {
		fmt.Fprintln(w, err.Error())
		return ExitFail
	}
	_, _ = w.Write(raw)
	return ExitOK
}

func failCtx(p platforms.Platform, args []string, base platforms.CommonOptions) report.Context {
	cmd := p.Info().ID
	if len(args) > 0 {
		cmd += " " + strings.Join(args, " ")
	}
	return report.Context{Command: cmd, Prefix: base.Prefix, Version: Version, Settings: settings.LoadedPath(), JSON: base.JSON}
}

func dispatch(ctx context.Context, p platforms.Platform, args []string, base platforms.CommonOptions, stdout, stderr io.Writer) int {
	info := failCtx(p, args, base)
	cmd := args[0]
	rest := args[1:]
	var err error
	switch cmd {
	case "setup":
		err = runSetup(ctx, p, rest, base)
	case "env":
		err = runEnv(p, base, stdout)
	case "status":
		err = runStatus(p, base, stdout)
	case "build":
		err = runBuild(ctx, p, rest, base)
	case "export-guest":
		err = runExportGuest(p, rest, base, stdout)
	case "run":
		err = runRun(ctx, p, rest, base)
	case "iso":
		err = runISO(ctx, p, rest, base)
	case "rom":
		err = runRom(ctx, p, rest, base)
	case "elf-info":
		err = runElfInfo(p, rest, base, stdout)
	case "chd":
		err = runChd(ctx, p, rest, base)
	case "fmv":
		err = runFMV(p, rest, base, stdout)
	case "meta":
		err = runMeta(p, rest, base, stdout)
	case "ffmpeg", "ffprobe":
		err = runEncode(ctx, p, cmd, rest, base)
	default:
		err = report.Usage(fmt.Sprintf("unknown command %q for platform %s", cmd, p.Info().ID))
	}
	return mapErr(err, stderr, info)
}

func runSetup(ctx context.Context, p platforms.Platform, args []string, base platforms.CommonOptions) error {
	fs := flag.NewFlagSet("setup", flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	profile := fs.String("profile", settings.Current().ProfileFor(p.Info().ID), "compile|dev|iso|rom")
	offline := fs.Bool("offline", false, "do not fetch; only discover local tools")
	if err := fs.Parse(args); err != nil {
		return report.Usage(err.Error())
	}
	return p.Setup(ctx, platforms.SetupOptions{CommonOptions: base, Profile: *profile, Offline: *offline})
}

func runEnv(p platforms.Platform, base platforms.CommonOptions, stdout io.Writer) error {
	env, err := p.Env(base)
	if err != nil {
		return err
	}
	if base.JSON {
		return json.NewEncoder(stdout).Encode(env)
	}
	keys := make([]string, 0, len(env))
	for k := range env {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		fmt.Fprintf(stdout, "%s=%s\n", k, env[k])
	}
	return nil
}

func runStatus(p platforms.Platform, base platforms.CommonOptions, stdout io.Writer) error {
	st, err := p.Status(base)
	if err != nil {
		return err
	}
	return json.NewEncoder(stdout).Encode(st)
}

func runBuild(ctx context.Context, p platforms.Platform, args []string, base platforms.CommonOptions) error {
	fs := flag.NewFlagSet("build", flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	src := fs.String("src", "", "guest CMake source dir (full replace of the bundled stub)")
	out := fs.String("out", "", "output PS-X EXE path")
	sample := fs.String("sample", "", "official SDK sample (template, gte) or the bundled guest (ps1: gdir), when --src is empty")
	overlay := fs.String("overlay", "", "extra or replacement *.cpp on top of the bundled stub (or --src); also BLAZIUM_PS1_OVERLAY")
	exportSrc := fs.String("export-src", "", "write the resolved guest C++ tree here")
	tim := fs.String("tim", "", "optional cooked TIM to embed in the guest (PS1)")
	mesh := fs.String("mesh", "", "optional cooked mesh to embed in the guest")
	gtex := fs.String("gtex", "", "optional cooked GTEX (GS PSM) to embed in the PS2 guest")
	ntex := fs.String("ntex", "", "optional cooked NTEX to embed in the N64 guest")
	inp := fs.String("inp", "", "optional cooked INP600.bin to embed in the N64 guest")
	sfx := fs.String("sfx", "", "optional cooked SFX00.wav (N64; toolchain runs audioconv64)")
	music := fs.String("music", "", "optional cooked MUSIC00.wav (N64; toolchain runs audioconv64)")
	pack := fs.String("pack", "", "optional cooked PACK01.bin (N64 extra DragonFS pack)")
	packDir := fs.String("pack-dir", "", "optional export dir with extra PACK/NODE/MESH/NTEX %02d.bin")
	vag := fs.String("vag", "", "optional cooked VAG to embed in the guest")
	sprite := fs.String("sprite", "", "optional cooked SPRITE table to embed in the guest")
	script := fs.String("script", "", "optional cooked SCRIPT.IR to embed in the guest")
	gdbc := fs.String("gdbc", "", "optional cooked SCRIPT.GD.BC to embed in the guest")
	luau := fs.String("luau", "", "optional cooked SCRIPT.LU.BC to embed in the guest")
	str := fs.String("str", "", "optional cooked FMV00.STR to embed in the guest")
	xa := fs.String("xa", "", "optional cooked FMV00.XA to embed in the guest")
	node := fs.String("node", "", "optional cooked NODE00.bin to embed in the guest")
	hud := fs.String("hud", "", "optional cooked HUD00.bin to embed in the guest")
	tile := fs.String("tile", "", "optional cooked TILE00.bin to embed in the guest")
	scene := fs.String("scene", "", "optional cooked SCENE00.bin to embed in the guest")
	anim := fs.String("anim", "", "optional cooked ANIM00.bin to embed in the guest")
	cam := fs.String("cam", "", "optional cooked CAM00.bin to embed in the guest")
	hit := fs.String("hit", "", "optional cooked HIT00.bin to embed in the guest")
	nav := fs.String("nav", "", "optional cooked NAV00.bin to embed in the guest")
	pathTbl := fs.String("path", "", "optional cooked PATH00.bin to embed in the guest")
	way := fs.String("way", "", "optional cooked WAY00.bin to embed in the guest")
	display := fs.String("display", settings.Current().N64.Display, "N64 framebuffer: 320 or 640")
	rumble := fs.Bool("rumble", settings.Current().N64.Rumble, "N64 Rumble Pak (P8 optional)")
	rdram := fs.String("rdram", settings.Current().N64.Rdram, "N64 RDRAM: 8 or 4")
	title := fs.String("title", "", "ps3: the game's name on the XMB (default: --out's name)")
	titleID := fs.String("title-id", "", "ps3: title ID, four letters and five digits (default BLAZ00001)")
	icon := fs.String("icon", "", "ps3: ICON0.PNG (320x176) for the XMB")
	if err := fs.Parse(args); err != nil {
		return report.Usage(err.Error())
	}
	return p.Build(ctx, platforms.BuildOptions{CommonOptions: base, Src: *src, Out: *out, Sample: *sample, Overlay: *overlay, ExportSrc: *exportSrc, Tim: *tim, Mesh: *mesh, Gtex: *gtex, Ntex: *ntex, Inp: *inp, Sfx: *sfx, Music: *music, Pack: *pack, PackDir: *packDir, Vag: *vag, Sprite: *sprite, Script: *script, Gdbc: *gdbc, Luau: *luau, Str: *str, Xa: *xa, Node: *node, Hud: *hud, Tile: *tile, Scene: *scene, Anim: *anim, Cam: *cam, Hit: *hit, Nav: *nav, Path: *pathTbl, Way: *way, Display: *display, Rumble: *rumble, Rdram: *rdram, Title: *title, TitleID: *titleID, Icon: *icon})
}

func runExportGuest(p platforms.Platform, args []string, base platforms.CommonOptions, stdout io.Writer) error {
	id := p.Info().ID
	if id != ps1.ID && id != ps2.ID && id != n64plat.ID {
		return report.Usage("export-guest is a ps1/ps2/n64 command")
	}
	fs := flag.NewFlagSet("export-guest", flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	out := fs.String("out", "", "directory to write bundled guest sources")
	if err := fs.Parse(args); err != nil {
		return report.Usage(err.Error())
	}
	dest := strings.TrimSpace(*out)
	var abi int
	var names []string
	var err error
	switch id {
	case ps2.ID:
		if dest == "" {
			dest = ps2.GuestDir(base.Prefix)
		}
		err = guestps2.Install(dest)
		abi = guestps2.CookABI
		names = guestps2.RuntimeNames
	case n64plat.ID:
		if dest == "" {
			dest = n64plat.GuestDir(base.Prefix)
		}
		err = guestn64.Install(dest)
		abi = guestn64.CookABI
		names = guestn64.RuntimeNames
	default:
		if dest == "" {
			dest = ps1.GuestDir(base.Prefix)
		}
		err = guestps1.Install(dest)
		abi = guestps1.CookABI
		names = guestps1.RuntimeNames
	}
	if err != nil {
		return err
	}
	if base.JSON {
		return json.NewEncoder(stdout).Encode(map[string]any{
			"out":   dest,
			"abi":   abi,
			"files": names,
		})
	}
	report.Line(stdout, report.Wrote, dest)
	for _, name := range names {
		fmt.Fprintf(stdout, "  %s\n", name)
	}
	return nil
}

func runRun(ctx context.Context, p platforms.Platform, args []string, base platforms.CommonOptions) error {
	fs := flag.NewFlagSet("run", flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	iso := fs.String("iso", "", "optional cue/bin path (ps1/ps2)")
	emu := fs.String("emu", settings.Current().N64.Emu, "n64 validator: ares|project64|both")
	timeout := fs.Duration("timeout", settings.Current().Smoke(), "stop the emulator after this duration (smoke)")
	pcdrv := fs.String("pcdrv", "", "host directory for pcsx-redux -pcdrvbase (default: EXE dir)")
	ui := fs.Bool("ui", false, "show the pcsx-redux window (default is -no-ui smoke)")
	rdram := fs.String("rdram", settings.Current().N64.Rdram, "N64 RDRAM: 8 or 4")
	if err := fs.Parse(args); err != nil {
		return report.Usage(err.Error())
	}
	exe := ""
	if fs.NArg() > 0 {
		exe = fs.Arg(0)
	}
	return p.Run(ctx, platforms.RunOptions{CommonOptions: base, Exe: exe, ISO: *iso, Emu: *emu, Timeout: *timeout, Pcdrv: *pcdrv, UI: *ui, Rdram: *rdram})
}

func runRom(ctx context.Context, p platforms.Platform, args []string, base platforms.CommonOptions) error {
	if p.Info().ID != n64plat.ID {
		return report.Usage("rom is an n64 command (no ISO/CUE)")
	}
	fs := flag.NewFlagSet("rom", flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	dir := fs.String("dir", "", "DragonFS tree (mkdfs)")
	elf := fs.String("elf", "", "guest ELF to wrap with n64tool")
	out := fs.String("out", "", "output .z64")
	if err := fs.Parse(args); err != nil {
		return report.Usage(err.Error())
	}
	tool, ok := p.(*n64plat.Tool)
	if !ok {
		return report.Usage("rom requires the n64 platform")
	}
	return tool.WriteROM(ctx, *dir, *elf, *out, base.Stdout, base.Stderr)
}

func runFMV(p platforms.Platform, args []string, base platforms.CommonOptions, stdout io.Writer) error {
	_ = p
	_ = args
	if base.JSON {
		return json.NewEncoder(stdout).Encode(map[string]any{
			"encoder":   "blazium-mit",
			"guest":     "DecDCTin + DecDCTout (MDEC RLE; no psxpress encode)",
			"spawn":     false,
			"in_editor": true,
			"note":      "Editor encodes STR/XA with MIT writers; guest DecDCTin only.",
		})
	}
	report.Line(stdout, report.Note, "ps1 FMV is in-editor MIT STR/XA encode (no psxpress spawn)")
	return nil
}

type stringList []string

func (s *stringList) String() string { return strings.Join(*s, ",") }
func (s *stringList) Set(v string) error {
	*s = append(*s, v)
	return nil
}

func runISO(ctx context.Context, p platforms.Platform, args []string, base platforms.CommonOptions) error {
	fs := flag.NewFlagSet("iso", flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	xml := fs.String("xml", "", "mkpsxiso project xml")
	dir := fs.String("dir", "", "VIDEO_TS parent directory")
	out := fs.String("out", "", "output path")
	volume := fs.String("volume", "", "ISO9660 volume label")
	bootELF := fs.String("boot-elf", "", "product-code name for the boot file on the disc (ps2: SLUS_000.01; ps1 --dir: BLAZ_000.01)")
	bootLogo := fs.String("boot-logo", "", "ps2: raw 8-bit grey boot logo (384x64 NTSC, 344x71 PAL) for the disc's logo sectors")
	title := fs.String("title", "", "movie / disc title")
	publisher := fs.String("publisher", "", "publisher identifier")
	preparer := fs.String("preparer", "", "data preparer")
	application := fs.String("application", "", "application identifier")
	system := fs.String("system", "", "system identifier")
	provider := fs.String("provider", "", "UDF provider")
	copyright := fs.String("copyright", "", "copyright text")
	copyrightFile := fs.String("copyright-file", "", "copyright file to pack")
	license := fs.String("license", "", "license text")
	licenseFile := fs.String("license-file", "", "license file to pack")
	readme := fs.String("readme", "", "readme text")
	readmeFile := fs.String("readme-file", "", "readme file to pack")
	credits := fs.String("credits", "", "credits text")
	creditsFile := fs.String("credits-file", "", "credits file to pack")
	author := fs.String("author", "", "author")
	studio := fs.String("studio", "", "studio")
	website := fs.String("website", "", "website")
	contact := fs.String("contact", "", "contact")
	version := fs.String("version", "", "version")
	catalog := fs.String("catalog", "", "catalog / SKU")
	description := fs.String("description", "", "synopsis")
	abstract := fs.String("abstract", "", "abstract text")
	abstractFile := fs.String("abstract-file", "", "abstract file")
	bibliographic := fs.String("bibliographic", "", "bibliographic text")
	biblioFile := fs.String("biblio-file", "", "bibliographic file")
	created := fs.String("created", "", "RFC3339 volume time")
	disc := fs.Int("disc", 0, "volume sequence number")
	discs := fs.Int("discs", 0, "volume set size")
	region := fs.Int("region-mask", 0, "DVD region mask (metadata)")
	parental := fs.Int("parental-level", 0, "parental level (metadata)")
	menuLang := fs.String("menu-language", "", "menu language (metadata)")
	audioLang := fs.String("audio-language", "", "audio language (metadata)")
	subLang := fs.String("subtitle-language", "", "subtitle language (metadata)")
	autorunLabel := fs.String("autorun-label", "", "AUTORUN.INF label")
	autorunOpen := fs.String("autorun-open", "", "AUTORUN.INF open")
	autorunIcon := fs.String("autorun-icon", "", "AUTORUN.INF icon")
	extrasDir := fs.String("extras-dir", "", "merge children onto disc root")
	recursive := fs.Bool("recursive", false, "walk nested extra folders")
	meta := fs.String("meta", "", "load disc.interdvd.json")
	writeMeta := fs.String("write-meta", "", "save resolved metadata JSON")
	var extras stringList
	fs.Var(&extras, "extra", "HOST or HOST:DISC (repeatable)")
	if err := fs.Parse(args); err != nil {
		return report.Usage(err.Error())
	}
	return p.ISO(ctx, platforms.ISOOptions{
		CommonOptions: base, XML: *xml, Dir: *dir, Out: *out, VolumeID: *volume, BootELF: *bootELF, BootLogo: *bootLogo,
		Extra: extras, ExtrasDir: *extrasDir, Recursive: *recursive,
		Title: *title, Publisher: *publisher, Preparer: *preparer, Application: *application,
		System: *system, Provider: *provider,
		Copyright: *copyright, CopyrightFile: *copyrightFile,
		License: *license, LicenseFile: *licenseFile,
		Readme: *readme, ReadmeFile: *readmeFile,
		Credits: *credits, CreditsFile: *creditsFile,
		Author: *author, Studio: *studio, Website: *website, Contact: *contact,
		Version: *version, Catalog: *catalog, Description: *description,
		Abstract: *abstract, AbstractFile: *abstractFile,
		Bibliographic: *bibliographic, BiblioFile: *biblioFile,
		Created: *created, Disc: *disc, Discs: *discs,
		RegionMask: *region, ParentalLevel: *parental,
		MenuLanguage: *menuLang, AudioLanguage: *audioLang, SubtitleLanguage: *subLang,
		AutorunLabel: *autorunLabel, AutorunOpen: *autorunOpen, AutorunIcon: *autorunIcon,
		Meta: *meta, WriteMeta: *writeMeta,
	})
}

func runElfInfo(p platforms.Platform, args []string, base platforms.CommonOptions, stdout io.Writer) error {
	if p.Info().ID != ps2.ID {
		return report.Usage("elf-info is a ps2 command")
	}
	if len(args) < 1 || strings.TrimSpace(args[0]) == "" {
		return report.Usage("ps2 elf-info <elf>")
	}
	path := args[0]
	text, err := ps2.TextSectionSize(path)
	if err != nil {
		return err
	}
	st, err := os.Stat(path)
	if err != nil {
		return err
	}
	if base.JSON {
		return json.NewEncoder(stdout).Encode(map[string]any{
			"path":           path,
			"text_bytes":     text,
			"file_bytes":     st.Size(),
			"text_max_bytes": settings.Current().PS2.ElfTextMax,
			"file_max_bytes": settings.Current().PS2.ElfFileMax,
		})
	}
	fmt.Fprintf(stdout, "text_bytes=%d\n", text)
	fmt.Fprintf(stdout, "file_bytes=%d\n", st.Size())
	s := settings.Current().PS2
	if s.ElfTextMax > 0 && int64(text) > s.ElfTextMax {
		return report.Fail(fmt.Sprintf(".text is %d bytes (max %d)", text, s.ElfTextMax), "raise ps2.elf_text_max in blazium-toolchain.yml or shrink the guest")
	}
	if s.ElfFileMax > 0 && st.Size() > s.ElfFileMax {
		return report.Fail(fmt.Sprintf("ELF is %d bytes (max %d)", st.Size(), s.ElfFileMax), "raise ps2.elf_file_max in blazium-toolchain.yml or shrink the guest")
	}
	return nil
}

func runChd(ctx context.Context, p platforms.Platform, args []string, base platforms.CommonOptions) error {
	if p.Info().ID != ps2.ID {
		return report.Usage("chd is a ps2 command")
	}
	fs := flag.NewFlagSet("chd", flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	isoPath := fs.String("iso", "", "ISO9660 image (not .cue)")
	out := fs.String("out", "", "output .chd path")
	if err := fs.Parse(args); err != nil {
		return report.Usage(err.Error())
	}
	return ps2.WriteCHD(ctx, *isoPath, *out, base.Stdout, base.Stderr)
}

func runEncode(ctx context.Context, p platforms.Platform, name string, args []string, base platforms.CommonOptions) error {
	tool, ok := p.(*interdvd.Tool)
	if !ok {
		return report.Usage(name + " is an interdvd command")
	}
	if len(args) > 0 && args[0] == "--" {
		args = args[1:]
	}
	return tool.RunTool(ctx, name, args, base)
}

func runMeta(p platforms.Platform, args []string, base platforms.CommonOptions, stdout io.Writer) error {
	if p.Info().ID != interdvd.ID {
		return report.Usage("meta is an interdvd command")
	}
	if len(args) == 0 {
		return report.Usage("interdvd meta <init|validate>")
	}
	switch args[0] {
	case "init":
		fs := flag.NewFlagSet("meta init", flag.ContinueOnError)
		fs.SetOutput(io.Discard)
		out := fs.String("out", "", "metadata JSON path")
		if err := fs.Parse(args[1:]); err != nil || *out == "" {
			return report.Usage("interdvd meta init --out FILE")
		}
		if err := iso.WriteMeta(*out, iso.InitMeta()); err != nil {
			return err
		}
		if base.JSON {
			return json.NewEncoder(stdout).Encode(map[string]string{"out": *out, "schema": iso.SchemaV1})
		}
		report.Line(stdout, report.Wrote, *out)
		return nil
	case "validate":
		fs := flag.NewFlagSet("meta validate", flag.ContinueOnError)
		fs.SetOutput(io.Discard)
		meta := fs.String("meta", "", "metadata JSON path")
		if err := fs.Parse(args[1:]); err != nil || *meta == "" {
			return report.Usage("interdvd meta validate --meta FILE")
		}
		m, err := iso.LoadMeta(*meta)
		if err != nil {
			return err
		}
		if err := iso.ValidateMeta(m); err != nil {
			return err
		}
		resolved, err := m.Resolve()
		if err != nil {
			return err
		}
		if base.JSON {
			return json.NewEncoder(stdout).Encode(resolved)
		}
		report.Line(stdout, report.Ready, "meta valid")
		return nil
	default:
		return report.Usage("interdvd meta <init|validate>")
	}
}

func mapErr(err error, stderr io.Writer, ctx report.Context) int {
	if err == nil {
		return ExitOK
	}
	report.WriteError(stderr, err, ctx)
	return report.ExitCode(err)
}

func usage(w io.Writer) {
	ps2Line := "ps2       supported (PlayStation 2; Windows/Linux; guest bundled in this CLI)"
	if !ps2.HostSupported() {
		ps2Line = fmt.Sprintf("ps2       supported (PlayStation 2; compile/run unavailable on %s; env/status/export-guest only)", runtime.GOOS)
	}
	n64Line := "n64       supported (Nintendo 64; Windows/Linux; guest bundled; product .z64)"
	if !n64plat.HostSupported() {
		n64Line = fmt.Sprintf("n64       supported (Nintendo 64; compile/run unavailable on %s; env/status/export-guest only)", runtime.GOOS)
	}
	fmt.Fprint(w, strings.TrimSpace(fmt.Sprintf(`
blazium-toolchain — official Blazium console toolchain manager

Usage:
  blazium-toolchain [--json] [--prefix DIR] [--settings FILE] version
  blazium-toolchain [--json] [--prefix DIR] [--settings FILE] list
  blazium-toolchain [--json] [--settings FILE] settings
  blazium-toolchain [--json] [--prefix DIR] [--settings FILE] <platform> <command> [flags]

Project YAML: --settings, $BLAZIUM_TOOLCHAIN_SETTINGS, or ./blazium-toolchain.yml (walk up).

Platforms:
  ps1       supported (PlayStation 1)
  %s
  %s
  interdvd  supported (Interactive DVD ISO9660+UDF)
  steam     supported (Valve steamcmd cache for blazium-cli deploy)
  ps3       supported (PlayStation 3 homebrew; any host, no SDK; guest bundled; product .pkg)
  ps4       planned

PS1/PS2/N64 host tools: Windows and Linux only. setup/build/run/iso/rom exit 2 on other OSes.

PS1 commands:
  setup [--profile compile|dev|iso] [--offline]
  env
  status
  build --out FILE [--src DIR | --sample template|gte] [--overlay DIR] [--export-src DIR]
    [--tim|--mesh|--vag|--sprite|--script|--gdbc|--luau|--str|--xa|--node|--hud|--tile|--scene|--anim|--cam|--hit|--nav|--path|--way]
  export-guest [--out DIR]
  run [--iso CUE] [--timeout 120s] [--ui] [GAME.EXE]
  iso --xml FILE [--out PATH]
  fmv

PS2 commands:
  setup [--profile compile|dev|iso] [--offline]
  env
  status
  build --out FILE.elf [--src DIR | --sample cube] [--overlay DIR] [--export-src DIR]
    [--node|--mesh|--gtex|--script]
  export-guest [--out DIR]
  run [--iso FILE.iso] [--timeout 120s] [--ui] [GAME.elf]
  iso --dir TREE --out FILE.iso
  elf-info FILE.elf
  chd --iso FILE.iso --out FILE.chd

Steam commands (steamcmd only; upload lives in blazium-cli):
  setup [--offline]
  env
  status

Interactive DVD commands:
  setup [--offline]
  env
  status
  ffmpeg -- <args>
  ffprobe -- <args>
  iso --dir DIR --out FILE [--meta FILE] [--write-meta FILE] [--volume ID] [--title TEXT]
      [--copyright TEXT] [--license TEXT] [--extra HOST[:DISC]] [--extras-dir DIR] [--recursive]
  meta init --out FILE
  meta validate --meta FILE

N64 commands:
  setup [--profile compile|dev|rom] [--offline]
  env
  status
  build --out FILE.z64 [--src DIR | --sample helloworld|rdpqdemo|t3dquad|ovldemo] [--overlay DIR] [--export-src DIR]
    [--display 320|640] [--rumble] [--rdram 8|4] [--ntex|--mesh|--node|--inp|--sfx|--music|--pack|--pack-dir|--script|--gdbc|--luau]
  export-guest [--out DIR]
  run [--emu ares|project64|both] [--timeout 120s] [--rdram 8|4] GAME.z64
  rom --dir TREE --out FILE.z64 [--elf FILE.elf]

PS3 commands:
  setup [--profile prebuilt|compile] [--offline]   (compile: ps3dev for --overlay, Linux)
  env
  status
  build --sample gdir --out GAME.pkg|DIR [--pack-dir EXPORT] [--title NAME] [--title-id BLAZ00001] [--icon ICON0.PNG] [--overlay DIR]
  run [--timeout 120s] GAME_FOLDER|EBOOT.BIN|GAME.pkg

Cache: <prefix>/<plat>/ (inst, src, guest, work). Portable copy: <exeDir>/<plat>/.
License: GPL-3.0-or-later (this repo may contain GCC, PSn00bSDK, mkpsxiso, pcsx-redux, ps2dev, libdragon, Ares).
The 3rd-party installer should invoke this binary (not the Blazium editor).
`, ps2Line, n64Line))+"\n")
}
