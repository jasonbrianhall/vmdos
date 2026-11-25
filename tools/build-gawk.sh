#!/bin/sh
# Build GNU awk 5.4.2 for DOS (DJGPP): GAWK.EXE and AWK.EXE (the same program)
# for C:\BIN, third_party/gawk/README.vmdos. make runs it (into deps/gawk);
# nothing prebuilt is kept in git.
#   OUT=dir        where the .EXEs go (default deps/gawk)
#   DJGPP=dir      a DJGPP cross toolchain (bin/i586-pc-msdosdjgpp-gcc); else one
#                  on PATH, else tools/get-djgpp.sh fetches one into deps/djgpp
#   GAWKSRC=dir    a checkout of gawk's git to take the source from (else cloned)
#   GAWK_GIT=url   clone from here (default: GNU's savannah, then a GitHub mirror;
#                  the tag's commit is checked, so any mirror gives the same source)
# Needs git, patch, make, a host gcc, bison; upx if there.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
port=$root/third_party/gawk
out=${OUT:-$root/deps/gawk}
tag=gawk-5.4.2
rev=fbf218a5c15705b108d38a78804f2fe52175e6c7   # "Release tarball: gawk 5.4.2."
p=i586-pc-msdosdjgpp-
if [ -n "$DJGPP" ]; then PATH="$DJGPP/bin:$PATH"
elif ! command -v ${p}gcc > /dev/null; then
    sh "$root/tools/get-djgpp.sh" "$root/deps/djgpp"
    PATH="$root/deps/djgpp/bin:$PATH"
fi
tmp=$(mktemp -d "${TMPDIR:-/tmp}/build-gawk.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

# A failure: what failed and the end of the log; the whole log stays in
# deps/gawk-build.log.
fail() {
    mkdir -p "$root/deps"
    cp "$1" "$root/deps/gawk-build.log"
    grep -E ': (fatal )?error:|\*\*\*|: command not found|: No such file|cannot execute|undefined reference' "$1" | head -20
    echo "--- last lines of $root/deps/gawk-build.log:"
    tail -15 "$1"
    exit 1
}

S=$tmp/gawk
if [ -n "$GAWKSRC" ]; then
    git -C "$GAWKSRC" archive --prefix=gawk/ $rev | tar -x -C "$tmp"
else
    for url in ${GAWK_GIT:-https://git.savannah.gnu.org/git/gawk.git https://github.com/GitMirroring/gawk}; do
        rm -rf "$S"
        git -c advice.detachedHead=false clone -q --depth 1 --branch $tag "$url" "$S" 2>/dev/null &&
            [ "$(git -C "$S" rev-parse HEAD)" = $rev ] && break
        echo "build-gawk: no $tag ($rev) from $url, trying the next" >&2
        rm -rf "$S"
    done
    [ -d "$S" ] || { echo "build-gawk: couldn't get gawk's source" >&2; exit 1; }
fi
# One time for every file: a checkout writes them in no particular order, and
# a generated file (configure, aclocal.m4, the parser) a moment older than what
# it's made from has make try to remake it (with autotools, bison, ...).
find "$S" -exec touch -h -d '2000-01-01 00:00:00' {} +
patch -s -d "$S" -p1 < "$port/gawk-dos.patch"

# Configure for DOS: no extensions (no shared libraries), no MPFR, no NLS.
# Every file gets dos/gawk-dos.h first. A 486 runs it (vmdos's CPU=486 image).
cflags="-O2 -march=i486 -mtune=pentium"
D=$port/dos
mkdir "$tmp/b"
cd "$tmp/b"
"$S/configure" --host=i586-pc-msdosdjgpp --build="$(gcc -dumpmachine)" \
    --disable-nls --disable-extensions --disable-mpfr --without-readline --disable-lint \
    CPPFLAGS="-include $D/gawk-dos.h -I$D/include" CFLAGS="$cflags" \
    CFLAGS_FOR_BUILD="-O2 -std=gnu17" > configure.log 2>&1 || fail configure.log
${p}gcc $cflags -Wall -I"$D/include" -c "$D/gawk-dos.c" -o gawk-dos.o
# just the program (gawk's git has no built manuals: make all would want makeinfo)
{ make -C support -j"$(nproc 2>/dev/null || echo 2)" &&
  make -j"$(nproc 2>/dev/null || echo 2)" gawk.exe LIBS="$tmp/b/gawk-dos.o"; } > build.log 2>&1 ||
    fail build.log
${p}strip gawk.exe

rm -rf "$out"
mkdir -p "$out"
if command -v upx > /dev/null; then
    upx -q --best -o "$out/GAWK.EXE" gawk.exe > /dev/null
else
    cp gawk.exe "$out/GAWK.EXE"
fi
cp "$out/GAWK.EXE" "$out/AWK.EXE"
echo "build-gawk: $out: GAWK.EXE, AWK.EXE ($(wc -c < "$out/GAWK.EXE") bytes each)"
