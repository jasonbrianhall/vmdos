#!/bin/sh
# Build third_party/curl/bin/CURL.EXE: curl for DOS (DJGPP), HTTP(S), FTP(S)
# and FILE, TLS by mbedTLS, on vmdos's own TCP through ssh's socket layer
# (third_party/openssh/dos-sock.c) in place of Watt-32's library
# (third_party/curl/dos/curl-dos.c gives curl the Watt-32 calls it makes).
# Nothing of it is in the kernel. The normal build doesn't need this: the
# binary is committed.
#
# DJGPP, either of:
#   a cross gcc i586-pc-msdosdjgpp-gcc on PATH (or DJGPP=dir with bin/), or
#   Docker: the image djfdyuruiry/djgpp (DJGPP_IMAGE=... for another one),
#           used when there's no cross gcc; DJGPP_DOCKER=1 to prefer it
# and: git, make, a host gcc with -m32 (gcc-multilib), python3; upx if there
#   CURLSRC=dir    an existing checkout of github.com/curl/curl
#   WATTSRC=dir    an existing checkout of github.com/gvanem/Watt-32
#   MBEDTLSSRC=dir an existing checkout of github.com/Mbed-TLS/mbedtls
#   KEEP_TMP=1     keep the build directory
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
port=$root/third_party/curl
wget=$root/third_party/wget
ssh=$root/third_party/openssh
[ -n "$DJGPP" ] && PATH="$DJGPP/bin:$PATH"
curl_tag=curl-8_23_0
watt_rev=3cf6f4e504b46bea2302b8d88b5ff2431396ddc9     # as tools/build-openssh.sh
mbedtls_tag=v3.6.4
image=${DJGPP_IMAGE:-djfdyuruiry/djgpp}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/build-curl.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
[ -n "$KEEP_TMP" ] && { trap - EXIT; echo "build dir: $tmp"; }

# Which DJGPP: the cross compiler, or Docker.
if [ -z "$DJGPP_DOCKER" ] && command -v i586-pc-msdosdjgpp-gcc > /dev/null; then
    docker=
    dj=$(dirname "$(command -v i586-pc-msdosdjgpp-gcc)")/..
    djinc=$dj/i586-pc-msdosdjgpp/sys-include
else
    command -v docker > /dev/null || { echo "build-curl: no i586-pc-msdosdjgpp-gcc and no docker" >&2; exit 1; }
    docker=1
    docker pull -q "$image" > /dev/null
    djinc=$tmp/djinc
    mkdir -p "$djinc"
    d=$(docker run --rm "$image" sh -c 'echo "#include <errno.h>" | gcc -x c -E -M - 2>/dev/null' |
        tr ' \\' '\n\n' | grep '/errno\.h$' | head -1 | xargs dirname)
    [ -n "$d" ] || { echo "build-curl: can't find DJGPP's errno.h in $image" >&2; exit 1; }
    docker run --rm "$image" tar -C "$d" -cf - . | tar -x -C "$djinc"
fi

src() {     # name tag/rev url local: a checkout in $tmp/name
    if [ -n "$4" ]; then
        git -C "$4" archive --prefix="$1/" "$2" | tar -x -C "$tmp"
    else
        git -c advice.detachedHead=false clone -q "$3" "$tmp/$1"
        git -c advice.detachedHead=false -C "$tmp/$1" checkout -q "$2"
    fi
}

# Watt-32: its socket headers and errno list for DJGPP, as for ssh
src Watt-32 $watt_rev https://github.com/gvanem/Watt-32 "$WATTSRC"
W=$tmp/Watt-32
python3 "$ssh/djstrerr.py" "$djinc/errno.h" > "$tmp/djstrerr.h"
gcc -m32 -w -DWATT32_DJGPP_MINGW --include "$djinc/errno.h" -D_ERRNO_H -D_BITS_ERRNO_H \
    --include "$djinc/sys/version.h" -include "$tmp/djstrerr.h" -idirafter "$W/inc" \
    -o "$tmp/dj_err" "$W/util/errnos.c"
(cd "$W/src" && sh configur.sh djgpp > /dev/null && "$tmp/dj_err" -e > ../inc/sys/djgpp.err)

