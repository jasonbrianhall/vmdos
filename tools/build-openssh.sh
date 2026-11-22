#!/bin/sh
# Build third_party/openssh/bin/SSH.EXE, SFTP.EXE and SCP.EXE: OpenSSH's
# clients for DOS (DJGPP, on vmdos's own TCP; see third_party/openssh/README.vmdos).
# The normal build doesn't need this: the binary is committed. Needs:
#   DJGPP   a cross gcc i586-pc-msdosdjgpp-gcc on PATH, or DJGPP=dir with bin/
#           (github.com/andrewwutw/build-djgpp releases)
#   git, make, autoconf, a host gcc with -m32 (gcc-multilib), libfl2, upx
#   SSHSRC=dir   an existing checkout of github.com/openssh/openssh-portable
#   WATTSRC=dir  an existing checkout of github.com/gvanem/Watt-32
#   SSH_TAG=tag  another OpenSSH release than the one openssh-dos.patch is for
#   SSH_KEEP=file  also keep the unstripped ssh.exe there (symbols, for debugging)
#   KEEP_TMP=1     keep the build directory
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
port=$root/third_party/openssh
[ -n "$DJGPP" ] && PATH="$DJGPP/bin:$PATH"
# OpenSSH's newest release (git tag; SSH_TAG= another): openssh-dos.patch is
# made against it, so a new one may need the patch brought up to date.
ssh_tag=${SSH_TAG:-V_10_6_P2}
watt_rev=3cf6f4e504b46bea2302b8d88b5ff2431396ddc9
dj=$(dirname "$(command -v i586-pc-msdosdjgpp-gcc)")/..
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
[ -n "$KEEP_TMP" ] && { trap - EXIT; echo "build dir: $tmp"; }

# Watt-32 (a DOS TCP/IP library): its socket headers and errno list for
# the build, and functions for OpenSSH's configure to find. The programs
# use none of its code (dos-sock.c: vmdos's TCP).
if [ -n "$WATTSRC" ]; then
    git -C "$WATTSRC" archive --prefix=Watt-32/ $watt_rev | tar -x -C "$tmp"
else
    git -c advice.detachedHead=false clone -q https://github.com/gvanem/Watt-32 "$tmp/Watt-32"
    git -c advice.detachedHead=false -C "$tmp/Watt-32" checkout -q $watt_rev
fi
W=$tmp/Watt-32
export WATT_ROOT=$W
# lang.c comes with lang.l (made from it with flex): a clone gives files
# whatever times their checkout did, and if lang.l came out newer make
# wanted flex to make lang.c again.
touch "$W/src/lang.c"
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
# DOS's changes: the user name prompt, ssh run inside sftp and scp (README.vmdos)
(cd "$tmp/openssh" && patch -s -p1 < "$port/openssh-dos.patch" && autoreconf > /dev/null 2>&1)
mkdir "$tmp/build"
cd "$tmp/build"
ac_cv_have_struct_timespec=yes ac_cv_func_ppoll=yes ac_cv_func_poll=yes \
ac_cv_func_getentropy=yes ac_cv_func_closefrom=yes \
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
for f in dos-co dos-sock dos-res; do
    $CC -O2 -march=i586 -Wall -I. -I"$tmp/openssh" -I"$W/inc" -DWATT32 -DWATT32_NO_OLDIES \
        -DHAVE_CONFIG_H -c "$port/$f.c" -o $f.o
done
i586-pc-msdosdjgpp-ar rcs libdoscompat.a dos-compat.o dos-term.o dos-co.o dos-sock.o dos-res.o
# The programs use vmdos's own TCP (dos-sock.c); Watt-32 gives configure
# its headers and functions to find, and nothing to the programs.
LIBS="-L. -ldoscompat -lopenbsd-compat -ldoscompat"
make -j"$(nproc)" ssh.exe sftp.o sftp-usergroup.o progressmeter.o sftp-common.o \
    sftp-client.o sftp-glob.o ssherr-nolibcrypto.o scp.o CC="$CC" sysconfdir=c:/vmdos/ssh LIBS="$LIBS" \
    > make.log 2>&1 || { grep -E -A3 "error:|undefined reference" make.log | head -40; exit 1; }
# SFTP.EXE and SCP.EXE carry ssh inside (dos-co.c, dos-spawn.c): ssh.c's
# main and clientloop.c's cleanup_exit get other names, and so does scp's
# cleanup_exit (dos-spawn.c's picks between them).
i586-pc-msdosdjgpp-objcopy --redefine-sym _main=_ssh_main ssh.o ssh-in.o
i586-pc-msdosdjgpp-objcopy --redefine-sym _cleanup_exit=_ssh_cleanup_exit \
    clientloop.o clientloop-in.o
$CC -O2 -march=i586 -Wall -I. -I"$tmp/openssh" -I"$W/inc" -DWATT32 -DWATT32_NO_OLDIES \
    -DHAVE_CONFIG_H -c "$port/dos-spawn.c" -o dos-spawn.o
SSH_IN="dos-spawn.o ssh-in.o readconf.o clientloop-in.o sshtty.o sshconnect.o
    sshconnect2.o mux.o ssh-pkcs11.o ssh-sk-client.o"
for p in sftp scp; do
    i586-pc-msdosdjgpp-objcopy --redefine-sym _cleanup_exit=_prog_cleanup_exit $p.o $p-prog.o
done
LINK="-L. -Lopenbsd-compat -lssh -lopenbsd-compat $LIBS"
i586-pc-msdosdjgpp-gcc -o sftp.exe sftp-prog.o sftp-usergroup.o progressmeter.o \
    sftp-common.o sftp-client.o sftp-glob.o ssherr-nolibcrypto.o $SSH_IN $LINK
i586-pc-msdosdjgpp-gcc -o scp.exe scp-prog.o progressmeter.o \
    sftp-common.o sftp-client.o sftp-glob.o ssherr-nolibcrypto.o $SSH_IN $LINK
[ -n "$SSH_KEEP" ] && cp ssh.exe "$SSH_KEEP"
mkdir -p "$port/bin"
for p in ssh sftp scp; do
    i586-pc-msdosdjgpp-strip $p.exe
    P=$(echo $p | tr a-z A-Z)
    rm -f "$port/bin/$P.EXE"
    upx -q --best -o "$port/bin/$P.EXE" $p.exe > /dev/null
    ls -l "$port/bin/$P.EXE"
done
