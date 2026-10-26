# blazium-toolchain

A CLI that fetches console compilers into a cache and builds PS1, PS2, PS3, GameCube, N64, and Interactive DVD products. GPL-3.0-or-later; the MIT Blazium editor only spawns the binary. `ps4` is reserved and exits `2`.

## Blazium Engine

[Blazium Engine](https://blazium.app) is the free MIT game engine. The editor and the tools in the table below belong to it. `blazium-cli` installs editors, manages projects, remote-controls a running editor, and deploys to Steam and itch.io. Two lines ship: `blazium-dev` is the Godot 4.3+ line, and `blazium_4.8` is the Godot 4.8+ line. Hub, the crash reporter, skills, and subagents track `blazium_4.8`.

## Blazium Games

[Blazium Games](https://blazium.games) is a separate store, operated by Divine Games, Inc. It is the platform for playing and publishing games, applications, mods, and assets. A game on that store does not have to be made with the Blazium engine, and the engine does not require that store. `chauffeur` is the only upload tool for Blazium Games. Docs are at [docs.blazium.games](https://docs.blazium.games).

## Community

- Official website: [https://blazium.app/](https://blazium.app/)
- IndieDB blog: [https://www.indiedb.com/engines/blazium-engine](https://www.indiedb.com/engines/blazium-engine)
- Official community: [Discord](https://blazium.app/chat)
- Docs: [docs.blazium.app](https://docs.blazium.app)

## Ecosystem

| Product | Role | Release |
|---------|------|---------|
| [Engine](https://github.com/blazium-games/blazium) | The editor. Two lines: `blazium-dev` (Godot 4.3+) and `blazium_4.8` (Godot 4.8+). Hub, crash reporter, skills, and subagents track `blazium_4.8`. | [blazium.app/download](https://blazium.app/download) |
| [CLI](https://github.com/blazium-games/blazium-cli) | Install editors, projects, remote control, Steam and itch.io deploy. Not the Games uploader. | Linux and Windows, x86_64 and x86_32. Catalog: [cli.json](https://cdn.blazium.app/cli/cli.json) |
| [Hub](https://github.com/blazium-games/blazium-hub) | Desktop companion; installers bundle the CLI. Engine builds track `blazium_4.8`. | Linux and Windows, x86_64 and x86_32. |
| [Crash reporter](https://github.com/blazium-games/blazium_crash_reporter) | Sidecar UI for engine and Hub crash reports. Engine builds track `blazium_4.8`. | Linux and Windows, x86_64 and x86_32. Catalog: [crash_reporter.json](https://cdn.blazium.app/crash_reporter/crash_reporter.json) |
| [Toolchain](https://github.com/blazium-games/blazium-toolchain) | PS1, PS2, PS3, GameCube, N64, and Interactive DVD. `ps4` is reserved and does not ship. | Linux and Windows, x86_64 and x86_32. Catalog: [toolchain.json](https://cdn.blazium.app/toolchain/toolchain.json) |
| [Skills](https://github.com/blazium-games/blazium-skills) | Agent skill packs for Claude, Cursor, Codex, and Grok. Own semver, separate from the 0.8.x API baseline. | Catalog: [skills.json](https://cdn.blazium.app/skills/skills.json) |
| [Subagents](https://github.com/blazium-games/blazium-subagents) | Studio roster that loads those skills. Own semver. | Catalog: [subagents.json](https://cdn.blazium.app/subagents/subagents.json) |
| [Blazium Games](https://blazium.games) | Separate store. Upload with chauffeur, not with these tools. | Site [blazium.games](https://blazium.games), docs [docs.blazium.games](https://docs.blazium.games). |

How this engine differs from Godot, Redot, Unity, and Unreal is in the [engine README](https://github.com/blazium-games/blazium).

```
blazium-toolchain [--json] [--prefix DIR] version
blazium-toolchain [--json] list
blazium-toolchain [--json] [--prefix DIR] ps1 setup [--profile compile|dev|iso] [--offline]
blazium-toolchain [--json] [--prefix DIR] ps1 env
blazium-toolchain [--json] [--prefix DIR] ps1 status
blazium-toolchain [--prefix DIR] ps1 build --out FILE [--src DIR | --sample template|gte]
  [--overlay DIR] [--export-src DIR]
  [--tim|--mesh|--vag|--sprite|--script|--gdbc|--luau|--str|--xa|--node|--hud|--tile|--scene|--anim|--cam|--hit|--nav|--path|--way]
blazium-toolchain [--prefix DIR] ps1 export-guest [--out DIR]
blazium-toolchain [--prefix DIR] ps1 run [--iso CUE] [--timeout 120s] [--ui] GAME.EXE
blazium-toolchain [--prefix DIR] ps1 iso --xml FILE [--out PATH]
blazium-toolchain [--json] [--prefix DIR] ps1 fmv
blazium-toolchain [--json] [--prefix DIR] interdvd setup [--offline]
blazium-toolchain [--json] [--prefix DIR] interdvd env
blazium-toolchain [--json] [--prefix DIR] interdvd status
blazium-toolchain [--prefix DIR] interdvd ffmpeg -- <args>
blazium-toolchain [--prefix DIR] interdvd ffprobe -- <args>
blazium-toolchain interdvd iso --dir DIR --out FILE [--meta FILE] [--write-meta FILE] [--extra HOST[:DISC]] [--recursive]
blazium-toolchain interdvd meta init --out disc.interdvd.json
blazium-toolchain interdvd meta validate --meta disc.interdvd.json
blazium-toolchain [--prefix DIR] ps2 setup [--profile compile|dev|iso] [--offline]
blazium-toolchain [--prefix DIR] ps2 env
blazium-toolchain [--prefix DIR] ps2 status
blazium-toolchain [--prefix DIR] ps2 build --out FILE.elf [--src DIR | --sample cube]
  [--overlay DIR] [--export-src DIR] [--node|--mesh|--gtex|--script]
blazium-toolchain [--prefix DIR] ps2 export-guest [--out DIR]
blazium-toolchain [--prefix DIR] ps2 run [--iso FILE.iso] [--timeout 120s] [--ui] GAME.elf
blazium-toolchain [--prefix DIR] ps2 iso --dir TREE --out FILE.iso
blazium-toolchain [--prefix DIR] ps2 elf-info FILE.elf
blazium-toolchain [--prefix DIR] ps2 chd --iso FILE.iso --out FILE.chd
blazium-toolchain [--prefix DIR] n64 setup [--profile compile|dev|rom] [--offline]
blazium-toolchain [--prefix DIR] n64 env
blazium-toolchain [--prefix DIR] n64 status
blazium-toolchain [--prefix DIR] n64 build --out FILE.z64 [--src DIR | --sample helloworld|rdpqdemo|t3dquad|ovldemo]
  [--overlay DIR] [--export-src DIR] [--display 320|640] [--rumble] [--rdram 8|4]
  [--ntex|--mesh|--node|--inp|--sfx|--music|--pack|--pack-dir|--script|--gdbc|--luau]
blazium-toolchain [--prefix DIR] n64 export-guest [--out DIR]
blazium-toolchain [--prefix DIR] n64 run [--emu ares|project64|both] [--timeout 120s] [--rdram 8|4] GAME.z64
blazium-toolchain [--prefix DIR] n64 rom --dir TREE --out FILE.z64 [--elf FILE.elf]
```

`n64` has no ISO path. The product is a big-endian `.z64`. `n64 iso` is rejected.

## Install

Linux and Windows, x64 and ia32:

```text
npx @blazium-engine/toolchain
npm install -g @blazium-engine/toolchain
```

The command is `blazium-toolchain`. Install `@blazium-engine/toolchain`. License: GPL-3.0-or-later.

npm installs one of these optional binaries for the current platform:

| Package | Platform | CPU |
|---------|----------|-----|
| `@blazium-engine/toolchain-linux-x64` | linux | x64 |
| `@blazium-engine/toolchain-linux-ia32` | linux | ia32 |
| `@blazium-engine/toolchain-win32-x64` | win32 | x64 |
| `@blazium-engine/toolchain-win32-ia32` | win32 | ia32 |

When that optional package is installed, its binary is used and nothing is downloaded. The CDN download runs only if that package is absent.

From source:

```
git clone https://github.com/blazium-games/blazium-toolchain.git
cd blazium-toolchain
go test ./...
go build -o blazium-toolchain.exe ./cmd/blazium-toolchain
```

Drop `.exe` on Unix. Go 1.23.8 or later. Published binaries are Linux and Windows, x86_64 and x86_32: [toolchain.json](https://cdn.blazium.app/toolchain/toolchain.json). A local `go build` on macOS can still master Interactive DVD; CI does not ship a macOS binary.

## Project settings

Copy [blazium-toolchain.yml](blazium-toolchain.yml) into a game repo, or use `.blazium-toolchain.yml`. The CLI looks for `--settings FILE`, then `$BLAZIUM_TOOLCHAIN_SETTINGS`, then those two filenames walking up from cwd.

Omitted keys keep built-in defaults. `blazium-toolchain settings` dumps the resolved file (`--json settings` for a machine). SHA-pinned PS1 and FFmpeg zips stay in the embedded `pins.json`.

Knobs you actually change per game: smoke/download timeouts, `download_max_bytes`, default profiles, N64 display/RDRAM/emu/rom title, PS2 ISO labels and SYSTEM.CNF, size gates (`ps2.elf_*_max`, `n64.cart_max`), fetch URLs, and host search paths.

`--tim`, `--node`, and the other cook flags read the file you name. The CLI does not sandbox those paths.

`interdvd` `menu_language`, `audio_language`, `subtitle_language`, `region_mask`, and `parental_level` are stored in `disc.interdvd.json`. They are not written into IFO or UDF.

## How it talks

Stdout is process status. One verb per line:

```
fetching   libdragon toolchain  https://…
installing libdragon preview  <prefix>/n64/inst
skip       project64  PROJECT64_EXE missing
wrote      hello.z64
ready      n64 compile
```

A failure prints a block you can paste into a GitHub issue:

```
error      missing-tool
           n64 compile needs N64_INST (mips64-elf-gcc, n64.mk, libdragon.a)
fix        blazium-toolchain n64 setup --profile compile
---
command    blazium-toolchain n64 build --out hello.z64
host       windows/amd64
version    blazium-toolchain 0.1.0
prefix     C:\Users\…\Blazium\blazium-toolchain
```

`--json` writes the same fields as one object. There is no session log.

## Cache

| Item | Use |
|------|-----|
| [manifest.json](manifest.json) | Name, version, platforms, commands (also embedded) |
| `blazium-toolchain --json version` | `{ "name", "version", "license" }` |
| `blazium-toolchain --json list` | Platform catalog |
| Exit `2` | Reserved platform, or this host cannot compile/run |
| Exit `3` | Host tool missing |
| `--prefix` / `BLAZIUM_TOOLCHAIN_PREFIX` | Cache root |

Default cache: `%LOCALAPPDATA%\Blazium\blazium-toolchain` (Windows), `~/.local/share/blazium-toolchain` (elsewhere).

```
<prefix>/
  ps1/gcc/  psn00bsdk/  pcsx-redux/  openbios/  cmake/  ninja/
  ps1/guest/runtime/    work/    state.json
  ps2/ps2dev/           guest/runtime/    work/    state.json
  n64/inst/             src/libdragon/    src/tiny3d/    ares/
  n64/guest/runtime/    work/    state.json
  interdvd/ffmpeg/      state.json
```

Lookup order: tool env, then `<prefix>/<plat>/`, then `<exeDir>/<plat>/`, then PATH.

The MIT guests (CMake/Makefile + C++) live inside the CLI. `ps1|ps2|n64 export-guest --out DIR` writes the stub. `build` without `--src` uses that stub. `--overlay DIR` copies extra or replacement `*.cpp` on top. `--export-src DIR` writes the resolved tree.

Compilers are not in the binary. `ps1 setup`, `ps2 setup`, and `n64 setup` fetch upstream zips into `<prefix>/<plat>/`. Sony BIOS images and N64 PIF dumps are not fetched. `C:\ultra` is ignored.

## PS1

| Profile | What you get |
|---------|--------------|
| `compile` | gcc + PSn00bSDK 0.24 + elf2x |
| `dev` | compile + OpenBIOS + pcsx-redux CLI |
| `iso` | dev + mkpsxiso |

`ps1 setup --profile compile` fetches PSn00bSDK v0.24 (GCC 12.3.0 + SDK/`elf2x`) when they are missing. `--offline` skips the network.

`ps1 setup --profile dev` also fetches the pcsx-redux Windows x64 CLI nightly and copies `openbios.bin` into `<prefix>/ps1/openbios/`. `ps1 run` smokes that CLI. Default is headless (`-no-ui`); `--ui` opens the window.

`ps1 build --sample template` builds the official zip template to a `PS-X EXE`. CMake and Ninja come from PATH, or a Windows fetch into the cache.

## PS2

| Profile | What you get |
|---------|--------------|
| `compile` | ps2dev EE gcc + PS2SDK |
| `dev` | compile + your `PCSX2_EXE` (BIOS is not fetched) |
| `iso` | dev + `ps2 iso --dir TREE` |

`ps2 setup --profile compile` fetches the ps2dev host tarball into `<prefix>/ps2/ps2dev/`. On Windows it also stages the MinGW DLLs that tarball leaves out.

Set `PCSX2_EXE` (and `PS2_BIOS_DIR` if you have a legal BIOS). `ps2 run` smokes the ELF; `--ui` shows PCSX2.

`ps2 build` without `--src` links the bundled EE guest. Sample `cube` comes from ps2sdk. Size gates (`ps2.elf_text_max`, `ps2.elf_file_max`) apply to `ps2 build` and `ps2 elf-info`. `ps2 chd` wraps an ISO with chdman when that tool is on PATH.

## N64

| Profile | What you get |
|---------|--------------|
| `compile` | libdragon preview `N64_INST` + `mips64-elf-gcc` + `n64.mk` + `libdragon.a` |
| `dev` | compile + Ares and/or Project64 (a missing emulator is a skip) |
| `rom` | compile + `mkdfs` / `n64tool` |

`n64 setup --profile compile` fetches the libdragon mips64-elf toolchain into `<prefix>/n64/inst/` (Windows zip or Linux `.deb`, extracted in-prefix), preview sources into `<prefix>/n64/src/libdragon/`, Tiny3D into `<prefix>/n64/src/tiny3d/`, then builds libdragon. Samples (`helloworld`, `rdpqdemo`, `t3dquad`, `ovldemo`) resolve from that cache.

`n64 setup --profile dev` also fetches Ares v148 on Windows x64. Linux has no zip in that release: install `ares` and set `ARES_EXE`. Project64 is discovered via `PROJECT64_EXE`. `n64 run` uses Parallel-RDP or Angrylion, not Jabo.

`n64 build` without `--src` links the bundled rdpq guest and writes a big-endian `.z64` (`80 37 12 40`).

| Flag | Effect |
|------|--------|
| `--display 320\|640` | Framebuffer; default 320 |
| `--rumble` | Drive a Rumble Pak when present |
| `--rdram 8\|4` | 8 MiB Expansion Pak (default) or 4 MiB so Ares can boot without the pak |
| `--sample helloworld\|rdpqdemo\|t3dquad\|ovldemo` | libdragon / Tiny3D samples |
| `--ntex/--mesh/--node/--inp/--sfx/--music/--pack/--pack-dir` | Cooked slices / DragonFS extras |

`n64 run --emu both` (default) needs both validators when both are installed. A missing single emulator is a skip. `--rdram 4` matches the 4 MiB build.

`n64 rom --dir TREE` packs a DragonFS tree with `mkdfs` and wraps an ELF with `n64tool`.

## PS3

`ps3 build --sample gdir` needs no PS3 SDK, on Windows, Linux or macOS. The GDIR guest comes compiled with the CLI (`guest/ps3/prebuilt`, built with PSL1GHT), and the CLI itself signs it as an NPDRM SELF (`EBOOT.BIN`) and writes `PARAM.SFO` and the `.pkg`, as PSL1GHT's `make_self_npdrm`, `sfo` and `pkg` do. It takes a PS2 GDIR export (`--pack-dir`, else the folder `--out` is in). The RSX draws the PS2 packs as they are, at the TV's resolution (720p, else 480p/576p), with the HUD in the PS2's 640x448 space at 4:3 in the middle. It writes:

- `--out GAME.pkg`: the installable package, with the game folder `TITLEID/` beside it (`PARAM.SFO`, `ICON0.PNG`, `USRDIR/EBOOT.BIN`, `USRDIR/DATA/`).
- `--out DIR`: `DIR/TITLEID.pkg` and `DIR/TITLEID/`.

| Flag | Effect |
|------|--------|
| `--title NAME` | The name on the XMB; default `--out`'s name |
| `--title-id BLAZ00001` | Four letters, five digits; also the save folder |
| `--icon ICON0.PNG` | 320x176 icon; default the export's `ICON0.PNG`, else a Blazium one |

Only a guest with code of your own (`--overlay`, or `GDIR_EXTRA_FLAGS`) is compiled, with ps3dev, which is built for Linux: `ps3 setup --profile compile` fetches ps3toolchain's ps3dev release into `<prefix>/ps3/inst/ps3dev/`, or set `PS3DEV`. After changing `guest/ps3/gdir/runtime`, rebuild the bundled guest with `scripts/ps3-guest-prebuilt.sh` (`go test ./guest/ps3` checks that it matches the sources).

Saves go to `BLAZIUM.SAV` in the game's `USRDIR`. Install the `.pkg` on a PS3 that runs homebrew (HEN or CFW), or in RPCS3 (File > Install Packages, with the PS3 system software installed). `ps3 run` starts `RPCS3_EXE` on the game folder or its `EBOOT.BIN`.

## GameCube

`gamecube build --sample gdir` needs no GameCube SDK, on Windows, Linux or macOS. The GDIR guest comes compiled with the CLI (`guest/gamecube/prebuilt`: `gdir.dol` and the disc's apploader, built with devkitPPC and libogc), and the CLI itself writes the GameCube disc image (GCM: disc header, apploader, DOL, file system table) and its `opening.bnr` banner. It takes a PS2 GDIR export (`--pack-dir`, else the folder `--out` is in); the cooked files go into the disc's `DATA/` folder as they are. The GX draws the PS2 packs at 640x480, with the HUD in the PS2's 640x448 space. It writes:

- `--out GAME.iso`: the disc image, with `GAME.THIRD-PARTY-NOTICES.txt` beside it.
- `--out DIR`: `DIR/GAMEID.iso`.

| Flag | Effect |
|------|--------|
| `--title NAME` | The disc's and the banner's name; default `--out`'s name |
| `--title-id GBZEBZ` | The game ID: a four-character game code (its fourth letter is the region: E America, P Europe, J Japan) and a two-character maker; also the Memory Card file's owner |
| `--icon BANNER.PNG` | The banner picture, scaled to 96x32; default the export's `BANNER.PNG`, else a plain one |

Saves go to the file `BLAZIUM` on the Memory Card in slot A (one block). Pads 1 to 4 are GameCube controllers: A cross, B circle, X square, Y triangle, L/R L1/R1 (and the analogue triggers), Z select, Start start; the control stick steers and, pushed well over, also works the d-pad in menus (Dolphin puts the arrow keys on it). Play the `.iso` in Dolphin, or on a console with Swiss or another disc loader. `gamecube run` starts `DOLPHIN_EXE` on it.

Only a guest with code of your own (`--overlay`, or `GDIR_EXTRA_FLAGS`) is compiled, with devkitPPC and libogc (devkitPro's `gamecube-dev`; set `DEVKITPRO`). After changing `guest/gamecube`, rebuild the bundled guest with `scripts/gamecube-guest-prebuilt.sh` (`go test ./guest/gamecube` checks that it matches the sources).

## Hosts

PS1/PS2/N64 setup, build, run, iso, and rom are Windows and Linux. PS3 and GameCube build anywhere (a custom PS3 guest compiles on Linux only). Other hosts exit `2`. `env`, `status`, and `export-guest` still work. Building the CLI locally on macOS can still master Interactive DVD. Published release binaries are Linux and Windows only.

CI runs `go test ./...`, `go vet ./...`, `ps2 build --out hello.elf`, and `n64 build --out hello.z64` on Ubuntu and Windows.

## Interactive DVD

`interdvd iso` masters ISO9660 + UDF 1.02 from a folder that already has `VIDEO_TS/` (and optional `AUDIO_TS/`). No mkisofs. Extra PC files (`--extra`, `--recursive`) sit beside the video folders. Title and related fields live in `disc.interdvd.json`.

`interdvd setup` can fetch pinned FFmpeg. `interdvd ffmpeg` / `ffprobe` spawn that binary.

## License

[GPL-3.0-or-later](LICENSE). Each fetched tree keeps its upstream license; see [THIRDPARTY.md](THIRDPARTY.md). The editor stays MIT and only spawns this binary. libpsn00b / ps2sdk / libdragon link into the exported guest, not the editor.

[CONTRIBUTING.md](CONTRIBUTING.md), [SECURITY.md](SECURITY.md).