# mbedTLS, curl
if [ -n "$MBEDTLSSRC" ]; then
    git -C "$MBEDTLSSRC" archive --prefix=mbedtls/ $mbedtls_tag | tar -x -C "$tmp"
else
    git -c advice.detachedHead=false clone -q --depth 1 --branch $mbedtls_tag https://github.com/Mbed-TLS/mbedtls "$tmp/mbedtls"
fi
if [ -n "$CURLSRC" ]; then
    git -C "$CURLSRC" archive --prefix=curl/ $curl_tag | tar -x -C "$tmp"
else
    git -c advice.detachedHead=false clone -q --depth 1 --branch $curl_tag https://github.com/curl/curl "$tmp/curl"
fi
C=$tmp/curl
# curl's own lists of its sources
lib_c=$(printf 'include Makefile.inc\nall:\n\t@echo $(CSOURCES)\n' | make -s -C "$C/lib" -f -)
src_c=$(printf 'include Makefile.inc\nall:\n\t@echo $(CURL_CFILES)\n' | make -s -C "$C/src" -f -)

# The DJGPP part, in one go (in the image, or here)
cat > "$tmp/dj-build.sh" <<EOF
set -e
if command -v i586-pc-msdosdjgpp-gcc > /dev/null; then p=i586-pc-msdosdjgpp-; else p=; fi
CC=\${p}gcc AR=\${p}ar STRIP=\${p}strip
M=$tmp/mbedtls
DOS="-include $port/dos/curl-dos.h"
MCF="-O2 -march=i586 \$DOS -I\$M/include -I\$M/library -I$wget -I$port -DMBEDTLS_USER_CONFIG_FILE=\\"curl_mbedtls_config.h\\""
NET="-I$wget/dos -I$ssh -I$W/inc -DWATT32 -DWATT32_NO_OLDIES"
CCF="-O2 -march=i586 \$DOS -I$C/include -I$port -I$C/lib -I\$M/include \$NET -DHAVE_CONFIG_H -DCURL_STATICLIB -DMBEDTLS_USER_CONFIG_FILE=\\"curl_mbedtls_config.h\\" -I$wget"
cd $tmp
mkdir -p o/mb o/lib o/src
for f in \$M/library/*.c; do \$CC \$MCF -c "\$f" -o o/mb/\$(basename "\$f" .c).o; done
\$AR rcs libmbedtls.a o/mb/*.o
for f in $lib_c; do
    mkdir -p o/lib/\$(dirname \$f)
    \$CC \$CCF -DBUILDING_LIBCURL -c $C/lib/\$f -o o/lib/\${f%.c}.o
done
\$AR rcs libcurl.a \$(find o/lib -name '*.o')
for f in $src_c; do
    mkdir -p o/src/\$(dirname \$f)
    \$CC \$CCF -I$C/src -c $C/src/\$f -o o/src/\${f%.c}.o
done
\$CC \$CCF -c $port/dos/curl-dos.c -o curl-dos.o
\$CC \$MCF -c $port/mbedtls-alt.c -o mbedtls-alt.o
\$CC -O2 -march=i586 \$DOS \$NET -c $ssh/dos-sock.c -o dos-sock.o
\$CC -O2 -march=i586 \$DOS \$NET -c $wget/dos/wget-dos.c -o dos-stubs.o
\$CC -o curl.exe \$(find o/src -name '*.o') curl-dos.o dos-sock.o dos-stubs.o mbedtls-alt.o libcurl.a libmbedtls.a
\$STRIP curl.exe
EOF
if [ -n "$docker" ]; then
    docker run --rm -v "$tmp:$tmp:z" -v "$root:$root:z" -u "$(id -u):$(id -g)" "$image" sh "$tmp/dj-build.sh"
else
    sh "$tmp/dj-build.sh"
fi
mkdir -p "$port/bin"
rm -f "$port/bin/CURL.EXE"
if command -v upx > /dev/null; then
    upx -q --best -o "$port/bin/CURL.EXE" "$tmp/curl.exe" > /dev/null
else
    echo "build-curl: no upx: CURL.EXE left uncompressed" >&2
    cp "$tmp/curl.exe" "$port/bin/CURL.EXE"
fi
ls -l "$port/bin/CURL.EXE"
