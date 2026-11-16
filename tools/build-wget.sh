#!/bin/sh
# Build third_party/wget/bin/WGET.EXE: downloads over HTTP and HTTPS for
# DOS (DJGPP), on vmdos's own TCP through ssh's socket layer
# (third_party/openssh/dos-sock.c) with mbedTLS for TLS. Nothing of it is in
# the kernel. The normal build doesn't need this: the binary is committed.
# Needs:
#   DJGPP   a cross gcc i586-pc-msdosdjgpp-gcc on PATH, or DJGPP=dir with bin/
#           (github.com/andrewwutw/build-djgpp releases)
#   git, a host gcc with -m32 (gcc-multilib), upx
#   WATTSRC=dir    an existing checkout of github.com/gvanem/Watt-32
#   MBEDTLSSRC=dir an existing checkout of github.com/Mbed-TLS/mbedtls
#   KEEP_TMP=1     keep the build directory
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
port=$root/third_party/wget
ssh=$root/third_party/openssh
[ -n "$DJGPP" ] && PATH="$DJGPP/bin:$PATH"
watt_rev=3cf6f4e504b46bea2302b8d88b5ff2431396ddc9     # as tools/build-openssh.sh
mbedtls_tag=v3.6.4
dj=$(dirname "$(command -v i586-pc-msdosdjgpp-gcc)")/..
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
[ -n "$KEEP_TMP" ] && { trap - EXIT; echo "build dir: $tmp"; }

# Watt-32: its socket headers (and errno list for DJGPP), as for ssh; none
# of its code is linked.
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

# mbedTLS, its default configuration less what DOS can't use (wget_mbedtls_config.h)
if [ -n "$MBEDTLSSRC" ]; then
    git -C "$MBEDTLSSRC" archive --prefix=mbedtls/ $mbedtls_tag | tar -x -C "$tmp"
else
    git -c advice.detachedHead=false clone -q --depth 1 --branch $mbedtls_tag https://github.com/Mbed-TLS/mbedtls "$tmp/mbedtls"
fi
M=$tmp/mbedtls
MCF="-O2 -march=i586 -I$M/include -I$M/library -I$port -DMBEDTLS_USER_CONFIG_FILE=\"wget_mbedtls_config.h\""
mkdir "$tmp/mb"
for f in "$M"/library/*.c; do
    i586-pc-msdosdjgpp-gcc $MCF -c "$f" -o "$tmp/mb/$(basename "$f" .c).o"
done
i586-pc-msdosdjgpp-ar rcs "$tmp/libmbedtls.a" "$tmp"/mb/*.o

# WGET: wget.c, ssh's dos-sock.c, and the little dos-sock.c needs from the
# rest of the ssh port (dos/: wget-dos.c and a few headers)
CF="-O2 -march=i586 -Wall -I$port/dos -I$ssh -I$W/inc -DWATT32 -DWATT32_NO_OLDIES"
cd "$tmp"
i586-pc-msdosdjgpp-gcc $CF $MCF -c "$port/wget.c" -o wget.o
i586-pc-msdosdjgpp-gcc $CF -c "$ssh/dos-sock.c" -o dos-sock.o
i586-pc-msdosdjgpp-gcc $CF -c "$port/dos/wget-dos.c" -o wget-dos.o
i586-pc-msdosdjgpp-gcc -o wget.exe wget.o dos-sock.o wget-dos.o libmbedtls.a
i586-pc-msdosdjgpp-strip wget.exe
mkdir -p "$port/bin"
rm -f "$port/bin/WGET.EXE"
upx -q --best -o "$port/bin/WGET.EXE" wget.exe > /dev/null
ls -l "$port/bin/WGET.EXE"
