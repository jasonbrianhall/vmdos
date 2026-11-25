#!/bin/sh
# Fetch the DJGPP cross compiler (gcc 12.2, binutils, the DJGPP C library) for
# x86_64 Linux into DIR (default deps/djgpp): DIR/bin/i586-pc-msdosdjgpp-gcc.
# It's the build-djgpp project's release (github.com/andrewwutw/build-djgpp),
# checked against its SHA-256. tools/build-bash.sh and tools/build-sbase.sh
# use it when no i586-pc-msdosdjgpp-gcc is on PATH and DJGPP isn't set.
set -e
dest=${1:-deps/djgpp}

# Its ar and ranlib were linked with flex's libfl.so.2 but use nothing from
# it, and few systems have it: name libc.so.6 there instead (same length
# with its NUL; a second libc is no harm).
unflex() {
    for b in "$dest/bin/i586-pc-msdosdjgpp-ar" "$dest/bin/i586-pc-msdosdjgpp-ranlib" \
             "$dest/i586-pc-msdosdjgpp/bin/ar" "$dest/i586-pc-msdosdjgpp/bin/ranlib"; do
        [ -f "$b" ] || continue
        python3 - "$b" <<'PY'
import sys
p = sys.argv[1]
b = open(p, 'rb').read()
old, new = b'\0libfl.so.2\0', b'\0libc.so.6\0\0'
if b.count(old) == 1:
    open(p, 'wb').write(b.replace(old, new))
PY
    done
}
url=https://github.com/andrewwutw/build-djgpp/releases/download/v3.4/djgpp-linux64-gcc1220.tar.bz2
sum=8464f17017d6ab1b2bb2df4ed82357b5bf692e6e2b7fee37e315638f3d505f00
if [ -x "$dest/bin/i586-pc-msdosdjgpp-gcc" ]; then unflex; exit 0; fi
case $(uname -sm) in
    "Linux x86_64") ;;
    *) echo "get-djgpp: this toolchain is for x86_64 Linux; install DJGPP and set DJGPP=dir" >&2; exit 1 ;;
esac
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
echo "get-djgpp: downloading the DJGPP cross compiler (80 MB)"
curl -fsSL --retry 3 -o "$tmp/djgpp.tar.bz2" "$url"
echo "$sum  $tmp/djgpp.tar.bz2" | sha256sum -c --quiet
mkdir -p "$tmp/x"
tar -xjf "$tmp/djgpp.tar.bz2" -C "$tmp/x"
mkdir -p "$(dirname "$dest")"
rm -rf "$dest"
mv "$tmp/x/djgpp" "$dest"
unflex
"$dest/bin/i586-pc-msdosdjgpp-gcc" --version | head -1
