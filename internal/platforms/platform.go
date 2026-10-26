package platforms

import (
	"context"
	"io"
	"time"
)

// Status is the public lifecycle of a console target.
type Status string

const (
	StatusSupported Status = "supported"
	StatusPlanned   Status = "planned"
)

// Info is listed by `blazium-toolchain list` and the 3rd-party downloader.
type Info struct {
	ID          string   `json:"id"`
	Name        string   `json:"name"`
	Status      Status   `json:"status"`
	Commands    []string `json:"commands"`
	Description string   `json:"description"`
}

// EnvMap is toolchain paths the editor or a parent installer can consume.
type EnvMap map[string]string

// Platform is one console family. PS1, PS2, PS3, N64 and the DS are implemented; others return ErrPlanned.
type Platform interface {
	Info() Info
	Setup(ctx context.Context, opts SetupOptions) error
	Env(opts CommonOptions) (EnvMap, error)
	Status(opts CommonOptions) (map[string]any, error)
	Build(ctx context.Context, opts BuildOptions) error
	Run(ctx context.Context, opts RunOptions) error
	ISO(ctx context.Context, opts ISOOptions) error
}

// CommonOptions apply to most commands.
type CommonOptions struct {
	Prefix string
	JSON   bool
	Stdout io.Writer
	Stderr io.Writer
}

// SetupOptions selects a download/layout profile.
type SetupOptions struct {
	CommonOptions
	Profile string
	Offline bool
}

// BuildOptions compile a guest with the platform SDK (spawn only).
type BuildOptions struct {
	CommonOptions
	Src       string
	Out       string
	Sample    string // template | gte; used when Src is empty
	Overlay   string // optional extra/replacement *.cpp on top of Src or the bundled stub
	ExportSrc string // write the resolved guest C++ tree here (export-guest snapshot)
	Tim       string // optional cooked TIM to embed (PS1)
	Mesh      string // optional cooked mesh (PS1 SVECTOR / PS2 EE-native / N64)
	Gtex      string // optional cooked GTEX (PS2 GS PSM; do not use Tim)
	Ntex      string // optional cooked NTEX (N64; do not use Tim/Gtex)
	Inp       string // optional cooked INP600.bin (N64 InputMap)
	Sfx       string // optional cooked SFX00.wav (N64; audioconv64 → wav64)
	Music     string // optional cooked MUSIC00.wav (N64; audioconv64 → wav64)
	Pack      string // optional cooked PACK01.bin (N64 extra rom:// pack)
	PackDir   string // optional export dir with PACK/NODE/MESH/NTEX %02d extras
	Vag       string // optional cooked VAG to embed (P5+)
	Sprite    string // optional cooked SPRITE table to embed (P8+)
	Script    string // optional cooked SCRIPT.IR / SCRP
	Gdbc      string // optional cooked SCRIPT.GD.BC
	Luau      string // optional cooked SCRIPT.LU.BC
	Str       string // optional cooked FMV00.STR
	Xa        string // optional cooked FMV00.XA
	Node      string // optional cooked NODE00.bin
	Hud       string // optional cooked HUD00.bin
	Tile      string // optional cooked TILE00.bin
	Scene     string // optional cooked SCENE00.bin
	Anim      string // optional cooked ANIM00.bin
	Cam       string // optional cooked CAM00.bin
	Hit       string // optional cooked HIT00.bin
	Nav       string // optional cooked NAV00.bin
	Path      string // optional cooked PATH00.bin
	Way       string // optional cooked WAY00.bin
	Display   string // N64 framebuffer: 320 (default) or 640
	Rumble    bool   // N64 Rumble Pak (P8 optional)
	Rdram     string // N64 RDRAM: 8 (default Expansion Pak) or 4 (no-pak)
	Title     string // PS3: the game's name on the XMB (default: --out's name)
	TitleID   string // PS3: the title ID, four letters and five digits (BLAZ00001)
	Icon      string // PS3: ICON0.PNG, 320x176 (default: the export's, else PSL1GHT's)
}

// RunOptions boot the emulator.
type RunOptions struct {
	CommonOptions
	Exe     string
	ISO     string
	Emu     string        // n64: ares|project64|both
	Timeout time.Duration // 0 means default 120s smoke watchdog
	Pcdrv   string        // host dir for -pcdrvbase; empty uses the EXE directory
	UI      bool          // show the pcsx-redux window; smoke stays headless
	Rdram   string        // N64 RDRAM: 8 (default Expansion Pak) or 4 (no-pak)
}

// ISOOptions spawn an ISO packer (ps1) or master a DVD-Video bridge (interdvd).
type ISOOptions struct {
	CommonOptions
	XML      string
	Dir      string
	Out      string
	VolumeID string
	// BootELF is the product-code name the boot ELF gets on a PS2 disc
	// (SLUS_000.01); empty uses ps2.default_elf, else SLUS_000.01.
	BootELF string
	// BootLogo is a raw 8-bit grey PS2 boot logo (384x64 NTSC, 344x71 PAL)
	// for a PS2 disc's first 12 sectors; empty leaves them blank.
	BootLogo                                                                              string
	Extra                                                                                 []string
	ExtrasDir                                                                             string
	Recursive                                                                             bool
	Title, Publisher, Preparer, Application, System, Provider                             string
	Copyright, CopyrightFile, License, LicenseFile, Readme, ReadmeFile                    string
	Credits, CreditsFile, Author, Studio, Website, Contact, Version, Catalog, Description string
	Abstract, AbstractFile, Bibliographic, BiblioFile                                     string
	Created                                                                               string
	Disc, Discs                                                                           int
	RegionMask, ParentalLevel                                                             int
	MenuLanguage, AudioLanguage, SubtitleLanguage                                         string
	AutorunLabel, AutorunOpen, AutorunIcon                                                string
	Meta, WriteMeta                                                                       string
}
