#!/bin/sh
# Build BASH.EXE, GNU bash 5.3 for DOS (DJGPP): third_party/bash/README.vmdos.
# make runs it (deps/bash/BASH.EXE); nothing prebuilt is kept in git.
#   OUT=dir       where BASH.EXE goes (default deps/bash)
#   DJGPP=dir     a DJGPP cross toolchain (bin/i586-pc-msdosdjgpp-gcc); else one
#                 on PATH, else tools/get-djgpp.sh fetches one into deps/djgpp
#   BASHSRC=dir   a checkout of bash's git to take the source from (else cloned)
#   BASH_GIT=url  clone from here (default: GNU's savannah, then a GitHub mirror;
#                 the commit is pinned, so any mirror gives the same source)
# Needs git, patch, make and a host gcc (bash builds a few host programs); upx
# if there.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
port=$root/third_party/bash
out=${OUT:-$root/deps/bash}
rev=9c465866b7d849378369ef700cbe2965aa9691e3   # "Bash-5.3 patch 20" (5.3.20)
p=i586-pc-msdosdjgpp-
if [ -n "$DJGPP" ]; then PATH="$DJGPP/bin:$PATH"
elif ! command -v ${p}gcc > /dev/null; then
    sh "$root/tools/get-djgpp.sh" "$root/deps/djgpp"
    PATH="$root/deps/djgpp/bin:$PATH"
fi
tmp=$(mktemp -d "${TMPDIR:-/tmp}/build-bash.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

# bash 5.3.20, the DOS changes, the DOS process layer
S=$tmp/bash
if [ -n "$BASHSRC" ]; then
    git -C "$BASHSRC" archive --prefix=bash/ $rev | tar -x -C "$tmp"
else
    for url in ${BASH_GIT:-https://git.savannah.gnu.org/git/bash.git https://github.com/tianon/mirror-bash}; do
        rm -rf "$S"
        git clone -q --depth 100 --branch master "$url" "$S" 2>/dev/null &&
            git -c advice.detachedHead=false -C "$S" checkout -q $rev 2>/dev/null && break
        echo "build-bash: no bash $rev from $url, trying the next" >&2
        rm -rf "$S"
    done
    [ -d "$S" ] || { echo "build-bash: couldn't get bash's source" >&2; exit 1; }
fi
patch -s -d "$S" -p1 < "$port/bash-5.3-djgpp.patch"
cp -r "$port/dos" "$S/dos"

# Configure for DOS: no job control, no coprocesses or process substitution
# (they need processes running side by side), no /dev/fd, no networking.
# Every file gets dos/dosbash.h first. A 486 runs it (vmdos's CPU=486 image).
cflags="-O2 -march=i486 -mtune=pentium"
mkdir "$tmp/b"
cd "$tmp/b"
"$S/configure" --host=i586-pc-msdosdjgpp --build="$(gcc -dumpmachine)" \
    --disable-job-control --disable-coprocesses --disable-process-substitution \
    --disable-net-redirections --without-bash-malloc --without-curses \
    --disable-nls --disable-mem-scramble --enable-readline --enable-history \
    bash_cv_dev_fd=absent bash_cv_dev_stdin=absent \
    CPPFLAGS="-include $S/dos/dosbash.h" CFLAGS="$cflags" > configure.log 2>&1 ||
    { tail -30 configure.log; exit 1; }

# The process layer, linked in front of DJGPP's C library with --wrap
${p}gcc $cflags -Wall -c "$S/dos/dosproc.c" -o dosproc.o
${p}gcc -c "$S/dos/dosfork.S" -o dosfork.o
wrap=
for f in fork pipe waitpid wait kill getpid getppid execve exit _exit close dup dup2 \
         fcntl signal sigaction sigprocmask; do
    wrap="$wrap,--wrap=$f"
done
make -j"$(nproc 2>/dev/null || echo 2)" LOCAL_LIBS="$tmp/b/dosproc.o $tmp/b/dosfork.o -Wl$wrap" \
    bash.exe > build.log 2>&1 || { grep -E 'error|\*\*\*' build.log | head -30; exit 1; }
${p}strip bash.exe

mkdir -p "$out"
rm -f "$out/BASH.EXE"
if command -v upx > /dev/null; then
    upx -q --best -o "$out/BASH.EXE" bash.exe > /dev/null
else
    cp bash.exe "$out/BASH.EXE"
fi
echo "build-bash: $out/BASH.EXE ($(wc -c < "$out/BASH.EXE") bytes)"
