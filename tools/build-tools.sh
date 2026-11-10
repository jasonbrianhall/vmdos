#!/bin/sh
# Rebuild dos/bin/EDIT.EXE and dos/bin/TREE.EXE (vmdos's own editor and
# TREE, sources in dos/edit and dos/tree) with Open Watcom (tools/fetch-watcom.sh
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
echo "built dos/bin/EDIT.EXE and dos/bin/TREE.EXE"
