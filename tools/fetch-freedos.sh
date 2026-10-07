#!/bin/sh
# Fetch the FreeDOS kernel (KERNEL.SYS) and FreeCOM (COMMAND.COM) binaries into
# freedos/. They come from Rufus's repository (res/freedos), which ships the
# FreeDOS release builds; the sources are at github.com/FDOS/kernel and
# github.com/FDOS/freecom (GPL). Any FreeDOS 1.3/1.4 KERNEL.SYS + COMMAND.COM
# copied into freedos/ by hand work just as well.
set -e
dest=${1:-freedos}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git clone -q --depth 1 --filter=blob:none --sparse https://github.com/pbatard/rufus "$tmp/rufus"
git -C "$tmp/rufus" sparse-checkout set res/freedos
mkdir -p "$dest"
cp "$tmp/rufus/res/freedos/KERNEL.SYS" "$tmp/rufus/res/freedos/COMMAND.COM" "$dest/"
echo "FreeDOS kernel and COMMAND.COM are in $dest/"
