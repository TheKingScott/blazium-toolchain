#!/usr/bin/env bash
# Rebuild guest/wiiu/prebuilt/gdir.rpx (the Wii U guest the CLI ships
# compiled) after changing guest/wiiu/gdir/runtime, and record which sources
# it is (guest/wiiu/prebuilt/SOURCES; go test ./guest/wiiu checks it). Needs
# devkitPro's devkitPPC, wut and wut-tools (elf2rpl):
#   DEVKITPRO=/opt/devkitpro scripts/wiiu-guest-prebuilt.sh
#
# The shaders are compiled already (gx2_shaders.h). After editing
# gx2_shaders.vsh or .psh, rebuild that header with latte-assembler
# (github.com/decaf-emu/decaf-emu, tools/latte-assembler), as its comment
# says. Its "assemble" mode reads the shader headers (.NAME, .TYPE, ...) from
# comment lines; in decaf-emu's assembler_cf.cpp the comment node must be
# kept as "; " + its text, or the headers are dropped without a word.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
: "${DEVKITPRO:=/opt/devkitpro}"
: "${DEVKITPPC:=$DEVKITPRO/devkitPPC}"
export DEVKITPRO DEVKITPPC
export PATH="$DEVKITPRO/tools/bin:$PATH"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
out="$root/guest/wiiu/prebuilt"
mkdir -p "$out"
cp -r "$root/guest/wiiu/gdir/runtime/." "$work/"
make -C "$work" -j"$(nproc)"
cp "$work/gdir.rpx" "$out/gdir.rpx"
(cd "$root" && go run ./guest/wiiu/cmd/sources > guest/wiiu/prebuilt/SOURCES)
echo "wrote guest/wiiu/prebuilt: gdir.rpx ($(wc -c < "$out/gdir.rpx") bytes), SOURCES $(cat "$out/SOURCES")"
