#!/bin/sh
# Build third_party/lynx/bin/LYNX.EXE: the Lynx text web browser for DOS
# (DJGPP), on vmdos's own TCP through ssh's socket layer
# (third_party/openssh/dos-sock.c), with OpenSSL for HTTPS
# (tools/build-openssl.sh) and PDCurses for the screen. See third_party/lynx/README.vmdos. The normal build doesn't
# need this: the binary is committed. Needs:
#   DJGPP   a cross gcc i586-pc-msdosdjgpp-gcc on PATH, or DJGPP=dir with bin/
#           (github.com/andrewwutw/build-djgpp releases)
#   git, make, perl, a host gcc with -m32 (gcc-multilib), python3, upx
#   LYNXSRC=dir    an existing clone of github.com/ThomasDickey/lynx-snapshots
#   WATTSRC=dir    an existing checkout of github.com/gvanem/Watt-32
#   OPENSSLSRC=dir an existing checkout of github.com/openssl/openssl
#   KEEP_TMP=1     keep the build directory
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
port=$root/third_party/lynx
ssh=$root/third_party/openssh
wget=$root/third_party/wget
[ -n "$DJGPP" ] && PATH="$DJGPP/bin:$PATH"
lynx_rev=bf39a4f6c05ed874b63fdde5efd9b8369818feb5  # lynx-snapshots: label v2-9-3 (Lynx 2.9.3, 2026-05-27)
lynx_ver=2.9.3
pdcurses_tag=3.9
zlib_tag=v1.3.1
watt_rev=3cf6f4e504b46bea2302b8d88b5ff2431396ddc9  # as tools/build-openssh.sh
command -v i586-pc-msdosdjgpp-gcc > /dev/null ||
    { echo "build-lynx: no i586-pc-msdosdjgpp-gcc (DJGPP=dir?)" >&2; exit 1; }
