#!/bin/sh
# Build the Unix tools for C:\BIN (ls, cp, grep, sed, find, ...) from suckless
# sbase for DOS (DJGPP): third_party/sbase/README.vmdos. make runs it (into
# deps/sbase); nothing prebuilt is kept in git. Which tools: third_party/sbase/TOOLS.
#   OUT=dir         where the .EXEs go (default deps/sbase)
#   DJGPP=dir       a DJGPP cross toolchain (bin/i586-pc-msdosdjgpp-gcc); else one
#                   on PATH, else tools/get-djgpp.sh fetches one into deps/djgpp
#   SBASESRC=dir    a checkout of sbase's git to take the source from (else cloned)
#   SBASE_GIT=url   clone from here (default: git.suckless.org, then a GitHub
#                   mirror; the commit is pinned, so any mirror gives the same source)
# Needs git, patch, make, yacc (bison or byacc, for bc); upx if there.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
port=$root/third_party/sbase
out=${OUT:-$root/deps/sbase}
rev=c1341583c96307cb0e6152c963ed23c4d56a4278   # sbase master, 2026-04-03
p=i586-pc-msdosdjgpp-
if [ -n "$DJGPP" ]; then PATH="$DJGPP/bin:$PATH"
elif ! command -v ${p}gcc > /dev/null; then
    sh "$root/tools/get-djgpp.sh" "$root/deps/djgpp"
    PATH="$root/deps/djgpp/bin:$PATH"
fi
want=$(grep -v '^#' "$port/TOOLS")
tmp=$(mktemp -d "${TMPDIR:-/tmp}/build-sbase.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

S=$tmp/sbase
if [ -n "$SBASESRC" ]; then
    git -C "$SBASESRC" archive --prefix=sbase/ $rev | tar -x -C "$tmp"
else
    for url in ${SBASE_GIT:-https://git.suckless.org/sbase https://github.com/fQwQf/sbase}; do
        rm -rf "$S"
        git clone -q --depth 200 "$url" "$S" 2>/dev/null &&
            git -c advice.detachedHead=false -C "$S" checkout -q $rev 2>/dev/null && break
        echo "build-sbase: no sbase $rev from $url, trying the next" >&2
        rm -rf "$S"
    done
    [ -d "$S" ] || { echo "build-sbase: couldn't get sbase's source" >&2; exit 1; }
fi
patch -s -d "$S" -p1 < "$port/sbase-dos.patch"
# sbase drives its build with its own make, built for the host by $(CC):
# use this one instead
printf '#!/bin/sh\nexec %s "$@"\n' "${MAKE:-make}" > "$S/scripts/make"
chmod +x "$S/scripts/make"

# A 486 runs them (vmdos's CPU=486 image)
cflags="-O2 -march=i486 -mtune=pentium"
D=$port/dos
${p}gcc $cflags -Wall -I"$D/include" -c "$D/sbase-dos.c" -o "$tmp/sbase-dos.o"
make -C "$S" -j"$(nproc 2>/dev/null || echo 2)" CC=${p}gcc AR=${p}ar RANLIB=${p}ranlib \
    CFLAGS="$cflags -include $D/sbase-dos.h -I$D/include" \
    LDFLAGS="$tmp/sbase-dos.o" $want > "$tmp/build.log" 2>&1 ||
    { grep -E 'error|\*\*\*' "$tmp/build.log" | head -30; exit 1; }

rm -rf "$out"
mkdir -p "$out"
for t in $want; do
    u=$(echo "$t" | tr a-z A-Z)
    ${p}strip -o "$tmp/strip.exe" "$S/$t"
    if command -v upx > /dev/null; then
        upx -q --best -o "$out/$u.EXE" "$tmp/strip.exe" > /dev/null
    else
        cp "$tmp/strip.exe" "$out/$u.EXE"
    fi
    rm -f "$tmp/strip.exe"
done
echo "build-sbase: $out: $(ls "$out" | wc -l) tools, $(du -sh "$out" | cut -f1)"
