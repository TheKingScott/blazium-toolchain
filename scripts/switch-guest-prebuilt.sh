#!/usr/bin/env bash
# Rebuild guest/switch/prebuilt/gdir.nro (the Switch guest the CLI ships
# compiled) after changing guest/switch/gdir/runtime, and record which
# sources it is (guest/switch/prebuilt/SOURCES; go test ./guest/switch checks
# it). Needs devkitPro's devkitA64, libnx, deko3d and switch-tools (elf2nro):
#   DEVKITPRO=/opt/devkitpro scripts/switch-guest-prebuilt.sh
#
# The shaders are compiled already (dk_shaders.h). After editing dk_vsh.glsl
# or dk_fsh.glsl, run it with --shaders first: that needs devkitPro's uam
# (deko3d's shader compiler) in $DEVKITPRO/tools/bin.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
: "${DEVKITPRO:=/opt/devkitpro}"
export DEVKITPRO
export PATH="$DEVKITPRO/tools/bin:$PATH"
rt="$root/guest/switch/gdir/runtime"
if [ "${1:-}" = "--shaders" ]; then
	sh=$(mktemp -d)
	uam -s vert -o "$sh/vsh.dksh" "$rt/dk_vsh.glsl"
	uam -s frag -o "$sh/fsh.dksh" "$rt/dk_fsh.glsl"
	python3 - "$sh" "$rt/dk_shaders.h" <<'PY'
import sys
d, out = sys.argv[1], sys.argv[2]
def arr(name, path):
    b = open(path, 'rb').read()
    s = 'static const unsigned char %s[%d] __attribute__((aligned(256))) = {\n' % (name, len(b))
    for i in range(0, len(b), 16):
        s += '\t' + ' '.join('0x%02x,' % x for x in b[i:i + 16]) + '\n'
    return s + '};\n'
head = open(out).read().split('#pragma once')[0]
open(out, 'w').write(head + '#pragma once\n\n' + arr('dk_vsh', d + '/vsh.dksh') + '\n' + arr('dk_fsh', d + '/fsh.dksh'))
PY
	rm -rf "$sh"
	echo "wrote guest/switch/gdir/runtime/dk_shaders.h"
fi
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
out="$root/guest/switch/prebuilt"
mkdir -p "$out"
cp -r "$rt/." "$work/"
make -C "$work" -j"$(nproc)"
cp "$work/gdir.nro" "$out/gdir.nro"
(cd "$root" && go run ./guest/switch/cmd/sources > guest/switch/prebuilt/SOURCES)
echo "wrote guest/switch/prebuilt: gdir.nro ($(wc -c < "$out/gdir.nro") bytes), SOURCES $(cat "$out/SOURCES")"
