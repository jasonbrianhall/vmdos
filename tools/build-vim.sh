#!/bin/sh
# Rebuild third_party/vim/bin/VIM.EXE (see third_party/vim/README.vmdos). The
# normal build doesn't need this: the binary is committed. Needs:
#   DJGPP   a cross gcc i586-pc-msdosdjgpp-gcc on PATH, or DJGPP=dir with bin/
#           (github.com/andrewwutw/build-djgpp releases)
#   git, make, upx (apt install upx-ucl)
#   VIMSRC=dir  an existing checkout of github.com/vim/vim to use (else cloned)
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
[ -n "$DJGPP" ] && PATH="$DJGPP/bin:$PATH"
tag=v7.4.1398                   # the last Vim with the DJGPP makefile (Make_djg.mak)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
if [ -n "$VIMSRC" ]; then
    git -C "$VIMSRC" archive --prefix=vim/ $tag | tar -x -C "$tmp"
else
    git clone -q --depth 1 --branch $tag https://github.com/vim/vim "$tmp/vim"
fi
cd "$tmp/vim"
patch -s -p1 < "$root/third_party/vim/vim-7.4.1398-djgpp.patch"
cd src
make -f Make_djg.mak CC="i586-pc-msdosdjgpp-gcc -fcommon -std=gnu89 -w" vim.exe > build.log 2>&1 ||
    { tail -30 build.log; exit 1; }
rm -f "$root/third_party/vim/bin/VIM.EXE"
upx -q --best -o "$root/third_party/vim/bin/VIM.EXE" vim.exe > /dev/null
ls -l "$root/third_party/vim/bin/VIM.EXE"
