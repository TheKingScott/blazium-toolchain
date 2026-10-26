#!/usr/bin/env bash
# Rebuild guest/ps3/prebuilt/gdir.elf (the PS3 guest the CLI ships compiled)
# after changing guest/ps3/gdir/runtime, and record which sources it is
# (guest/ps3/prebuilt/SOURCES; go test ./guest/ps3 checks it). Needs ps3dev
# (Linux): PS3DEV=/path/to/ps3dev scripts/ps3-guest-prebuilt.sh
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
: "${PS3DEV:=/usr/local/ps3dev}"
export PS3DEV PSL1GHT="${PSL1GHT:-$PS3DEV}"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp -r "$root/guest/ps3/gdir/runtime/." "$work/"
make -C "$work" -j"$(nproc)"
cp "$work/gdir.prx.elf" "$root/guest/ps3/prebuilt/gdir.elf"
(cd "$root" && go run ./guest/ps3/cmd/sources > guest/ps3/prebuilt/SOURCES)
echo "wrote guest/ps3/prebuilt/gdir.elf ($(wc -c < "$root/guest/ps3/prebuilt/gdir.elf") bytes), SOURCES $(cat "$root/guest/ps3/prebuilt/SOURCES")"
