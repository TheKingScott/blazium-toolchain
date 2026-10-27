#!/usr/bin/env bash
# Rebuild guest/wii/prebuilt/boot.dol (the Wii guest the CLI ships
# compiled) after changing guest/wii/gdir/runtime, and record which sources
# it is (guest/wii/prebuilt/SOURCES; go test ./guest/wii checks it). Needs
# devkitPPC, libogc (wii), libfat (portlibs/wii) and elf2dol:
#   DEVKITPRO=/opt/devkitpro scripts/wii-guest-prebuilt.sh
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
: "${DEVKITPRO:=/opt/devkitpro}"
: "${DEVKITPPC:=$DEVKITPRO/devkitPPC}"
: "${ELF2DOL:=$(command -v elf2dol || echo "$DEVKITPRO/tools/bin/elf2dol")}"
export DEVKITPRO DEVKITPPC
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
out="$root/guest/wii/prebuilt"
mkdir -p "$out"
cp -r "$root/guest/wii/gdir/runtime/." "$work/"
make -C "$work" -j"$(nproc)" ELF2DOL="$ELF2DOL"
cp "$work/boot.dol" "$out/boot.dol"
(cd "$root" && go run ./guest/wii/cmd/sources > guest/wii/prebuilt/SOURCES)
echo "wrote guest/wii/prebuilt: boot.dol ($(wc -c < "$out/boot.dol") bytes), SOURCES $(cat "$out/SOURCES")"
