// blazium-toolchain fetches console compilers and builds PS1, PS2, N64, and
// Interactive DVD products. GPL-3.0-or-later. The MIT editor only spawns it.
//
//	blazium-toolchain [--json] [--prefix DIR] [--settings FILE] version
//	blazium-toolchain [--json] [--settings FILE] list
//	blazium-toolchain [--json] [--settings FILE] settings
//	blazium-toolchain [--json] [--prefix DIR] [--settings FILE] ps1 setup [--profile compile|dev|iso] [--offline]
//	blazium-toolchain [--json] [--prefix DIR] ps1 env
//	blazium-toolchain [--json] [--prefix DIR] ps1 status
//	blazium-toolchain [--prefix DIR] ps1 build --out FILE [--src DIR | --sample template|gte]
//	  [--overlay DIR] [--export-src DIR]
//	  [--tim|--mesh|--vag|--sprite|--script|--gdbc|--luau|--str|--xa|--node|--hud|--tile|--scene|--anim|--cam|--hit|--nav|--path|--way]
//	  --src is optional; the bundled MIT guest stub is the default.
//	  --overlay copies extra or replacement *.cpp on top of the stub (or --src).
//	  --export-src writes the resolved guest C++ tree (same files as export-guest plus overlay).
//	blazium-toolchain [--prefix DIR] ps1 export-guest [--out DIR]
//	  writes bundled guest *.cpp / *.h / CMakeLists.txt (default: prefix/ps1/guest/runtime).
//	blazium-toolchain [--prefix DIR] ps1 run [--iso CUE] [--timeout 120s] [--ui] GAME.EXE
//	blazium-toolchain [--prefix DIR] ps1 iso --xml FILE [--out PATH]
//	blazium-toolchain [--json] [--prefix DIR] ps1 fmv
//	blazium-toolchain [--json] [--prefix DIR] interdvd setup [--offline]
//	blazium-toolchain [--json] [--prefix DIR] interdvd env
//	blazium-toolchain [--json] [--prefix DIR] interdvd status
//	blazium-toolchain [--prefix DIR] interdvd ffmpeg -- <args>
//	blazium-toolchain [--prefix DIR] interdvd ffprobe -- <args>
//	blazium-toolchain interdvd iso --dir DIR --out FILE [--meta FILE] [--write-meta FILE]
//	blazium-toolchain interdvd meta init --out FILE
//	blazium-toolchain interdvd meta validate --meta FILE
//	blazium-toolchain [--prefix DIR] ps2 setup [--profile compile|dev|iso] [--offline]
//	blazium-toolchain [--prefix DIR] ps2 env
//	blazium-toolchain [--prefix DIR] ps2 status
//	blazium-toolchain [--prefix DIR] ps2 build --out FILE.elf [--src DIR | --sample cube]
//	  [--overlay DIR] [--export-src DIR] [--node|--mesh|--gtex|--script]
//	blazium-toolchain [--prefix DIR] ps2 export-guest [--out DIR]
//	blazium-toolchain [--prefix DIR] ps2 run [--iso FILE.iso] [--timeout 120s] [--ui] GAME.elf
//	blazium-toolchain [--prefix DIR] ps2 iso --dir TREE --out FILE.iso
//	blazium-toolchain [--prefix DIR] ps2 elf-info FILE.elf
//	blazium-toolchain [--prefix DIR] ps2 chd --iso FILE.iso --out FILE.chd
//	blazium-toolchain [--prefix DIR] n64 setup [--profile compile|dev|rom] [--offline]
//	blazium-toolchain [--prefix DIR] n64 env
//	blazium-toolchain [--prefix DIR] n64 status
//	blazium-toolchain [--prefix DIR] n64 build --out FILE.z64 [--src DIR | --sample helloworld|rdpqdemo|t3dquad|ovldemo]
//	  [--overlay DIR] [--export-src DIR] [--display 320|640] [--rumble] [--rdram 8|4]
//	  [--ntex|--mesh|--node|--inp|--sfx|--music|--pack|--pack-dir|--script|--gdbc|--luau]
//	blazium-toolchain [--prefix DIR] n64 export-guest [--out DIR]
//	blazium-toolchain [--prefix DIR] n64 run [--emu ares|project64|both] [--timeout 120s] [--rdram 8|4] GAME.z64
//	blazium-toolchain [--prefix DIR] n64 rom --dir TREE --out FILE.z64 [--elf FILE.elf]
//	blazium-toolchain [--prefix DIR] ps3 setup [--profile prebuilt|compile] [--offline]
//	blazium-toolchain [--prefix DIR] ps3 build --sample gdir --out GAME.pkg|DIR [--pack-dir EXPORT]
//	  [--title NAME] [--title-id BLAZ00001] [--icon ICON0.PNG] [--overlay DIR]
//	blazium-toolchain [--prefix DIR] ps3 run [--timeout 120s] GAME_FOLDER|EBOOT.BIN|GAME.pkg
//	blazium-toolchain [--prefix DIR] gamecube setup [--profile prebuilt|compile]
//	blazium-toolchain [--prefix DIR] gamecube build --sample gdir --out GAME.iso|DIR [--pack-dir EXPORT]
//	  [--title NAME] [--title-id GBZEBZ] [--icon BANNER.PNG] [--overlay DIR]
//	blazium-toolchain [--prefix DIR] gamecube run [--timeout 120s] GAME.iso
//	blazium-toolchain [--prefix DIR] wii setup [--profile prebuilt|compile]
//	blazium-toolchain [--prefix DIR] wii build --sample gdir --out GAME.zip|DIR [--pack-dir EXPORT]
//	  [--title NAME] [--title-id APPFOLDER] [--icon ICON.PNG] [--overlay DIR]
//	blazium-toolchain [--prefix DIR] wii run [--timeout 120s] apps/NAME|boot.dol|SD_ROOT
//
// ps1, ps2, ps3, gamecube, wii, n64, and interdvd are implemented. Guests are bundled. N64 writes
// a big-endian .z64. Cache is <prefix>/<plat>/{inst,src,guest,work}. Settings
// come from blazium-toolchain.yml. Setup/build/run/iso/rom need Windows or
// Linux (exit 2 elsewhere); ps3, gamecube and wii build on any host with their bundled guests. ps4
// is reserved (exit 2).
package main
