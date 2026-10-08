#!/bin/sh
# Rebuild third_party/ctmouse/CTMOUSE.COM from its source.
# Needs: jwasm (github.com/Baron-von-Riedesel/JWasm), wlink + exe2bin (Open Watcom).
set -e
src=$(cd "$(dirname "$0")/../third_party/ctmouse/src" && pwd)
out=$(cd "$src/.." && pwd)/CTMOUSE.COM
tmp=$(mktemp -d)
cp -r "$src"/. "$tmp"
cd "$tmp"
cp ctm-en.msg ctmouse.msg
jwasm -mt -Fo=ctmouse.obj ctmouse.asm
wlink format dos name ctmouse.exe file ctmouse.obj option quiet
exe2bin ctmouse.exe ctmouse.bin
cp ctmouse.bin "$out"
rm -rf "$tmp"
echo "built $out"
