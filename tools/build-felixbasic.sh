#!/bin/sh
# Rebuild third_party/felixbasic/bin/FELIXBAS.EXE (see
# third_party/felixbasic/README.vmdos). The normal build doesn't need this:
# the binary is committed. Needs:
#   DJGPP   a cross g++ i586-pc-msdosdjgpp-g++ on PATH, or DJGPP=dir with bin/
#           (github.com/andrewwutw/build-djgpp releases)
#   git, make, upx (apt install upx-ucl)
#   FELIXSRC=dir  an existing checkout of github.com/jasonbrianhall/felixterminal
#                 to use (else cloned, REF=branch/tag, default main)
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
[ -n "$DJGPP" ] && PATH="$DJGPP/bin:$PATH"
ref=${REF:-main}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
if [ -n "$FELIXSRC" ]; then
    git -C "$FELIXSRC" archive --prefix=felix/ HEAD wopr | tar -x -C "$tmp"
    rev=$(git -C "$FELIXSRC" rev-parse --short HEAD)
else
    git clone -q --depth 1 --branch "$ref" https://github.com/jasonbrianhall/felixterminal "$tmp/felix"
    rev=$(git -C "$tmp/felix" rev-parse --short HEAD)
fi
cd "$tmp/felix/wopr/wopr_basic/msdos"
# The plain DJGPP stub (no CWSDPMI): vmdos is the DPMI host, as for LESS and VIM.
make DOCKER=0 felixbas.coff > build.log 2>&1 || { tail -30 build.log; exit 1; }
rm -f "$root/third_party/felixbasic/bin/FELIXBAS.EXE"
upx -q --best -o "$root/third_party/felixbasic/bin/FELIXBAS.EXE" felixbas.exe > /dev/null
ls -l "$root/third_party/felixbasic/bin/FELIXBAS.EXE"
echo "felixterminal $rev"
