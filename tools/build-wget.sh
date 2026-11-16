#!/bin/sh
# Build third_party/wget/bin/WGET.EXE: downloads over HTTP and HTTPS for
# DOS (DJGPP), on vmdos's own TCP through ssh's socket layer
# (third_party/openssh/dos-sock.c) with mbedTLS for TLS. Nothing of it is in
# the kernel. The normal build doesn't need this: the binary is committed.
#
# DJGPP, either of:
#   a cross gcc i586-pc-msdosdjgpp-gcc on PATH (or DJGPP=dir with bin/), or
#   Docker: the image djfdyuruiry/djgpp (DJGPP_IMAGE=... for another one),
#           used when there's no cross gcc; DJGPP_DOCKER=1 to prefer it
# and: git, a host gcc with -m32 (gcc-multilib), python3; upx if there (smaller)
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
image=${DJGPP_IMAGE:-djfdyuruiry/djgpp}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/build-wget.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
[ -n "$KEEP_TMP" ] && { trap - EXIT; echo "build dir: $tmp"; }

# Which DJGPP: the cross compiler, or Docker.
if [ -z "$DJGPP_DOCKER" ] && command -v i586-pc-msdosdjgpp-gcc > /dev/null; then
    docker=
    dj=$(dirname "$(command -v i586-pc-msdosdjgpp-gcc)")/..
    djinc=$dj/i586-pc-msdosdjgpp/sys-include
else
    command -v docker > /dev/null || { echo "build-wget: no i586-pc-msdosdjgpp-gcc and no docker" >&2; exit 1; }
    docker=1
    docker pull -q "$image" > /dev/null
    # DJGPP's headers, out of the image, for the errno list below
    djinc=$tmp/djinc
    mkdir -p "$djinc"
    d=$(docker run --rm "$image" sh -c 'echo "#include <errno.h>" | gcc -x c -E -M - 2>/dev/null' |
        tr ' \\' '\n\n' | grep '/errno\.h$' | head -1 | xargs dirname)
    [ -n "$d" ] || { echo "build-wget: can't find DJGPP's errno.h in $image" >&2; exit 1; }
    docker run --rm "$image" tar -C "$d" -cf - . | tar -x -C "$djinc"
fi

# Watt-32: its socket headers (and errno list for DJGPP), as for ssh; none
# of its code is linked.
if [ -n "$WATTSRC" ]; then
    git -C "$WATTSRC" archive --prefix=Watt-32/ $watt_rev | tar -x -C "$tmp"
else
    git -c advice.detachedHead=false clone -q https://github.com/gvanem/Watt-32 "$tmp/Watt-32"
    git -c advice.detachedHead=false -C "$tmp/Watt-32" checkout -q $watt_rev
fi
W=$tmp/Watt-32
python3 "$ssh/djstrerr.py" "$djinc/errno.h" > "$tmp/djstrerr.h"
gcc -m32 -w -DWATT32_DJGPP_MINGW --include "$djinc/errno.h" -D_ERRNO_H -D_BITS_ERRNO_H \
    --include "$djinc/sys/version.h" -include "$tmp/djstrerr.h" -idirafter "$W/inc" \
    -o "$tmp/dj_err" "$W/util/errnos.c"
(cd "$W/src" && sh configur.sh djgpp > /dev/null && "$tmp/dj_err" -e > ../inc/sys/djgpp.err)

# mbedTLS: its default configuration less what DOS can't use (wget_mbedtls_config.h)
if [ -n "$MBEDTLSSRC" ]; then
    git -C "$MBEDTLSSRC" archive --prefix=mbedtls/ $mbedtls_tag | tar -x -C "$tmp"
else
    git -c advice.detachedHead=false clone -q --depth 1 --branch $mbedtls_tag https://github.com/Mbed-TLS/mbedtls "$tmp/mbedtls"
fi

# The DJGPP part, in one go (in the image, or here): mbedTLS's library,
# wget.c, ssh's dos-sock.c and what it needs from the ssh port (dos/).
cat > "$tmp/dj-build.sh" <<EOF
set -e
if command -v i586-pc-msdosdjgpp-gcc > /dev/null; then p=i586-pc-msdosdjgpp-; else p=; fi
CC=\${p}gcc AR=\${p}ar STRIP=\${p}strip
M=$tmp/mbedtls
MCF="-O2 -march=i586 -I\$M/include -I\$M/library -I$port -DMBEDTLS_USER_CONFIG_FILE=\\"wget_mbedtls_config.h\\""
CF="-O2 -march=i586 -Wall -I$port/dos -I$ssh -I$tmp/Watt-32/inc -DWATT32 -DWATT32_NO_OLDIES"
mkdir -p $tmp/mb
cd $tmp
for f in \$M/library/*.c; do \$CC \$MCF -c "\$f" -o mb/\$(basename "\$f" .c).o; done
\$AR rcs libmbedtls.a mb/*.o
\$CC \$CF \$MCF -c $port/wget.c -o wget.o
\$CC \$CF -c $ssh/dos-sock.c -o dos-sock.o
\$CC \$CF -c $port/dos/wget-dos.c -o wget-dos.o
\$CC -o wget.exe wget.o dos-sock.o wget-dos.o libmbedtls.a
\$STRIP wget.exe
EOF
if [ -n "$docker" ]; then
    docker run --rm -v "$tmp:$tmp:z" -v "$root:$root:z" -u "$(id -u):$(id -g)" "$image" sh "$tmp/dj-build.sh"
else
    sh "$tmp/dj-build.sh"
fi
mkdir -p "$port/bin"
rm -f "$port/bin/WGET.EXE"
if command -v upx > /dev/null; then
    upx -q --best -o "$port/bin/WGET.EXE" "$tmp/wget.exe" > /dev/null
else
    echo "build-wget: no upx: WGET.EXE left uncompressed" >&2
    cp "$tmp/wget.exe" "$port/bin/WGET.EXE"
fi
ls -l "$port/bin/WGET.EXE"
