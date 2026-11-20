#!/bin/sh
# Builds real 3dfx Glide for the Voodoo 3 (h3) and its test programs, for
# DOS, into build/glide/extra (copied to C:\ by glidetest.sh):
#   - Glide 2.x with DJGPP: TEST00-TEST29.EXE, its own DPMI client (vmdos's DPMI)
#   - Glide 3.x with Open Watcom for DOS/4GW: W\W00.EXE ... (the extender
#     most DOS games use), WINIT.EXE, and AINIT.EXE for DOS/32A
# From github.com/sezero/glide (3dfx's open source Glide, Daniel Borca's DOS
# ports), pinned below. Fetches into build/glide/ on first use: the Glide
# source, a DJGPP cross compiler (andrewwutw/build-djgpp) and Open Watcom
# (tools/fetch-watcom.sh, into .watcom).
#   tests/voodoo/glide/build.sh            (from the top of the tree)
# Needs: git, curl, gcc-multilib (32-bit host tools), libfl2 (DJGPP's
# binutils link against libfl.so.2), make.
set -e
top=$(pwd)
here=$top/tests/voodoo/glide
b=$top/build/glide
GLIDE_REPO=https://github.com/sezero/glide
GLIDE_REV=2f226f0f9225ce8ee83e6a4a7042981e719d19ee
DJGPP_URL=https://github.com/andrewwutw/build-djgpp/releases/download/v3.4/djgpp-linux64-gcc1220.tar.bz2
mkdir -p "$b"

# ---- fetch ----
if [ ! -d "$b/glide/.git" ]; then
    git clone -q "$GLIDE_REPO" "$b/glide"
fi
git -C "$b/glide" checkout -q "$GLIDE_REV"
if [ ! -x "$b/djgpp/djgpp/bin/i586-pc-msdosdjgpp-gcc" ]; then
    mkdir -p "$b/djgpp"
    curl -fsSL -o "$b/djgpp.tar.bz2" "$DJGPP_URL"
    tar xjf "$b/djgpp.tar.bz2" -C "$b/djgpp"
    rm -f "$b/djgpp.tar.bz2"
fi
[ -d "$top/.watcom/binl64" ] || [ -d "$top/.watcom/binl" ] || sh "$top/tools/fetch-watcom.sh" "$top/.watcom"

DJ=$b/djgpp/djgpp
export DXE_LD_LIBRARY_PATH=$DJ/i586-pc-msdosdjgpp/lib
export WATCOM=$top/.watcom
export INCLUDE=$WATCOM/h
WBIN=$(ls -d "$WATCOM/binl64" "$WATCOM/binl" 2>/dev/null | head -1)
export PATH=$DJ/bin:$WBIN:$PATH
if ! i586-pc-msdosdjgpp-ar --version >/dev/null 2>&1; then
    echo "DJGPP's binutils don't run: install libfl2 (apt install libfl2)" >&2; exit 1
fi

out=$b/extra
rm -rf "$out"; mkdir -p "$out/W"

# ---- Glide 2.x, h3, DJGPP ----
g2=$b/glide/glide2x
ln -sfn ../swlibs "$g2/swlibs"
make -s -C "$g2" -f Makefile.DJ FX_GLIDE_HW=h3 USE_X86=1 USE_3DNOW=1 \
    CC=i586-pc-msdosdjgpp-gcc AR=i586-pc-msdosdjgpp-ar HOST_CC="gcc -m32" >"$b/glide2x.log" 2>&1 ||
    { tail -20 "$b/glide2x.log"; exit 1; }
[ -f "$g2/h3/lib/libglide2x.a" ] || { tail -20 "$b/glide2x.log"; echo "no libglide2x.a" >&2; exit 1; }
t2=$g2/h3/glide/tests
for i in 00 01 02 03 04 05 06 07 08 09 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29; do
    make -s -C "$t2" -f Makefile.DJ CC=i586-pc-msdosdjgpp-gcc test$i.exe >>"$b/glide2x.log" 2>&1 ||
        { tail -20 "$b/glide2x.log"; exit 1; }
    cp "$t2/test$i.exe" "$out/TEST$i.EXE"
done
cp "$t2"/*.3df "$out/"

# ---- Glide 3.x, h3, Open Watcom ----
g3=$b/glide/glide3x
ln -sfn ../swlibs "$g3/swlibs"
# newer wlib takes the library name without -o
sed -i 's/\$(AR) \$(ARFLAGS) -o \$(call FIXPATH,\$@) @wlib.lbc/$(AR) $(ARFLAGS) $(call FIXPATH,$@) @wlib.lbc/' \
    "$g3/h3/glide3/src/Makefile.wat"
make -s -C "$g3" -f Makefile.wat FX_GLIDE_HW=h3 USE_X86=1 HOST_CC="gcc -m32" >"$b/glide3x.log" 2>&1 ||
    { tail -20 "$b/glide3x.log"; exit 1; }
lib3=$g3/h3/lib/glide3x.lib
[ -f "$lib3" ] || { tail -20 "$b/glide3x.log"; echo "no glide3x.lib" >&2; exit 1; }
t3=$g3/h3/glide3/tests
for i in 00 04 05 11 17 20 25 26; do
    make -s -C "$t3" -f Makefile.wat LDFLAGS="-zq -k16384 -l=dos4g" test$i.exe >>"$b/glide3x.log" 2>&1 ||
        { tail -20 "$b/glide3x.log"; exit 1; }
    cp "$t3/test$i.exe" "$out/W/W$i.EXE"
done
cp "$t3"/*.3df "$out/W/"
WF="-bt=dos -zq -ox -5s -D__DOS__ -DH3 -D__DOS32__ -I$g3/h3/glide3/src -I$g3/h3/incsrc -I$g3/swlibs/fxmisc"
(cd "$b" && wcl386 $WF -l=dos4g -fe=WINIT.EXE "$here/winit.c" "$lib3" >/dev/null &&
            wcl386 $WF -l=dos32a -fe=AINIT.EXE "$here/winit.c" "$lib3" >/dev/null 2>&1; rm -f winit.o winit.obj)
cp "$b/WINIT.EXE" "$b/AINIT.EXE" "$out/W/"
cp "$WATCOM/binw/dos4gw.exe" "$out/W/DOS4GW.EXE"
cp "$WATCOM/binw/dos32a.exe" "$out/W/DOS32A.EXE"
echo "Glide tests built in $out"
