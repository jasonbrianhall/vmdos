#!/bin/sh
# Rebuild third_party/dosutils/bin from third_party/dosutils/src (see its
# README.vmdos). The normal build doesn't need this: the binaries are
# committed. Needs:
#   Open Watcom   tools/fetch-watcom.sh puts it in .watcom/ (WATCOM=dir for another)
#   nasm, upx     (apt install nasm upx-ucl)
#   jwasm         for NANSI (github.com/Baron-von-Riedesel/JWasm), on PATH or JWASM=
#   DJGPP         for LESS: a cross gcc i586-pc-msdosdjgpp-gcc on PATH, or DJGPP=dir
#                 with bin/ (github.com/andrewwutw/build-djgpp releases)
# ZIP and UNZIP are not built here: see README.vmdos.
#   tools/build-dosutils.sh [name ...]     (default: all)
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
src=$root/third_party/dosutils/src
bin=$root/third_party/dosutils/bin
WATCOM=${WATCOM:-$root/.watcom}
export WATCOM INCLUDE="$WATCOM/h"
export PATH="$WATCOM/binl64:$WATCOM/binl:$PATH"
[ -n "$DJGPP" ] && PATH="$DJGPP/bin:$PATH"
JWASM=${JWASM:-jwasm}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

build() {
    rm -rf "$tmp/$1"
    cp -r "$src/$1" "$tmp/$1"
    cd "$tmp/$1"
    case $1 in
    more|find|sort|move|choice|label)       # Jim Hall's tools: their own build.sh
        COMPILER=watcom sh build.sh > build.log 2>&1 || { cat build.log; exit 1; }
        out=$(echo $1 | tr a-z A-Z).EXE
        cp src/$1.exe "$bin/$out" ;;
    attrib)                                 # upper-case file names, lower-case #includes
        for f in *; do l=$(echo "$f" | tr A-Z a-z); [ -f "$f" ] && [ "$f" != "$l" ] && cp "$f" "$l"; done; true
        wcl -q -bt=dos -mt -s -os ATTRIB.C -fe=attrib.com
        cp attrib.com "$bin/ATTRIB.COM" ;;
    deltree)
        nasm -f bin deltree.asm -o "$bin/DELTREE.COM" ;;
    mem)
        cd source
        wcl -q -bt=dos -j -ms -oahls -s mem.c prf.c kitten.c -fe=mem.exe
        cp mem.exe "$bin/MEM.EXE" ;;
    xcopy)
        cd source
        wcl -oas -bt=DOS -zp1 -ms -0 -zq -k12288 -fe=xcopy.exe xcopy.c kitten.c prf.c
        cp xcopy.exe "$bin/XCOPY.EXE" ;;
    nansi)                                  # MASM sources: JWasm, case-insensitive like MASM
        cd source
        for f in nansi nansi_p nansi_f nansi_i; do "$JWASM" -q -Zm -Cu -Fo$f.obj $f.asm; done
        wlink format dos name nansi.exe file nansi.obj,nansi_p.obj,nansi_f.obj,nansi_i.obj option quiet
        exe2bin nansi.exe nansi.sys
        cp nansi.sys "$bin/NANSI.SYS" ;;
    mkeyb)
        wmake -h -f Makefile.wcc mkeyb.exe
        cp mkeyb.exe "$bin/MKEYB.EXE" ;;
    less)                                   # DJGPP (32-bit, DPMI); help.c, funcs.h, lessmsg.inc pregenerated
        cp defines.ds defines.h
        make -f Makefile.dsg CC=i586-pc-msdosdjgpp-gcc CFLAGS=-O2 LDFLAGS=-s less > build.log 2>&1 || { tail -20 build.log; exit 1; }
        upx -q --best -o "$tmp/less.exe" less.exe > /dev/null
        cp "$tmp/less.exe" "$bin/LESS.EXE" ;;
    *) echo "unknown: $1" >&2; exit 1 ;;
    esac
    echo "built $1"
}

names=${*:-more find sort move choice label attrib deltree mem xcopy nansi mkeyb less}
for n in $names; do (build $n); done
