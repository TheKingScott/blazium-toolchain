#!/usr/bin/env bash
# Rebuild guest/psp/prebuilt/gdir.prx (the PSP guest the CLI ships
# compiled) after changing guest/psp/gdir/runtime, and record which sources
# it is (guest/psp/prebuilt/SOURCES; go test ./guest/psp checks it). Needs
# pspdev (its toolchain and PSPSDK):
#   PSPDEV=/usr/local/pspdev scripts/psp-guest-prebuilt.sh
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
: "${PSPDEV:=/usr/local/pspdev}"
export PSPDEV
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
out="$root/guest/psp/prebuilt"
mkdir -p "$out"
cp -r "$root/guest/psp/gdir/runtime/." "$work/"
make -C "$work" -j"$(nproc)"
cp "$work/gdir.prx" "$out/gdir.prx"
(cd "$root" && go run ./guest/psp/cmd/sources > guest/psp/prebuilt/SOURCES)
echo "wrote guest/psp/prebuilt: gdir.prx ($(wc -c < "$out/gdir.prx") bytes), SOURCES $(cat "$out/SOURCES")"
