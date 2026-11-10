#!/bin/sh
# Rebuild third_party/doslfn/DOSLFN.COM from its source.
# DOSLFN is written for TASM's IDEAL mode, which JWasm and NASM don't take,
# so this runs Borland's TASM 4.1 and TLINK 7.1 in DOSBox:
#   tools/build-doslfn.sh /path/to/TASM/BIN
# The folder needs TASM.EXE, TLINK.EXE, RTM.EXE and DPMI16BI.OVL
# (github.com/zajo/TASM has them in BIN). Needs: dosbox.
set -e
tasm=${1:?usage: $0 /path/to/TASM/BIN}
src=$(cd "$(dirname "$0")/../third_party/doslfn/src" && pwd)
out=$(cd "$src/.." && pwd)/DOSLFN.COM
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/c"
cp "$src"/doslfn.asm "$src"/prolog.asm "$src"/text.inc "$tmp/c/"
for f in TASM.EXE TLINK.EXE RTM.EXE DPMI16BI.OVL; do cp "$tasm/$f" "$tmp/c/"; done
# Two DOSBox runs: in one, TLINK didn't see the object TASM had just written.
for step in "TASM -m5 -t doslfn.asm,doslfn.obj > tasm.out" "TLINK /3 /t doslfn.obj > tlink.out"; do
    printf '[cpu]\ncycles=max\n[autoexec]\nmount c "%s"\nc:\n%s\nexit\n' "$tmp/c" "$step" > "$tmp/dosbox.conf"
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy dosbox -conf "$tmp/dosbox.conf" -noconsole > /dev/null 2>&1
done
if [ ! -f "$tmp/c/DOSLFN.COM" ]; then cat "$tmp/c"/*.OUT 2>/dev/null; echo "build failed" >&2; exit 1; fi
cp "$tmp/c/DOSLFN.COM" "$out"
echo "built $out"
