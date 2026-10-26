#!/usr/bin/env bash
# Rebuild guest/gamecube/prebuilt (the GameCube guest the CLI ships
# compiled: gdir.dol and the disc's apploader, apploader.img) after
# changing guest/gamecube, and record which sources they are
# (guest/gamecube/prebuilt/SOURCES; go test ./guest/gamecube checks it).
# Needs devkitPPC, libogc and elf2dol:
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

# the guest
cp -r "$root/guest/gamecube/gdir/runtime/." "$work/"
make -C "$work" -j"$(nproc)" ELF2DOL="$ELF2DOL"
cp "$work/gdir.dol" "$out/gdir.dol"

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
