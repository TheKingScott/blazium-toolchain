# Third-party components

Each tree keeps its upstream license. This CLI spawns compilers, packers, and emulators; it does not statically link them into Blazium.

| Component | Upstream license | Where it lives | Role |
|-----------|------------------|----------------|------|
| This CLI | GPL-3.0-or-later | this repo | Manager |
| mipsel-none-elf GCC | GPLv3 (unmodified binaries; sources from GNU) | `<prefix>/ps1/gcc/` | Spawned at `ps1 build` |
| PSn00bSDK / libpsn00b | MPL 2.0 | `<prefix>/ps1/psn00bsdk/` | Linked into the guest only |
| elf2x | PSn00bSDK / MPL 2.0 | `<prefix>/ps1/psn00bsdk/` or `ps1/elf2x/` | Spawned |
| mkpsxiso | GPLv2 or later | `<prefix>/ps1/mkpsxiso/` | Spawned by `ps1 iso` |
| pcsx-redux | GPLv2 | `<prefix>/ps1/pcsx-redux/` | Spawned by `ps1 run` |
| OpenBIOS | pcsx-redux tree | `<prefix>/ps1/openbios/` | No Sony BIOS |
| CMake | BSD-3 | `<prefix>/ps1/cmake/` | Spawned at `ps1 build` if not on PATH |
| Ninja | Apache-2.0 | `<prefix>/ps1/ninja/` | Spawned at `ps1 build` if not on PATH |
| ps2dev (EE gcc + PS2SDK) | various (ps2dev / GPL / Academic) | `<prefix>/ps2/ps2dev/` | Spawned at `ps2 build` |
| PCSX2 | GPLv3 | user `PCSX2_EXE` | Spawned by `ps2 run`; BIOS never fetched |
| libdragon mips64-elf GCC | GPLv3 (unmodified binaries) | `<prefix>/n64/inst/` | Spawned at `n64 build` |
| libdragon preview | Unlicense | `<prefix>/n64/src/libdragon/` (built into `inst/`) | Linked into the N64 guest only |
| Tiny3D | Unlicense | `<prefix>/n64/src/tiny3d/` | `n64 build --sample t3dquad` |
| Ares | ISC | `<prefix>/n64/ares/` | Spawned by `n64 run`; no PIF dump |
| Project64 | GPLv2 | user `PROJECT64_EXE` | Spawned by `n64 run` (Parallel-RDP or Angrylion) |
| FFmpeg | GPL/LGPL (pinned build) | `<prefix>/interdvd/ffmpeg/` | Spawned by `interdvd ffmpeg` / `ffprobe` |
| chdman | MAME / BSD | user PATH | Spawned by `ps2 chd` when present |
| PSL1GHT (libraries, start-up code) and newlib | MIT; BSD-style | `guest/ps3/prebuilt/gdir.elf` (compiled in); `<prefix>/ps3/inst/ps3dev/` for `ps3 setup --profile compile` | Linked into the PS3 guest only; notices written beside each `.pkg` |
| ps3dev (PPU GCC, PSL1GHT) | GPLv3 / MIT (unmodified ps3toolchain release) | `<prefix>/ps3/inst/ps3dev/` | Spawned only to compile a PS3 guest with `--overlay` (Linux) |
| make_self_npdrm, sfo, pkg (PSL1GHT tools/geohot, tools/sfo_pkg) | GPLv3 / MIT | ported into `internal/platforms/ps3` (self.go, pkg.go) | Sign EBOOT.BIN, write PARAM.SFO and the .pkg |
| RPCS3 | GPLv2 | user `RPCS3_EXE` | Spawned by `ps3 run`; PS3 system software never fetched |
| libogc (libraries, start-up code) and newlib | zlib-style (parts BSD-style); BSD-style | `guest/gamecube/prebuilt/gdir.dol` (compiled in) | Linked into the GameCube guest only; notices written beside each `.iso` |
| devkitPPC (PowerPC GCC), elf2dol | GPLv3 / GPLv2 | user `DEVKITPRO` / `DEVKITPPC` | Spawned only to compile a GameCube or Wii guest with `--overlay` |
| Dolphin | GPLv2+ | user `DOLPHIN_EXE` | Spawned by `gamecube run` and `wii run`; no IPL dump or Wii system files needed or fetched |
| libogc for the Wii (libraries, start-up code, Bluetooth stack), wiiuse, libfat and newlib | zlib-style; GPLv3 with a linking exception (wiiuse); BSD (libfat); BSD-style | `guest/wii/prebuilt/boot.dol` (compiled in) | Linked into the Wii guest only; notices written beside each `.zip` and in the app folder |

Do not copy these trees into `blazium/` or link them into the editor.

## Layout

`--prefix` / `BLAZIUM_TOOLCHAIN_PREFIX` is the root. A portable CLI may repeat the same `<plat>/` folders next to the executable.

```
<prefix>/
  ps1/gcc/bin/mipsel-none-elf-gcc[.exe]
  ps1/psn00bsdk/                 (SDK prefix / libpsn00b)
  ps1/pcsx-redux/pcsx-redux[.exe]
  ps1/openbios/openbios.bin
  ps1/guest/runtime/             (exported MIT stub)
  ps1/work/
  ps1/state.json

  ps2/ps2dev/
  ps2/guest/runtime/
  ps2/work/
  ps2/state.json

  n64/inst/                      (N64_INST)
  n64/src/libdragon/
  n64/src/tiny3d/
  n64/ares/
  n64/guest/runtime/
  n64/work/
  n64/state.json

  interdvd/ffmpeg/
  interdvd/state.json
```

Discovery: environment, then `<prefix>/<plat>/`, then `<exeDir>/<plat>/`, then PATH. Fetch URLs live in `blazium-toolchain.yml` (and SHA pins in `pins.json`).
