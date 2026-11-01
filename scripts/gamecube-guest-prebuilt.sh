#!/usr/bin/env bash
# Rebuild guest/gamecube/prebuilt (the GameCube guest the CLI ships
# compiled: gdir.dol and the disc's apploader, apploader.img) after
# changing guest/gamecube, and record which sources they are
# (guest/gamecube/prebuilt/SOURCES; go test ./guest/gamecube checks it).
# Needs devkitPPC, catnip, elf2dol and git. The guest links libogc v3.2.1
# with guest/gamecube/libogc/wii-mode.patch (so the disc also runs on a
# Wii under Nintendont), built here from LIBOGC_SRC, a libogc checkout,
# or else a clone of devkitPro's tag:
#   DEVKITPRO=/opt/devkitpro scripts/gamecube-guest-prebuilt.sh
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
: "${DEVKITPRO:=/opt/devkitpro}"
: "${DEVKITPPC:=$DEVKITPRO/devkitPPC}"
: "${ELF2DOL:=$(command -v elf2dol || echo "$DEVKITPRO/tools/bin/elf2dol")}"
export DEVKITPRO DEVKITPPC
bin="$DEVKITPPC/bin"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
out="$root/guest/gamecube/prebuilt"
mkdir -p "$out"

# libogc v3.2.1 with the Wii-mode patch, the GameCube libraries only
: "${CATNIP:=$(command -v catnip || echo "$DEVKITPRO/tools/bin/catnip")}"
src="$work/libogc-src"
if [ -n "${LIBOGC_SRC:-}" ]; then
	git -C "$LIBOGC_SRC" archive --format=tar v3.2.1 | (mkdir -p "$src" && tar -x -C "$src")
else
	git clone -q --depth 1 --branch v3.2.1 https://github.com/devkitPro/libogc "$src"
fi
git -C "$src" apply "$root/guest/gamecube/libogc/wii-mode.patch" 2>/dev/null ||
	(cd "$src" && patch -p1 < "$root/guest/gamecube/libogc/wii-mode.patch")
sed -i -e 's/DEFAULT cube wii/DEFAULT cube/' -e '/catnip_add_preset(wii/d' "$src/Catnip.cmake"
libogc="$work/libogc"
(cd "$src" && LIBOGC_VER=v3.2.1 "$CATNIP" -S "$src" -B "$work/libogc-build" -D CMAKE_INSTALL_PREFIX="$libogc" -j"$(nproc)" install >/dev/null)

# the guest
mkdir -p "$work/gdir"
cp -r "$root/guest/gamecube/gdir/runtime/." "$work/gdir/"
make -C "$work/gdir" -j"$(nproc)" ELF2DOL="$ELF2DOL" LIBOGC="$libogc"
cp "$work/gdir/gdir.dol" "$out/gdir.dol"

# the apploader: its code at 0x81200000, after a 0x20-byte header (the
# build's date, the entry point, the code's size, no trailer)
app="$work/apploader"
mkdir -p "$app"
"$bin/powerpc-eabi-gcc" -O2 -mcpu=750 -meabi -mhard-float -ffreestanding -nostdlib -fno-builtin \
	-fno-zero-initialized-in-bss -msdata=none -G0 \
	-fno-asynchronous-unwind-tables -fno-unwind-tables -Wl,-N -Wl,--build-id=none -Wl,-Ttext=0x81200000 -Wl,-e,entry \
	-o "$app/apploader.elf" "$root/guest/gamecube/apploader/apploader.c"
"$bin/powerpc-eabi-objcopy" -O binary -R .comment "$app/apploader.elf" "$app/apploader.bin"
entry=$("$bin/powerpc-eabi-nm" "$app/apploader.elf" | awk '$3 == "entry" { print $1 }')
python3 - "$app/apploader.bin" "$entry" "$out/apploader.img" <<'EOF'
import struct, sys
code = open(sys.argv[1], "rb").read()
code += b"\0" * (-len(code) % 32)
hdr = b"2026/10/26".ljust(16, b"\0") + struct.pack(">III", int(sys.argv[2], 16), len(code), 0) + b"\0" * 4
open(sys.argv[3], "wb").write(hdr + code)
EOF

(cd "$root" && go run ./guest/gamecube/cmd/sources > guest/gamecube/prebuilt/SOURCES)
echo "wrote guest/gamecube/prebuilt: gdir.dol ($(wc -c < "$out/gdir.dol") bytes), apploader.img ($(wc -c < "$out/apploader.img") bytes), SOURCES $(cat "$out/SOURCES")"
