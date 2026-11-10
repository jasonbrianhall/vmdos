#!/bin/sh
# Rebuild third_party/fsck/bin/FSCK.EXE from third_party/fsck/src (see its
# README.vmdos). The normal build doesn't need this: the binary is committed.
# Needs DJGPP: a cross gcc i586-pc-msdosdjgpp-gcc on PATH, or DJGPP=dir with
# bin/ (github.com/andrewwutw/build-djgpp releases), or DJGPP_CC=any command
# that runs it (a Docker image, say).
#   tools/build-fsck.sh          FSCK.EXE (DOS)
#   tools/build-fsck.sh host     fsck for Linux, to check disk images (esp.img)
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
src=$root/third_party/fsck/src
[ -n "$DJGPP" ] && PATH="$DJGPP/bin:$PATH"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cp -r "$src/." "$tmp/"
cd "$tmp"
if [ "$1" = host ]; then
    make -s fsck
    cp fsck "$root/fsck"
    echo "fsck (Linux): $root/fsck"
else
    make -s dos ${DJGPP_CC:+DJGPP_CC="$DJGPP_CC"}
    cp FSCK.EXE "$root/third_party/fsck/bin/FSCK.EXE"
    echo "third_party/fsck/bin/FSCK.EXE"
fi
