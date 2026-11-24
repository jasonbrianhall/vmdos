#!/bin/sh
# Rebuild third_party/bash/bin/BASH.EXE: GNU bash 5.3 for DOS (DJGPP), see
# third_party/bash/README.vmdos. The normal build doesn't need this: the
# binary is committed. Needs:
#   DJGPP   a cross gcc i586-pc-msdosdjgpp-gcc on PATH, or DJGPP=dir with bin/
#           (github.com/andrewwutw/build-djgpp releases)
#   git, make, a host gcc (bash builds a few helper programs for the host);
#   upx if there
#   BASHSRC=dir   an existing checkout of bash's git (else cloned)
#   BASH_GIT=url  where to clone it from (default: GNU's savannah)
#   KEEP_TMP=1    keep the build directory
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
port=$root/third_party/bash
[ -n "$DJGPP" ] && PATH="$DJGPP/bin:$PATH"
rev=9c465866b7d849378369ef700cbe2965aa9691e3   # bash-5.3 patch 20 (5.3.20)
git_url=${BASH_GIT:-https://git.savannah.gnu.org/git/bash.git}
command -v i586-pc-msdosdjgpp-gcc > /dev/null ||
    { echo "build-bash: no i586-pc-msdosdjgpp-gcc (set DJGPP=dir)" >&2; exit 1; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/build-bash.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
[ -n "$KEEP_TMP" ] && { trap - EXIT; echo "build dir: $tmp"; }

# bash 5.3.20, the DOS changes, the DOS process layer
if [ -n "$BASHSRC" ]; then
    git -C "$BASHSRC" archive --prefix=bash/ $rev | tar -x -C "$tmp"
else
    git -c advice.detachedHead=false clone -q --depth 100 --branch master "$git_url" "$tmp/bash"
    git -c advice.detachedHead=false -C "$tmp/bash" checkout -q $rev
fi
S=$tmp/bash
patch -s -d "$S" -p1 < "$port/bash-5.3-djgpp.patch"
cp -r "$port/dos" "$S/dos"

# Configure for DOS: no job control, no coprocesses or process substitution
# (they need processes running side by side), no /dev/fd, no networking.
# Every file gets dos/dosbash.h first.
mkdir "$tmp/b"
cd "$tmp/b"
"$S/configure" --host=i586-pc-msdosdjgpp --build="$(gcc -dumpmachine)" \
    --disable-job-control --disable-coprocesses --disable-process-substitution \
    --disable-net-redirections --without-bash-malloc --without-curses \
    --disable-nls --disable-mem-scramble --enable-readline --enable-history \
    bash_cv_dev_fd=absent bash_cv_dev_stdin=absent \
    CPPFLAGS="-include $S/dos/dosbash.h" CFLAGS="-O2 -march=i586" > configure.log 2>&1 ||
    { tail -30 configure.log; exit 1; }

# The process layer, linked in front of DJGPP's C library with --wrap
i586-pc-msdosdjgpp-gcc -O2 -march=i586 -Wall -c "$S/dos/dosproc.c" -o dosproc.o
i586-pc-msdosdjgpp-gcc -c "$S/dos/dosfork.S" -o dosfork.o
wrap=
for f in fork pipe waitpid wait kill getpid getppid execve exit _exit close dup dup2 \
         fcntl signal sigaction sigprocmask; do
    wrap="$wrap,--wrap=$f"
done
make -j"$(nproc 2>/dev/null || echo 2)" LOCAL_LIBS="$tmp/b/dosproc.o $tmp/b/dosfork.o -Wl$wrap" \
    bash.exe > build.log 2>&1 || { grep -E 'error|\*\*\*' build.log | head -30; exit 1; }
i586-pc-msdosdjgpp-strip bash.exe

mkdir -p "$port/bin"
rm -f "$port/bin/BASH.EXE"
if command -v upx > /dev/null; then
    upx -q --best -o "$port/bin/BASH.EXE" bash.exe > /dev/null
else
    echo "build-bash: no upx: BASH.EXE left uncompressed" >&2
    cp bash.exe "$port/bin/BASH.EXE"
fi
ls -l "$port/bin/BASH.EXE"
