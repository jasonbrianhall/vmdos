#!/bin/sh
# Build third_party/openssh/bin/SSH.EXE: OpenSSH's ssh client for DOS
# (DJGPP + Watt-32 on vmdos's packet driver; see third_party/openssh/README.vmdos).
# The normal build doesn't need this: the binary is committed. Needs:
#   DJGPP   a cross gcc i586-pc-msdosdjgpp-gcc on PATH, or DJGPP=dir with bin/
#           (github.com/andrewwutw/build-djgpp releases)
#   git, make, autoconf, a host gcc with -m32 (gcc-multilib), libfl2, upx
#   SSHSRC=dir   an existing checkout of github.com/openssh/openssh-portable
#   WATTSRC=dir  an existing checkout of github.com/gvanem/Watt-32
#   SSH_KEEP=file  also keep the unstripped ssh.exe there (symbols, for debugging)
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
port=$root/third_party/openssh
[ -n "$DJGPP" ] && PATH="$DJGPP/bin:$PATH"
ssh_tag=V_10_6_P2
watt_rev=3cf6f4e504b46bea2302b8d88b5ff2431396ddc9
dj=$(dirname "$(command -v i586-pc-msdosdjgpp-gcc)")/..
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Watt-32: TCP/IP with BSD sockets on a packet driver (VMPKT).
if [ -n "$WATTSRC" ]; then
    git -C "$WATTSRC" archive --prefix=Watt-32/ $watt_rev | tar -x -C "$tmp"
else
    git -c advice.detachedHead=false clone -q https://github.com/gvanem/Watt-32 "$tmp/Watt-32"
    git -c advice.detachedHead=false -C "$tmp/Watt-32" checkout -q $watt_rev
fi
W=$tmp/Watt-32
export WATT_ROOT=$W
# Its errno list comes from a program built against DJGPP's <errno.h>; run
# that on the host (as util/dj-errno.mak does with MinGW) with DJGPP's numbers.
inc=$dj/i586-pc-msdosdjgpp/sys-include
python3 "$port/djstrerr.py" "$inc/errno.h" > "$tmp/djstrerr.h"
gcc -m32 -w -DWATT32_DJGPP_MINGW --include "$inc/errno.h" -D_ERRNO_H -D_BITS_ERRNO_H \
    --include "$inc/sys/version.h" -include "$tmp/djstrerr.h" -idirafter "$W/inc" \
    -o "$tmp/dj_err" "$W/util/errnos.c"
(cd "$W/src" && sh configur.sh djgpp > /dev/null &&
    "$tmp/dj_err" -e > ../inc/sys/djgpp.err &&
    "$tmp/dj_err" -s > build/djgpp/syserr.c &&
    make -f djgpp.mak -j"$(nproc)" > "$tmp/watt.log" 2>&1) ||
    { tail -30 "$tmp/watt.log"; exit 1; }

# OpenSSH, the client only, with its own crypto (no OpenSSL).
if [ -n "$SSHSRC" ]; then
    git -C "$SSHSRC" archive --prefix=openssh/ $ssh_tag | tar -x -C "$tmp"
else
    git -c advice.detachedHead=false clone -q --depth 1 --branch $ssh_tag https://github.com/openssh/openssh-portable "$tmp/openssh"
fi
(cd "$tmp/openssh" && autoreconf > /dev/null 2>&1)
mkdir "$tmp/build"
cd "$tmp/build"
ac_cv_have_struct_timespec=yes ac_cv_func_ppoll=yes ac_cv_func_poll=yes \
ac_cv_func_getentropy=yes \
"$tmp/openssh/configure" --host=i586-pc-msdosdjgpp \
    --without-openssl --without-zlib --without-pam --without-selinux \
    --without-security-key-builtin --disable-security-key --disable-pkcs11 \
    --disable-lastlog --disable-utmp --disable-wtmp --disable-utmpx --disable-wtmpx \
    --without-hardening --without-stackprotect --without-retpoline \
    --with-sandbox=no --disable-strip \
    CFLAGS="-O2 -march=i586" CPPFLAGS="-I$W/inc -DWATT32 -DWATT32_NO_OLDIES" \
    LDFLAGS="-L$W/lib" LIBS="-lwatt" > configure.log 2>&1 ||
    { tail -30 configure.log; exit 1; }
# dos-compat.h goes into every file; dos-compat.c is what DOS lacks,
# dos-term.c the terminal.
CC="i586-pc-msdosdjgpp-gcc -include $port/dos-compat.h"
for f in dos-compat dos-term; do
    $CC -O2 -march=i586 -Wall -I. -I"$tmp/openssh" -I"$W/inc" -DWATT32 -DWATT32_NO_OLDIES \
        -DHAVE_CONFIG_H -c "$port/$f.c" -o $f.o
done
i586-pc-msdosdjgpp-ar rcs libdoscompat.a dos-compat.o dos-term.o
make -j"$(nproc)" ssh.exe CC="$CC" sysconfdir=c:/vmdos/ssh \
    LIBS="-L. -ldoscompat -lopenbsd-compat -lwatt" > make.log 2>&1 ||
    { grep -E -A3 "error:|undefined reference" make.log | head -40; exit 1; }
[ -n "$SSH_KEEP" ] && cp ssh.exe "$SSH_KEEP"
i586-pc-msdosdjgpp-strip ssh.exe
mkdir -p "$port/bin"
rm -f "$port/bin/SSH.EXE"
upx -q --best -o "$port/bin/SSH.EXE" ssh.exe > /dev/null
ls -l "$port/bin/SSH.EXE"