dj=$(dirname "$(command -v i586-pc-msdosdjgpp-gcc)")/..
P=i586-pc-msdosdjgpp-
tmp=$(mktemp -d "${TMPDIR:-/tmp}/build-lynx.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
[ -n "$KEEP_TMP" ] && { trap - EXIT; echo "build dir: $tmp"; }
dep=$tmp/dep
mkdir -p "$dep/include" "$dep/lib"
CF="-O2 -march=i586"

# Lynx 2.9.3 (the snapshot repository's commit for the release)
if [ -n "$LYNXSRC" ]; then
    git -C "$LYNXSRC" archive --prefix=lynx/ $lynx_rev | tar -x -C "$tmp"
else
    git clone -q --filter=blob:none --no-checkout https://github.com/ThomasDickey/lynx-snapshots "$tmp/lynx-git"
    git -C "$tmp/lynx-git" archive --prefix=lynx/ $lynx_rev | tar -x -C "$tmp"
fi
grep -q "LYNX_VERSION  *\"$lynx_ver\"" "$tmp/lynx/userdefs.h" ||
    { echo "build-lynx: $lynx_rev isn't Lynx $lynx_ver" >&2; exit 1; }

# Watt-32: its socket headers and errno list for DJGPP (as for ssh); none
# of its code is linked (ossl-dos.c, lynx-dos.c and dos-sock.c are the
# network).
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

# OpenSSL's libssl and libcrypto (tools/build-openssl.sh)
OPENSSL_PREFIX=$tmp/ossl NO_EXE=1 WATTSRC=$WATTSRC OPENSSLSRC=$OPENSSLSRC \
    sh "$root/tools/build-openssl.sh"

# PDCurses (its DOS port: BIOS and the text screen) as libcurses.a
git -c advice.detachedHead=false clone -q --depth 1 --branch $pdcurses_tag https://github.com/wmcbrine/PDCurses "$tmp/pdcurses"
mkdir "$tmp/pdc.o"
for f in "$tmp"/pdcurses/pdcurses/*.c "$tmp"/pdcurses/dos/*.c; do
    ${P}gcc $CF -I"$tmp/pdcurses" -c "$f" -o "$tmp/pdc.o/$(basename "$f" .c).o"
done
${P}ar rcs "$dep/lib/libcurses.a" "$tmp"/pdc.o/*.o
cp "$tmp/pdcurses/curses.h" "$tmp/pdcurses/panel.h" "$dep/include/"

# zlib: gzip and deflate encoded pages
git -c advice.detachedHead=false clone -q --depth 1 --branch $zlib_tag https://github.com/madler/zlib "$tmp/zlib"
mkdir "$tmp/z.o"
for f in adler32 crc32 deflate infback inffast inflate inftrees trees zutil \
         compress uncompr gzclose gzlib gzread gzwrite; do
    ${P}gcc $CF -DHAVE_UNISTD_H -c "$tmp/zlib/$f.c" -o "$tmp/z.o/$f.o"
done
${P}ar rcs "$dep/lib/libz.a" "$tmp"/z.o/*.o
cp "$tmp/zlib/zlib.h" "$tmp/zlib/zconf.h" "$dep/include/"

# The network glue: ossl-dos.c (Watt-32's calls, shared with OPENSSL.EXE),
# lynx-dos.c (Lynx's own on top), ssh's dos-sock.c
GF="$CF -Wall -I$wget/dos -I$ssh -I$W/inc -I$tmp/ossl/include -I$dep/include -DWATT32 -DWATT32_NO_OLDIES"
${P}gcc $GF -c "$root/third_party/openssl/ossl-dos.c" -o "$tmp/ossl-dos.o"
${P}gcc $GF -c "$ssh/dos-sock.c" -o "$tmp/dos-sock.o"
${P}gcc $GF -c "$port/lynx-dos.c" -o "$tmp/lynx-dos.o"
NET="$tmp/ossl-dos.o $tmp/dos-sock.o"
# Lynx's configure adds -lwatt for DJGPP itself: an empty one stands in
${P}ar rc "$dep/lib/libwatt.a"

# Lynx's configure, cross: PDCurses as plain curses (no default-colors:
# Lynx's restart_curses() wants ncurses for that, and DOS has no default;
# no color-style: Lynx's classic colors, no LYNX.LSS to carry), IPv6 (getaddrinfo is
# dos-sock.c's), zlib, OpenSSL. Then lynx-dos.h into every file, and
# linked with the glue instead of Watt-32 (which configure never sees
# either: the glue answers its checks).
cd "$tmp/lynx"
# lynx-dos.patch: the mouse (PDCurses 3's DOS port has one; Lynx's idle
# loop wakes up for it)
patch -s -p1 < "$port/lynx-dos.patch"
cf_cv_getaddrinfo=yes ./configure --host=i586-pc-msdosdjgpp --build="$(./config.guess)" \
    --with-screen=curses --without-x --enable-ipv6 --disable-nls \
    --with-ssl="$tmp/ossl" --with-zlib --without-bzlib --disable-default-colors --disable-color-style --enable-externs \
    --enable-nested-tables --enable-prettysrc --enable-source-cache \
    --enable-persistent-cookies --enable-addrlistpage --disable-full-paths \
    CC=${P}gcc BUILD_CC=gcc CFLAGS="$CF" \
    CPPFLAGS="-I$dep/include -I$W/inc -DWATT32 -DWATT32_NO_OLDIES" \
    LDFLAGS="-L$dep/lib" LIBS="$NET" > configure.log 2>&1 ||
    { tail -30 configure.log; exit 1; }
cat >> lynx_cfg.h <<'EOF2'
/* build-lynx.sh: LYNX.CFG is next to LYNX.EXE in C:\VMDOS\NET (LYNX_CFG=
   or -cfg= for another) */
#undef LYNX_CFG_FILE
#undef LYNX_CFG_PATH
#define LYNX_CFG_FILE "C:/VMDOS/NET/LYNX.CFG"
#define LYNX_CFG_PATH "C:/VMDOS/NET"
EOF2
grep -q "^#define USE_SSL" lynx_cfg.h || { echo "build-lynx: configure found no OpenSSL" >&2; tail -30 configure.log; exit 1; }
extra="-include $port/lynx-dos.h"
for m in makefile src/makefile WWW/Library/Implementation/makefile; do
    sed -i "s|^CPPFLAGS\(\s*\)=\(.*\)|CPPFLAGS\1=\2 $extra|" $m
done
for m in makefile src/makefile; do
    sed -i "s|^LIBS\(\s*\)=.*|LIBS\1= $tmp/lynx-dos.o $NET -L$tmp/ossl/lib -lssl -lcrypto -lz -lcurses|" $m
done
make -j"$(nproc)" > make.log 2>&1 || { grep -E "error|Error" make.log | head -30; exit 1; }
${P}strip lynx.exe
mkdir -p "$port/bin"
rm -f "$port/bin/LYNX.EXE"
if command -v upx > /dev/null; then
    upx -q --best -o "$port/bin/LYNX.EXE" lynx.exe > /dev/null
else
    echo "build-lynx: no upx: LYNX.EXE left uncompressed" >&2
    cp lynx.exe "$port/bin/LYNX.EXE"
fi
ls -l "$port/bin/LYNX.EXE"
