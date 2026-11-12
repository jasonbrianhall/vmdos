#!/bin/sh
# Rebuild dos/bin/EDIT.EXE, TREE.EXE and ZCOPY.EXE (vmdos's own editor, TREE
# and ZCOPY, sources in dos/edit, dos/tree and dos/zcopy) with Open Watcom (tools/fetch-watcom.sh
# puts it in .watcom/; WATCOM=dir for another). The normal build doesn't need
# this: the programs are committed.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
WATCOM=${WATCOM:-$root/.watcom}
export WATCOM INCLUDE="$WATCOM/h"
export PATH="$WATCOM/binl64:$WATCOM/binl:$PATH"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cd "$tmp"
wcl -q -bt=dos -ml -0 -os -s -za99 -wx "$root/dos/edit/edit.c" -fe="$root/dos/bin/EDIT.EXE"
wcl -q -bt=dos -ms -0 -os -s -za99 -wx -k16384 "$root/dos/tree/tree.c" -fe="$root/dos/bin/TREE.EXE"
wcl -q -bt=dos -ml -0 -os -s -za99 -wx -k16384 "$root/dos/zcopy/zcopy.c" -fe="$root/dos/bin/ZCOPY.EXE"
echo "built dos/bin/EDIT.EXE, dos/bin/TREE.EXE and dos/bin/ZCOPY.EXE"
