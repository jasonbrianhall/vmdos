#!/bin/sh
# Rebuild third_party/felixbasic/bin/FELIXBAS.EXE (see
# third_party/felixbasic/README.vmdos). The normal build doesn't need this:
# the binary is committed. .github/workflows/felixbasic.yml runs it nightly
# and opens a PR when felixterminal's BASIC has changed.
#   tools/build-felixbasic.sh         rebuild, and record the source in README.vmdos
#   tools/build-felixbasic.sh check   only say whether felixterminal has a newer
#                                     BASIC: prints "current" or "new REV"
# Needs:
#   Docker  felixterminal's msdos Makefile builds in the djfdyuruiry/djgpp image
#           when there is no local DJGPP: nothing else to install. Or a local
#           cross g++ i586-pc-msdosdjgpp-g++ on PATH, or DJGPP=dir with bin/
#           (github.com/andrewwutw/build-djgpp releases); DOCKER=1 / DOCKER=0
#           forces one or the other.
#   git, make, wget, unzip (CWSDPMI); upx (apt install upx-ucl) to pack it, else it goes in unpacked
#   CSDPMI=dir    csdpmi7b.zip already unpacked there (bin/CWSDSTUB.EXE)
#   FELIXSRC=dir  an existing checkout of github.com/jasonbrianhall/felixterminal
#                 to use (else cloned, REF=branch/tag, default main)
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
readme=$root/third_party/felixbasic/README.vmdos
[ -n "$DJGPP" ] && PATH="$DJGPP/bin:$PATH"
ref=${REF:-main}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
if [ -n "$FELIXSRC" ]; then
    src=$FELIXSRC
else
    git clone -q --depth 1 --branch "$ref" https://github.com/jasonbrianhall/felixterminal "$tmp/src"
    src=$tmp/src
fi
rev=$(git -C "$src" rev-parse --short HEAD)
date=$(git -C "$src" log -1 --format=%cs HEAD)
# What the build reads: wopr_basic and the two files it links to in ../. Their
# git object ids give a source id that changes only when the BASIC does.
# "cwsdstub": how it's built (bump it when that changes, to rebuild).
id=$({ echo cwsdstub; git -C "$src" rev-parse HEAD:wopr/wopr_basic HEAD:wopr/wopr.h HEAD:wopr/msvc_posix_compat.h; } |
     sha1sum | cut -c1-12)
have=$(sed -n 's/^felixterminal [0-9a-f]* [0-9-]* source \([0-9a-f]*\)$/\1/p' "$readme")
if [ "$1" = check ]; then
    [ "$id" = "$have" ] && echo current || echo "new $rev"
    exit 0
fi

git -C "$src" archive --prefix=felix/ HEAD wopr | tar -x -C "$tmp"
cd "$tmp/felix/wopr/wopr_basic"
# wopr.h and msvc_posix_compat.h are links into ../, which the Docker build
# (it mounts only wopr_basic) can't follow: make them copies.
for f in $(find . -maxdepth 1 -type l); do cp --remove-destination "$(readlink -f "$f")" "$f"; done
cd msdos
# CWSDPMI's stub in front, as felixterminal's own build (make msdos_embedded)
# does: it uses the DPMI host that is there (vmdos's), and where there is
# none (VMHV, plain DOS) starts the CWSDPMI inside it. get-csdpmi downloads
# csdpmi7b.zip from the DJGPP mirror.
[ -n "$CSDPMI" ] && cp -r "$CSDPMI" csdpmi     # skips the download
make ${DOCKER:+DOCKER="$DOCKER"} felixbas.coff get-csdpmi > build.log 2>&1 || { tail -30 build.log; exit 1; }
stub=csdpmi/bin/CWSDSTUB.EXE
[ -f "$stub" ] || { echo "no $stub (csdpmi7b.zip)"; exit 1; }
out=$root/third_party/felixbasic/bin/FELIXBAS.EXE
if command -v upx > /dev/null; then
    # UPX packs the program behind the plain DJGPP stub (felixbas.exe, left
    # by the felixbas.coff step); that stub is then swapped for CWSDSTUB.
    upx -q --best -o packed.exe felixbas.exe > /dev/null
    # The COFF image starts where the stub's MZ header says the stub ends.
    set -- $(od -An -tu2 -j2 -N4 packed.exe)
    skip=$(( $2 * 512 - ($1 ? 512 - $1 : 0) ))
    { cat "$stub"; tail -c +$((skip + 1)) packed.exe; } > "$out"
else
    echo "upx not found: FELIXBAS.EXE not packed"
    cat "$stub" felixbas.coff > "$out"
fi
sed -i "s/^felixterminal [0-9a-f]* [0-9-]* source [0-9a-f]*\$/felixterminal $rev $date source $id/" "$readme"
ls -l "$out"
echo "felixterminal $rev ($date), source $id"
