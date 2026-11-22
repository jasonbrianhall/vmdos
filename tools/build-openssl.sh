#!/bin/sh
# Build third_party/openssl/bin/OPENSSL.EXE: the openssl command for DOS
# (DJGPP), on vmdos's own TCP through ssh's socket layer
# (third_party/openssh/dos-sock.c, third_party/openssl/ossl-dos.c). See
# third_party/openssl/README.vmdos. The normal build doesn't need this: the
# binary is committed. tools/build-lynx.sh uses this script for LYNX.EXE's
# libssl and libcrypto (OPENSSL_PREFIX=dir NO_EXE=1). Needs:
#   DJGPP   a cross gcc i586-pc-msdosdjgpp-gcc on PATH, or DJGPP=dir with bin/
#           (github.com/andrewwutw/build-djgpp releases)
#   git, make, perl, a host gcc with -m32 (gcc-multilib), python3, upx
#   OPENSSLSRC=dir  an existing checkout of github.com/openssl/openssl
#   WATTSRC=dir     an existing checkout of github.com/gvanem/Watt-32
#   OPENSSL_PREFIX=dir  also install the headers and libraries there
#   NO_EXE=1        don't build OPENSSL.EXE (libraries only)
#   OPENSSL_EXTRA=  more options for its Configure (tools/build-openssh.sh: no-sock)
#   KEEP_TMP=1      keep the build directory
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
port=$root/third_party/openssl
ssh=$root/third_party/openssh
wget=$root/third_party/wget
[ -n "$DJGPP" ] && PATH="$DJGPP/bin:$PATH"
openssl_tag=openssl-3.6.5
watt_rev=3cf6f4e504b46bea2302b8d88b5ff2431396ddc9  # as tools/build-openssh.sh
command -v i586-pc-msdosdjgpp-gcc > /dev/null ||
    { echo "build-openssl: no i586-pc-msdosdjgpp-gcc (DJGPP=dir?)" >&2; exit 1; }
dj=$(dirname "$(command -v i586-pc-msdosdjgpp-gcc)")/..
P=i586-pc-msdosdjgpp-
tmp=$(mktemp -d "${TMPDIR:-/tmp}/build-openssl.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
[ -n "$KEEP_TMP" ] && { trap - EXIT; echo "build dir: $tmp"; }

# Watt-32: its socket headers and errno list for DJGPP (as for ssh); none
# of its code is linked (ossl-dos.c and dos-sock.c are the network).
if [ -n "$WATTSRC" ]; then
    git -C "$WATTSRC" archive --prefix=Watt-32/ $watt_rev | tar -x -C "$tmp"
else
    git -c advice.detachedHead=false clone -q https://github.com/gvanem/Watt-32 "$tmp/Watt-32"
    git -c advice.detachedHead=false -C "$tmp/Watt-32" checkout -q $watt_rev
fi
W=$tmp/Watt-32
inc=$dj/i586-pc-msdosdjgpp/sys-include
python3 "$ssh/djstrerr.py" "$inc/errno.h" > "$tmp/djstrerr.h"
gcc -m32 -w -DWATT32_DJGPP_MINGW --include "$inc/errno.h" -D_ERRNO_H -D_BITS_ERRNO_H \
    --include "$inc/sys/version.h" -include "$tmp/djstrerr.h" -idirafter "$W/inc" \
    -o "$tmp/dj_err" "$W/util/errnos.c"
(cd "$W/src" && sh configur.sh djgpp > /dev/null && "$tmp/dj_err" -e > ../inc/sys/djgpp.err)

# OpenSSL: its DJGPP target, static, no threads, DSOs or engines.
# openssl-dos.patch: seeded by getentropy() (ossl-dos.c's); non-blocking
# sockets by ioctlsocket() (DJGPP's fcntl() doesn't know them); the
# command's name in any case (OPENSSL VERSION). dos-time.h in every file:
# the clock without DJGPP's time code (ossl-dos.c's). Its default places
# (OPENSSLDIR) are C:\VMDOS\NET (DJGPP: /dev/c/VMDOS/NET): OPENSSL.CNF there.
if [ -n "$OPENSSLSRC" ]; then
    git -C "$OPENSSLSRC" archive --prefix=openssl/ $openssl_tag | tar -x -C "$tmp"
else
    git -c advice.detachedHead=false clone -q --depth 1 --branch $openssl_tag https://github.com/openssl/openssl "$tmp/openssl"
fi
cd "$tmp/openssl"
patch -s -p1 < "$port/openssl-dos.patch"
./Configure DJGPP --cross-compile-prefix=$P no-shared no-dso no-threads \
    no-async no-tests no-docs no-engine --with-rand-seed=getrandom $OPENSSL_EXTRA \
    --prefix="${OPENSSL_PREFIX:-$tmp/inst}" --openssldir=/dev/c/VMDOS/NET \
    "-I$W/inc" "-include $port/dos-time.h" > configure.log 2>&1 || { tail -30 configure.log; exit 1; }
make -j"$(nproc)" build_libs > make.log 2>&1 || { grep -E "error|Error" make.log | head -30; exit 1; }
[ -n "$OPENSSL_PREFIX" ] && make install_dev > install.log 2>&1
[ -n "$NO_EXE" ] && exit 0

# The openssl command: linked with ossl-dos.c and dos-sock.c instead of
# Watt-32 (its target names -lwatt: an empty one stands in)
GF="-O2 -march=i586 -Wall -I$wget/dos -I$ssh -I$W/inc -DWATT32 -DWATT32_NO_OLDIES"
${P}gcc $GF -c "$port/ossl-dos.c" -o "$tmp/ossl-dos.o"
${P}gcc $GF -c "$ssh/dos-sock.c" -o "$tmp/dos-sock.o"
mkdir "$tmp/nowatt"
${P}ar rc "$tmp/nowatt/libwatt.a"
make -j"$(nproc)" build_programs LDFLAGS="-L$tmp/nowatt" \
    EX_LIBS="$tmp/ossl-dos.o $tmp/dos-sock.o" > apps.log 2>&1 ||
    { grep -E "error|undefined" apps.log | head -30; exit 1; }
${P}strip apps/openssl.exe
mkdir -p "$port/bin"
rm -f "$port/bin/OPENSSL.EXE"
if command -v upx > /dev/null; then
    upx -q --best -o "$port/bin/OPENSSL.EXE" apps/openssl.exe > /dev/null
else
    echo "build-openssl: no upx: OPENSSL.EXE left uncompressed" >&2
    cp apps/openssl.exe "$port/bin/OPENSSL.EXE"
fi
sed 's/$/\r/' apps/openssl.cnf > "$port/OPENSSL.CNF"
ls -l "$port/bin/OPENSSL.EXE"
